(() => {
  'use strict';

  const dbToGain = (db) => 10 ** (db / 20);
  const gainToDb = (gain) => gain > 0 ? 20 * Math.log10(gain) : -Infinity;

  function biquadCoefficients(type, sampleRate, frequency, q, gainDb = 0) {
    const omega = 2 * Math.PI * frequency / sampleRate;
    const cosine = Math.cos(omega);
    const sine = Math.sin(omega);
    const alpha = sine / (2 * q);
    let b0;
    let b1;
    let b2;
    let a0;
    let a1;
    let a2;

    if (type === 'highpass') {
      b0 = (1 + cosine) / 2;
      b1 = -(1 + cosine);
      b2 = (1 + cosine) / 2;
      a0 = 1 + alpha;
      a1 = -2 * cosine;
      a2 = 1 - alpha;
    } else {
      const amplitude = 10 ** (gainDb / 40);
      const rootAmplitude = Math.sqrt(amplitude);
      b0 = amplitude * ((amplitude + 1) + (amplitude - 1) * cosine + 2 * rootAmplitude * alpha);
      b1 = -2 * amplitude * ((amplitude - 1) + (amplitude + 1) * cosine);
      b2 = amplitude * ((amplitude + 1) + (amplitude - 1) * cosine - 2 * rootAmplitude * alpha);
      a0 = (amplitude + 1) - (amplitude - 1) * cosine + 2 * rootAmplitude * alpha;
      a1 = 2 * ((amplitude - 1) - (amplitude + 1) * cosine);
      a2 = (amplitude + 1) - (amplitude - 1) * cosine - 2 * rootAmplitude * alpha;
    }

    return {
      b0: b0 / a0,
      b1: b1 / a0,
      b2: b2 / a0,
      a1: a1 / a0,
      a2: a2 / a0
    };
  }

  function createFilterState(coefficients) {
    return { ...coefficients, z1: 0, z2: 0 };
  }

  function filterSample(sample, filter) {
    const output = filter.b0 * sample + filter.z1;
    filter.z1 = filter.b1 * sample - filter.a1 * output + filter.z2;
    filter.z2 = filter.b2 * sample - filter.a2 * output;
    return output;
  }

  function measurePeaks(buffer) {
    let samplePeak = 0;
    let truePeak = 0;
    for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
      const samples = buffer.getChannelData(channelIndex);
      for (let frame = 0; frame < samples.length; frame += 1) {
        const previous = samples[Math.max(0, frame - 1)];
        const current = samples[frame];
        const next = samples[Math.min(samples.length - 1, frame + 1)];
        const afterNext = samples[Math.min(samples.length - 1, frame + 2)];
        samplePeak = Math.max(samplePeak, Math.abs(current));
        truePeak = Math.max(truePeak, Math.abs(current));

        const a = (-previous + 3 * current - 3 * next + afterNext) / 2;
        const b = previous - 2.5 * current + 2 * next - 0.5 * afterNext;
        const c = (next - previous) / 2;
        for (const fraction of [0.25, 0.5, 0.75]) {
          const interpolated = ((a * fraction + b) * fraction + c) * fraction + current;
          truePeak = Math.max(truePeak, Math.abs(interpolated));
        }
      }
    }
    return { samplePeak, truePeak };
  }

  function measureLoudness(buffer) {
    const sampleRate = buffer.sampleRate;
    const channelCount = buffer.numberOfChannels;
    const { samplePeak, truePeak } = measurePeaks(buffer);
    if (!buffer.length || !channelCount) {
      return { integratedLufs: -Infinity, samplePeak: 0, truePeak: 0, truePeakDb: -Infinity };
    }

    const shelf = biquadCoefficients('highshelf', sampleRate, 1681.974, 0.7072, 3.9998);
    const highpass = biquadCoefficients('highpass', sampleRate, 38.1355, 0.5003);
    const shelfFilters = Array.from({ length: channelCount }, () => createFilterState(shelf));
    const highpassFilters = Array.from({ length: channelCount }, () => createFilterState(highpass));
    const chunkSize = Math.max(1, Math.round(sampleRate / 10));
    const chunkPowers = [];

    for (let start = 0; start < buffer.length; start += chunkSize) {
      const end = Math.min(start + chunkSize, buffer.length);
      let power = 0;
      for (let channelIndex = 0; channelIndex < channelCount; channelIndex += 1) {
        const samples = buffer.getChannelData(channelIndex);
        const shelfFilter = shelfFilters[channelIndex];
        const highpassFilter = highpassFilters[channelIndex];
        for (let frame = start; frame < end; frame += 1) {
          const weighted = filterSample(filterSample(samples[frame], shelfFilter), highpassFilter);
          power += weighted * weighted;
        }
      }
      chunkPowers.push(power / (end - start));
    }

    const chunksPerWindow = Math.max(1, Math.round(sampleRate * 0.4 / chunkSize));
    const windowPowers = [];
    if (chunkPowers.length < chunksPerWindow) {
      windowPowers.push(chunkPowers.reduce((sum, power) => sum + power, 0) / chunkPowers.length);
    } else {
      let rollingPower = 0;
      for (let index = 0; index < chunkPowers.length; index += 1) {
        rollingPower += chunkPowers[index];
        if (index >= chunksPerWindow) rollingPower -= chunkPowers[index - chunksPerWindow];
        if (index >= chunksPerWindow - 1) {
          windowPowers.push(rollingPower / chunksPerWindow);
        }
      }
    }

    const toLufs = (power) => power > 0 ? -0.691 + 10 * Math.log10(power) : -Infinity;
    const absoluteGated = windowPowers.filter((power) => toLufs(power) >= -70);
    if (!absoluteGated.length) {
      return {
        integratedLufs: -Infinity,
        samplePeak,
        truePeak,
        truePeakDb: gainToDb(truePeak)
      };
    }

    const preliminaryPower = absoluteGated.reduce((sum, power) => sum + power, 0) / absoluteGated.length;
    const relativeGate = toLufs(preliminaryPower) - 10;
    const gated = absoluteGated.filter((power) => toLufs(power) >= relativeGate);
    const integratedPower = gated.reduce((sum, power) => sum + power, 0) / gated.length;
    return {
      integratedLufs: toLufs(integratedPower),
      samplePeak,
      truePeak,
      truePeakDb: gainToDb(truePeak)
    };
  }

  function limitPeaks(buffer, ceilingDb = -1.35, lookaheadMs = 5, releaseMs = 80) {
    const blockSize = Math.max(1, Math.round(buffer.sampleRate / 1000));
    const blockCount = Math.ceil(buffer.length / blockSize);
    const lookaheadBlocks = Math.max(1, Math.ceil(lookaheadMs));
    const ceiling = dbToGain(ceilingDb);
    const peaks = new Float32Array(blockCount);
    let minimumGain = 1;

    for (let block = 0; block < blockCount; block += 1) {
      const start = block * blockSize;
      const end = Math.min(start + blockSize, buffer.length);
      let peak = 0;
      for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
        const samples = buffer.getChannelData(channelIndex);
        for (let frame = start; frame < end; frame += 1) {
          peak = Math.max(peak, Math.abs(samples[frame]));
        }
      }
      peaks[block] = peak;
    }

    const release = Math.exp(-blockSize / (buffer.sampleRate * releaseMs / 1000));
    let gain = 1;
    for (let block = 0; block < blockCount; block += 1) {
      let lookaheadPeak = 0;
      const lookaheadEnd = Math.min(blockCount, block + lookaheadBlocks + 1);
      for (let future = block; future < lookaheadEnd; future += 1) {
        lookaheadPeak = Math.max(lookaheadPeak, peaks[future]);
      }
      const targetGain = lookaheadPeak > ceiling ? ceiling / lookaheadPeak : 1;
      gain = targetGain < gain ? targetGain : 1 - (1 - gain) * release;
      minimumGain = Math.min(minimumGain, gain);
      const start = block * blockSize;
      const end = Math.min(start + blockSize, buffer.length);
      for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
        const samples = buffer.getChannelData(channelIndex);
        for (let frame = start; frame < end; frame += 1) {
          samples[frame] *= gain;
        }
      }
    }

    return { maximumReductionDb: gainToDb(minimumGain) };
  }

  function applyOutputLevel(buffer, targetLufs = -9, targetTruePeakDb = -1) {
    const before = measureLoudness(buffer);
    if (!Number.isFinite(before.integratedLufs) || before.truePeak === 0) {
      throw new Error('Der gerenderte Master enthält kein messbares Audiosignal.');
    }

    const gainDb = Math.max(-36, Math.min(36, targetLufs - before.integratedLufs));
    const gain = dbToGain(gainDb);
    for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
      const samples = buffer.getChannelData(channelIndex);
      for (let frame = 0; frame < samples.length; frame += 1) {
        samples[frame] *= gain;
      }
    }

    const limiter = limitPeaks(buffer);
    let after = measureLoudness(buffer);
    const peakCorrectionDb = Math.min(0, targetTruePeakDb - after.truePeakDb);
    if (peakCorrectionDb < 0) {
      const correction = dbToGain(peakCorrectionDb);
      for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
        const samples = buffer.getChannelData(channelIndex);
        for (let frame = 0; frame < samples.length; frame += 1) {
          samples[frame] *= correction;
        }
      }
      after = measureLoudness(buffer);
    }

    return {
      ...after,
      gainDb: gainDb + peakCorrectionDb,
      limiterReductionDb: limiter.maximumReductionDb
    };
  }

  function analyzeResonances(buffer, startFrame = 0, endFrame = buffer.length) {
    const start = Math.max(0, Math.floor(startFrame));
    const end = Math.min(buffer.length, Math.max(start + 1, Math.floor(endFrame)));
    const downsample = Math.max(1, Math.floor(buffer.sampleRate / 22050));
    const sampleRate = buffer.sampleRate / downsample;
    const windowSize = Math.min(Math.floor(sampleRate * 0.2), Math.ceil((end - start) / downsample));
    const totalSamples = Math.ceil((end - start) / downsample);
    const windowCount = Math.min(8, Math.max(1, Math.floor(totalSamples / windowSize)));
    const maximumFrequency = Math.min(10000, sampleRate * 0.45);
    if (maximumFrequency <= 2200) return [];
    const frequencies = Array.from({ length: 64 }, (_, index) => (
      2200 * (maximumFrequency / 2200) ** (index / 63)
    ));
    const powers = new Float64Array(frequencies.length);
    const prominenceCount = new Uint8Array(frequencies.length);

    for (let window = 0; window < windowCount; window += 1) {
      const offset = Math.round((totalSamples - windowSize) * window / Math.max(1, windowCount - 1));
      const firstFrame = start + offset * downsample;
      const spectrum = new Float64Array(frequencies.length);
      for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
        const samples = buffer.getChannelData(channelIndex);
        for (let frequencyIndex = 0; frequencyIndex < frequencies.length; frequencyIndex += 1) {
          const omega = 2 * Math.PI * frequencies[frequencyIndex] / sampleRate;
          const coefficient = 2 * Math.cos(omega);
          let previous = 0;
          let previousPrevious = 0;
          for (let index = 0; index < windowSize; index += 1) {
            const frame = Math.min(end - 1, firstFrame + index * downsample);
            const windowWeight = 0.5 - 0.5 * Math.cos(2 * Math.PI * index / Math.max(1, windowSize - 1));
            const current = samples[frame] * windowWeight + coefficient * previous - previousPrevious;
            previousPrevious = previous;
            previous = current;
          }
          const power = Math.max(0, previous * previous + previousPrevious * previousPrevious - coefficient * previous * previousPrevious);
          spectrum[frequencyIndex] += power / buffer.numberOfChannels;
        }
      }

      for (let index = 2; index < frequencies.length - 2; index += 1) {
        const neighborhood = (spectrum[index - 2] + spectrum[index - 1] + spectrum[index + 1] + spectrum[index + 2]) / 4;
        if (spectrum[index] > neighborhood * 10 ** (5 / 10)) prominenceCount[index] += 1;
        powers[index] += spectrum[index];
      }
    }

    const candidates = [];
    for (let index = 2; index < frequencies.length - 2; index += 1) {
      if (prominenceCount[index] < Math.ceil(windowCount / 2)) continue;
      const neighbors = (powers[index - 2] + powers[index - 1] + powers[index + 1] + powers[index + 2]) / 4;
      const prominenceDb = gainToDb(Math.sqrt(powers[index] / Math.max(neighbors, Number.EPSILON)));
      if (prominenceDb < 2.5) continue;
      candidates.push({ frequency: frequencies[index], prominenceDb });
    }
    candidates.sort((left, right) => right.prominenceDb - left.prominenceDb);

    const selected = [];
    for (const candidate of candidates) {
      if (selected.every((peak) => Math.abs(Math.log2(candidate.frequency / peak.frequency)) > 1 / 6)) {
        selected.push(candidate);
        if (selected.length === 4) break;
      }
    }
    return selected.sort((left, right) => left.frequency - right.frequency);
  }

  function analyzeMix(buffer, startFrame = 0, endFrame = buffer.length) {
    const start = Math.max(0, Math.min(buffer.length - 1, Math.floor(startFrame)));
    const end = Math.max(start + 1, Math.min(buffer.length, Math.floor(endFrame)));
    const analysisFrames = end - start;
    const windowFrames = Math.min(buffer.sampleRate, analysisFrames);
    const windowCount = Math.min(30, Math.max(1, Math.ceil(analysisFrames / windowFrames)));
    const coarseFrequencies = [140, 2800];
    const detailFrequencies = [90, 300, 1500, 6000];
    const frequencies = [...coarseFrequencies, ...detailFrequencies];
    const coefficients = frequencies.map((frequency) => 1 - Math.exp(-2 * Math.PI * frequency / buffer.sampleRate));
    const bandPower = [0, 0, 0];
    const detailPower = [0, 0, 0, 0, 0];
    let totalPower = 0;
    let peak = 0;
    let count = 0;

    for (let window = 0; window < windowCount; window += 1) {
      const windowStart = start + (windowCount === 1
        ? 0
        : Math.round((analysisFrames - windowFrames) * window / (windowCount - 1)));
      const lowPass = Array.from({ length: buffer.numberOfChannels }, () => new Float64Array(frequencies.length));
      for (let frame = windowStart; frame < windowStart + windowFrames; frame += 1) {
        for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
          const sample = buffer.getChannelData(channelIndex)[frame];
          const state = lowPass[channelIndex];
          for (let filter = 0; filter < frequencies.length; filter += 1) {
            state[filter] += coefficients[filter] * (sample - state[filter]);
          }
          const low = state[0];
          const mid = state[1] - low;
          const high = sample - state[1];
          bandPower[0] += low * low;
          bandPower[1] += mid * mid;
          bandPower[2] += high * high;
          let previous = 0;
          for (let filter = 0; filter < detailFrequencies.length; filter += 1) {
            const value = state[coarseFrequencies.length + filter];
            const band = value - previous;
            detailPower[filter] += band * band;
            previous = value;
          }
          const highDetail = sample - previous;
          detailPower[detailPower.length - 1] += highDetail * highDetail;
          totalPower += sample * sample;
          peak = Math.max(peak, Math.abs(sample));
          count += 1;
        }
      }
    }

    const totalBandPower = bandPower.reduce((sum, value) => sum + value, 0);
    const ratios = totalBandPower > 0
      ? bandPower.map((value) => value / totalBandPower)
      : [0, 0, 0];
    const totalDetailPower = detailPower.reduce((sum, value) => sum + value, 0);
    const detailRatios = totalDetailPower > 0
      ? detailPower.map((value) => value / totalDetailPower)
      : [0, 0, 0, 0, 0];
    const rms = count ? Math.sqrt(totalPower / count) : 0;
    const crestDb = peak > 0 && rms > 0 ? gainToDb(peak) - gainToDb(rms) : 0;
    return {
      bandRatios: ratios,
      eqBandRatios: detailRatios,
      rmsDb: gainToDb(rms),
      peakDb: gainToDb(peak),
      crestDb
    };
  }

  function recommendAdaptiveMastering(features) {
    if (
      !features
      || !Array.isArray(features.bandRatios)
      || features.bandRatios.length !== 3
      || !features.bandRatios.every(Number.isFinite)
      || !Number.isFinite(features.crestDb)
    ) {
      throw new TypeError('Für die automatische Anpassung werden gültige Audio-Messwerte benötigt.');
    }

    const [lowRatio, midRatio, highRatio] = features.bandRatios;
    const eq = [0, 0, 0, 0, 0];
    if (lowRatio > 0.48) {
      eq[0] = -Math.min(1.5, (lowRatio - 0.48) * 8);
      eq[1] = -Math.min(0.8, (lowRatio - 0.48) * 4);
    } else if (lowRatio < 0.2) {
      eq[0] = Math.min(0.6, (0.2 - lowRatio) * 3);
    }
    if (midRatio > 0.62) eq[2] = -Math.min(1, (midRatio - 0.62) * 5);
    if (highRatio < 0.12 && lowRatio < 0.52) {
      eq[4] = Math.min(0.8, (0.12 - highRatio) * 8);
    } else if (highRatio > 0.35) {
      eq[4] = -Math.min(1.2, (highRatio - 0.35) * 6);
    }
    if (Array.isArray(features.eqBandRatios) && features.eqBandRatios.length === 5) {
      const [sub, bass, body, presence, air] = features.eqBandRatios;
      if (sub > 0.36) eq[0] = Math.min(eq[0], -0.5);
      if (bass > 0.31) eq[1] = Math.min(eq[1], -0.5);
      if (body > 0.42) eq[2] = Math.min(eq[2], -0.5);
      if (presence < 0.11 && lowRatio > 0.32 && highRatio < 0.32) eq[3] = 0.5;
      else if (presence > 0.26) eq[3] = -0.5;
      if (air > 0.24) eq[4] = Math.min(eq[4], -0.5);
    }
    const recommendedEq = eq.map((value) => Math.round(value * 2) / 2);

    const intensity = Math.round(Math.max(30, Math.min(64, 42 + (features.crestDb - 10) * 2)));
    const changes = [];
    if (recommendedEq[0] < 0 || recommendedEq[1] < 0) changes.push('Bass kontrolliert');
    else if (recommendedEq[0] > 0) changes.push('Tiefbass behutsam ergänzt');
    if (recommendedEq[2] < 0) changes.push('Mitten sanft entzerrt');
    if (recommendedEq[3] > 0) changes.push('Präsenz behutsam unterstützt');
    else if (recommendedEq[3] < 0) changes.push('Präsenz entschärft');
    if (recommendedEq[4] > 0) changes.push('Höhen behutsam geöffnet');
    else if (recommendedEq[4] < 0) changes.push('Höhen leicht zurückgenommen');
    const summary = `${changes.length ? changes.join(' · ') : 'Tonbalance wirkt ausgewogen'} · Dynamik ${intensity < 38 ? 'bereits dicht' : intensity > 56 ? 'mit viel Spielraum' : 'ausgewogen'}`;

    return { eq: recommendedEq, intensity, summary };
  }

  const api = { measureLoudness, applyOutputLevel, analyzeResonances, analyzeMix, recommendAdaptiveMastering };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  if (typeof window !== 'undefined') window.SonoraAudioEngine = api;
})();
