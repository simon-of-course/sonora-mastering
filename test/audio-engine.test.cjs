const assert = require('node:assert/strict');
const test = require('node:test');
const engine = require('../audio-engine.js');

function createBuffer({ sampleRate = 48000, seconds = 3, channels = 1, sample }) {
  const length = Math.round(sampleRate * seconds);
  const data = Array.from({ length: channels }, (_, channel) => {
    const samples = new Float32Array(length);
    for (let frame = 0; frame < length; frame += 1) {
      samples[frame] = sample(frame, channel, sampleRate);
    }
    return samples;
  });
  return {
    sampleRate,
    length,
    numberOfChannels: channels,
    getChannelData(channel) {
      return data[channel];
    }
  };
}

test('mastering intensity blends processing strength, never dry and phase-shifted wet audio', () => {
  assert.deepEqual(engine.getMixGains(0.35), { dryGain: 0, wetGain: 1 });
  assert.deepEqual(engine.getMixGains(1), { dryGain: 0, wetGain: 1 });
  assert.deepEqual(engine.getMixGains(0), { dryGain: 1, wetGain: 0 });
  assert.deepEqual(engine.getMixGains(0.7, true), { dryGain: 1, wetGain: 0 });
});

test('loudness normalization targets a commercial release level without exceeding the true-peak ceiling', () => {
  const buffer = createBuffer({
    sample: (frame, _channel, sampleRate) => 0.08 * Math.sin(2 * Math.PI * 1000 * frame / sampleRate)
  });
  const result = engine.applyOutputLevel(buffer);
  assert.ok(Math.abs(result.integratedLufs - (-9)) < 0.4, `${result.integratedLufs} LUFS`);
  assert.ok(result.truePeakDb <= -0.95, `${result.truePeakDb} dBTP`);
  assert.ok(result.samplePeak <= 1);
  assert.ok(result.samplePeak > 0.4, `${result.samplePeak} sample peak`);
});

test('lookahead limiting controls a transient after loudness gain is applied', () => {
  const buffer = createBuffer({
    sample: (frame, _channel, sampleRate) => {
      const tone = 0.025 * Math.sin(2 * Math.PI * 997 * frame / sampleRate);
      return frame === Math.round(sampleRate * 1.5) ? 1 : tone;
    }
  });
  const result = engine.applyOutputLevel(buffer);
  assert.ok(result.truePeakDb <= -0.95, `${result.truePeakDb} dBTP`);
  assert.ok(result.limiterReductionDb < 0);
});

test('quiet mastered tracks receive enough level to reach the release target', () => {
  const buffer = createBuffer({
    sample: (frame, _channel, sampleRate) => 0.008 * Math.sin(2 * Math.PI * 1000 * frame / sampleRate)
  });
  const result = engine.applyOutputLevel(buffer);
  assert.ok(Math.abs(result.integratedLufs - (-9)) < 0.4, `${result.integratedLufs} LUFS`);
  assert.ok(result.gainDb > 12);
  assert.ok(result.truePeakDb <= -0.95, `${result.truePeakDb} dBTP`);
});

test('silent material is reported instead of exported as a successful master', () => {
  const buffer = createBuffer({ sample: () => 0 });
  assert.throws(() => engine.applyOutputLevel(buffer), /kein messbares Audiosignal/);
});

test('mix analysis distinguishes low- and high-frequency-dominant material', () => {
  const low = createBuffer({
    seconds: 1,
    sample: (frame, _channel, sampleRate) => 0.5 * Math.sin(2 * Math.PI * 80 * frame / sampleRate)
  });
  const high = createBuffer({
    seconds: 1,
    sample: (frame, _channel, sampleRate) => 0.5 * Math.sin(2 * Math.PI * 8000 * frame / sampleRate)
  });
  const lowAnalysis = engine.analyzeMix(low);
  const highAnalysis = engine.analyzeMix(high);
  assert.ok(lowAnalysis.bandRatios[0] > highAnalysis.bandRatios[0]);
  assert.ok(highAnalysis.bandRatios[2] > lowAnalysis.bandRatios[2]);
});

test('extended mix analysis identifies vocal-presence and air regions', () => {
  const presence = createBuffer({
    seconds: 1,
    sample: (frame, _channel, sampleRate) => 0.5 * Math.sin(2 * Math.PI * 3500 * frame / sampleRate)
  });
  const air = createBuffer({
    seconds: 1,
    sample: (frame, _channel, sampleRate) => 0.5 * Math.sin(2 * Math.PI * 9000 * frame / sampleRate)
  });
  const presenceAnalysis = engine.analyzeMix(presence);
  const airAnalysis = engine.analyzeMix(air);
  assert.equal(presenceAnalysis.eqBandRatios.length, 5);
  assert.equal(presenceAnalysis.eqBandRatios.indexOf(Math.max(...presenceAnalysis.eqBandRatios)), 3);
  assert.equal(airAnalysis.eqBandRatios.indexOf(Math.max(...airAnalysis.eqBandRatios)), 4);
});

test('resonance analysis detects a persistent narrow high-frequency tone', () => {
  const buffer = createBuffer({
    seconds: 2,
    sample: (frame, _channel, sampleRate) => 0.45 * Math.sin(2 * Math.PI * 3500 * frame / sampleRate)
  });
  const resonances = engine.analyzeResonances(buffer);
  assert.ok(resonances.length > 0);
  assert.ok(resonances.some((peak) => Math.abs(peak.frequency - 3500) < 250));
});

test('automatic mastering makes bounded corrections based on measured tonal balance', () => {
  const low = engine.recommendAdaptiveMastering({
    bandRatios: [0.7, 0.25, 0.05],
    crestDb: 12
  });
  const balanced = engine.recommendAdaptiveMastering({
    bandRatios: [0.35, 0.45, 0.2],
    crestDb: 10
  });
  assert.ok(low.eq[0] < 0);
  assert.ok(low.eq.every((value) => value >= -1 && value <= 0));
  assert.ok(low.eq.every((value) => Number.isInteger(value * 2)));
  assert.ok(low.intensity >= 20 && low.intensity <= 42);
  assert.deepEqual(balanced.eq, [0, 0, 0, 0, 0]);
  assert.match(balanced.summary, /ausgewogen/);
});

test('natural automatic EQ only makes restrained cuts, never guesses at presence boosts', () => {
  const recommendation = engine.recommendAdaptiveMastering({
    bandRatios: [0.4, 0.3, 0.3],
    eqBandRatios: [0.1, 0.1, 0.4, 0.05, 0.35],
    crestDb: 10
  });
  assert.equal(recommendation.eq[3], 0);
  assert.equal(recommendation.eq[4], -0.5);
  assert.ok(recommendation.eq.every((value) => value <= 0));
  assert.ok(recommendation.intensity < 42);
  assert.match(recommendation.summary, /Höhen leicht zurückgenommen/);
  assert.doesNotMatch(recommendation.summary, /unterstützt|geöffnet/);

  const sparse = engine.recommendAdaptiveMastering({
    bandRatios: [0.1, 0.85, 0.05],
    eqBandRatios: [0.05, 0.05, 0.75, 0.05, 0.1],
    crestDb: 10
  });
  assert.ok(sparse.eq.every((value) => value <= 0));
});
