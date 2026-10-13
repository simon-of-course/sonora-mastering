(() => {
  'use strict';

  const PRESETS = {
    universal: { label: 'Universal', eq: [0, 0, 0, 0.2, 0.2], thresholds: [-25, -25, -24], ratios: [1.65, 1.7, 1.5], crossovers: [120, 2800], gain: 0.4 },
    fire: { label: 'Fire', eq: [0.3, 0.4, 0.2, 0.9, 0.8], thresholds: [-28, -29, -26], ratios: [1.95, 2.05, 1.75], crossovers: [130, 3000], gain: 0.8 },
    clarity: { label: 'Clarity', eq: [-0.6, -0.3, 0.1, 0.7, 0.9], thresholds: [-25, -26, -27], ratios: [1.6, 1.7, 1.8], crossovers: [140, 3200], gain: 0.5 },
    tape: { label: 'Tape', eq: [0.5, 0.5, -0.3, 0.2, -0.2], thresholds: [-26, -26, -24], ratios: [1.75, 1.8, 1.5], crossovers: [125, 2600], gain: 0.5 },
    natural: { label: 'Natural', eq: [0, 0, 0, 0, 0.1], thresholds: [-23, -23, -22], ratios: [1.4, 1.45, 1.35], crossovers: [110, 2700], gain: 0.2 },
    cinematic: { label: 'Cinematic', eq: [0.8, 0.1, -0.5, 0.4, 0.7], thresholds: [-24, -24, -23], ratios: [1.45, 1.5, 1.4], crossovers: [140, 2500], gain: 0.3 },
    bass: { label: 'Bass', eq: [1.3, 0.8, -0.2, 0, 0], thresholds: [-28, -25, -23], ratios: [2, 1.65, 1.45], crossovers: [150, 2800], gain: 0.55 },
    punch: { label: 'Punch', eq: [0.6, 0.1, -0.2, 0.7, 0.2], thresholds: [-27, -29, -25], ratios: [1.85, 2, 1.65], crossovers: [135, 2600], gain: 0.7 },
    warm: { label: 'Warm', eq: [0.8, 0.5, -0.2, -0.3, 0.1], thresholds: [-25, -26, -24], ratios: [1.7, 1.75, 1.5], crossovers: [120, 2500], gain: 0.45 },
    open: { label: 'Open', eq: [-0.3, -0.2, 0.1, 0.5, 0.8], thresholds: [-23, -24, -24], ratios: [1.45, 1.55, 1.5], crossovers: [120, 3100], gain: 0.3 },
    vocal: { label: 'Vocal', eq: [-0.5, -0.4, 0.7, 0.7, 0.3], thresholds: [-24, -27, -26], ratios: [1.5, 1.85, 1.75], crossovers: [150, 3200], gain: 0.4 },
    lowend: { label: 'Low-end control', eq: [0.4, -0.3, 0, 0, 0], thresholds: [-30, -25, -24], ratios: [2.2, 1.65, 1.5], crossovers: [160, 2600], gain: 0.4 }
  };
  const FREQUENCIES = [60, 250, 1000, 4000, 12000];
  const EQ_Q = [0.75, 0.85, 0.7, 0.8, 0.7];
  const $ = (selector) => document.querySelector(selector);
  const $$ = (selector) => [...document.querySelectorAll(selector)];

  const fileInput = $('#file-input');
  const uploadZone = $('#upload-zone');
  const trackCard = $('#track-card');
  const player = $('#audio-player');
  const playButton = $('#play-button');
  const seek = $('#seek');
  const trimStartSlider = $('#trim-start');
  const trimEndSlider = $('#trim-end');
  const intensitySlider = $('#intensity');
  const eqSliders = $$('.eq-slider');
  const statusMessage = $('#status-message');

  let audioContext;
  let mediaSource;
  let audioNodes;
  let decodedBuffer;
  let waveformPeaks;
  let currentObjectUrl;
  let currentPreset = 'universal';
  let eqValues = [...PRESETS[currentPreset].eq];
  let trimStartValue = 0;
  let trimEndValue = 1000;
  let intensity = 58;
  let bypass = false;
  let isSeeking = false;
  let animationFrame = 0;
  let loadSequence = 0;
  const MIN_TRIM_SECONDS = 0.05;

  function dbToGain(db) {
    return Math.pow(10, db / 20);
  }

  function formatTime(seconds) {
    if (!Number.isFinite(seconds)) return '0:00';
    const minutes = Math.floor(seconds / 60);
    const remainder = Math.floor(seconds % 60).toString().padStart(2, '0');
    return `${minutes}:${remainder}`;
  }

  function setStatus(message = '') {
    statusMessage.textContent = message;
  }

  function setColorTheme(theme, savePreference = false) {
    const dark = theme === 'dark';
    document.documentElement.dataset.theme = dark ? 'dark' : 'light';
    const toggle = $('#theme-toggle');
    const label = dark ? 'Helles Farbschema aktivieren' : 'Dunkles Farbschema aktivieren';
    toggle.setAttribute('aria-label', label);
    toggle.title = label;
    $('meta[name="theme-color"]').content = dark ? '#171619' : '#f5f4f1';
    if (savePreference) {
      try {
        localStorage.setItem('sonora-theme', dark ? 'dark' : 'light');
      } catch (error) {
        console.warn('Farbschema kann nicht gespeichert werden:', error);
      }
    }
    drawEqGraph();
  }

  function getTrimFrameRange() {
    if (!decodedBuffer) return { startFrame: 0, endFrame: 0 };
    const minimumFrames = Math.min(
      decodedBuffer.length,
      Math.max(1, Math.round(decodedBuffer.sampleRate * MIN_TRIM_SECONDS))
    );
    const startFrame = Math.min(
      decodedBuffer.length - minimumFrames,
      Math.round((trimStartValue / 1000) * decodedBuffer.length)
    );
    const endFrame = Math.max(
      startFrame + minimumFrames,
      Math.min(decodedBuffer.length, Math.round((trimEndValue / 1000) * decodedBuffer.length))
    );
    return { startFrame, endFrame };
  }

  function updateTrimControls() {
    if (!decodedBuffer) return;
    const minimumFrames = Math.min(
      decodedBuffer.length,
      Math.max(1, Math.round(decodedBuffer.sampleRate * MIN_TRIM_SECONDS))
    );
    const minSteps = Math.min(1000, Math.max(1, Math.ceil((minimumFrames / decodedBuffer.length) * 1000)));
    trimStartSlider.max = String(1000 - minSteps);
    trimEndSlider.min = String(minSteps);
    trimStartValue = Math.min(trimStartValue, Number(trimStartSlider.max));
    trimEndValue = Math.max(trimEndValue, Number(trimEndSlider.min));
    if (trimEndValue - trimStartValue < minSteps) {
      trimEndValue = Math.min(1000, trimStartValue + minSteps);
      trimStartValue = Math.max(0, trimEndValue - minSteps);
    }
    trimStartSlider.value = String(trimStartValue);
    trimEndSlider.value = String(trimEndValue);
    const { startFrame, endFrame } = getTrimFrameRange();
    const startSeconds = startFrame / decodedBuffer.sampleRate;
    const endSeconds = endFrame / decodedBuffer.sampleRate;
    $('#trim-start-time').textContent = formatTime(startSeconds);
    $('#trim-end-time').textContent = formatTime(endSeconds);
    const wrap = $('.wave-wrap');
    wrap.style.setProperty('--trim-start', `${(startFrame / decodedBuffer.length) * 100}%`);
    wrap.style.setProperty('--trim-end', `${(endFrame / decodedBuffer.length) * 100}%`);
    trimStartSlider.style.setProperty('--fill', `${trimStartValue}%`);
    trimEndSlider.style.setProperty('--fill', `${trimEndValue}%`);
    $('#reset-trim').disabled = trimStartValue === 0 && trimEndValue === 1000;
    if (Number.isFinite(player.duration) && player.currentTime < startSeconds) {
      player.currentTime = startSeconds;
    } else if (Number.isFinite(player.duration) && player.currentTime >= endSeconds) {
      if (!player.paused) player.pause();
      player.currentTime = startSeconds;
    }
    updateTrackProgress();
    drawWaveform();
  }

  function setTrimValue(which, inputValue) {
    if (!decodedBuffer) return;
    const minimumFrames = Math.min(
      decodedBuffer.length,
      Math.max(1, Math.round(decodedBuffer.sampleRate * MIN_TRIM_SECONDS))
    );
    const gap = Math.min(1000, Math.max(1, Math.ceil((minimumFrames / decodedBuffer.length) * 1000)));
    if (which === 'start') {
      trimStartValue = Math.max(0, Math.min(Number(inputValue), trimEndValue - gap));
    } else {
      trimEndValue = Math.max(trimStartValue + gap, Math.min(1000, Number(inputValue)));
    }
    updateTrimControls();
  }

  function getThemeColor(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  function getAudioContext() {
    if (!audioContext) {
      const Context = window.AudioContext || window.webkitAudioContext;
      if (!Context) throw new Error('Dieser Browser unterstützt keine Web Audio API.');
      audioContext = new Context();
    }
    return audioContext;
  }

  function createAudioGraph() {
    const context = getAudioContext();
    if (mediaSource) return;

    mediaSource = context.createMediaElementSource(player);
    const dryGain = context.createGain();
    const wetGain = context.createGain();
    mediaSource.connect(dryGain);
    const chain = createMasterChain(context, mediaSource);
    const analyser = context.createAnalyser();
    analyser.fftSize = 2048;
    analyser.smoothingTimeConstant = 0.82;
    const channelSplitter = context.createChannelSplitter(2);
    const leftMeter = context.createAnalyser();
    const rightMeter = context.createAnalyser();
    leftMeter.fftSize = 2048;
    rightMeter.fftSize = 2048;

    dryGain.connect(analyser);
    chain.output.connect(wetGain);
    wetGain.connect(analyser);
    analyser.connect(context.destination);
    analyser.connect(channelSplitter);
    channelSplitter.connect(leftMeter, 0);
    channelSplitter.connect(rightMeter, 1);

    audioNodes = {
      dryGain,
      wetGain,
      chain,
      analyser,
      leftMeter,
      rightMeter,
      spectrumData: new Uint8Array(analyser.frequencyBinCount),
      leftSamples: new Float32Array(leftMeter.fftSize),
      rightSamples: new Float32Array(rightMeter.fftSize)
    };
    updateAudioGraph();
    updateMonitorState();
  }

  function updateAudioGraph() {
    if (!audioNodes) return;
    const preset = PRESETS[currentPreset];
    const amount = bypass ? 0 : intensity / 100;
    const now = audioContext.currentTime;
    audioNodes.dryGain.gain.setTargetAtTime(1 - amount, now, 0.025);
    audioNodes.wetGain.gain.setTargetAtTime(amount, now, 0.025);
    configureMasterChain(audioNodes.chain, preset, eqValues, amount, now);
  }

  function updateIntensityUI() {
    $('#intensity-value').innerHTML = `${intensity}<span>%</span>`;
    $('#intensity-description').textContent = intensity < 25
      ? 'Leicht'
      : intensity < 70
        ? 'Moderat'
        : intensity < 90
          ? 'Deutlich'
          : 'Stark';
    intensitySlider.style.setProperty('--fill', `${intensity}%`);
  }

  function updateEqUI() {
    eqSliders.forEach((slider, index) => {
      const value = eqValues[index];
      slider.value = String(value);
      const label = $(`#eq-value-${index}`);
      label.textContent = `${value > 0 ? '+' : ''}${Number(value).toFixed(1)}`;
      label.style.color = value === 0
        ? getThemeColor('--eq-neutral')
        : value > 0
          ? getThemeColor('--eq-up')
          : getThemeColor('--eq-down');
    });
    drawEqGraph();
  }

  function selectPreset(name) {
    if (!PRESETS[name]) return;
    currentPreset = name;
    eqValues = [...PRESETS[name].eq];
    $$('.preset-card').forEach((button) => {
      const selected = button.dataset.preset === name;
      button.classList.toggle('selected', selected);
      button.setAttribute('aria-pressed', String(selected));
    });
    updateEqUI();
    updateAudioGraph();
  }

  function updateMonitorState() {
    const active = Boolean(decodedBuffer);
    $('#monitor-state').textContent = active ? 'SIGNAL BEREIT' : 'WARTET AUF TRACK';
    $('.monitor-state').classList.toggle('active', active);
  }

  function drawWaveform() {
    const canvas = $('#waveform');
    if (!decodedBuffer || !waveformPeaks) return;
    const bounds = canvas.getBoundingClientRect();
    if (!bounds.width || !bounds.height) return;
    const ratio = window.devicePixelRatio || 1;
    const width = Math.round(bounds.width * ratio);
    const height = Math.round(bounds.height * ratio);
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const context = canvas.getContext('2d');
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, bounds.width, bounds.height);

    const bars = Math.max(40, Math.floor(bounds.width / 3));
    const center = bounds.height / 2;
    const progress = player.duration ? player.currentTime / player.duration : 0;
    const activeColor = getThemeColor('--wave-active');
    const inactiveColor = getThemeColor('--wave-inactive');
    const outsideColor = getThemeColor('--quiet');
    for (let i = 0; i < bars; i += 1) {
      const peakIndex = Math.min(waveformPeaks.length - 1, Math.floor((i / bars) * waveformPeaks.length));
      const peak = waveformPeaks[peakIndex];
      const barHeight = Math.max(2, Math.min(center - 3, peak * (center - 3) * 1.65));
      const x = (i / bars) * bounds.width;
      const played = i / bars <= progress;
      const insideSelection = i / bars >= trimStartValue / 1000 && i / bars < trimEndValue / 1000;
      context.fillStyle = insideSelection
        ? played ? activeColor : inactiveColor
        : outsideColor;
      context.fillRect(x, center - barHeight, Math.max(1, bounds.width / bars - 1), barHeight * 2);
    }
    $('#playhead').style.left = `${progress * 100}%`;
  }

  function drawEqGraph() {
    const canvas = $('#eq-graph');
    if (!canvas) return;
    const bounds = canvas.getBoundingClientRect();
    if (!bounds.width || !bounds.height) return;
    const ratio = window.devicePixelRatio || 1;
    canvas.width = Math.round(bounds.width * ratio);
    canvas.height = Math.round(bounds.height * ratio);
    const context = canvas.getContext('2d');
    context.scale(ratio, ratio);
    const width = bounds.width;
    const height = bounds.height;
    context.clearRect(0, 0, width, height);
    const left = 5;
    const right = width - 5;
    const top = 9;
    const bottom = height - 9;
    const zeroY = top + (bottom - top) / 2;
    const minLog = Math.log10(20);
    const maxLog = Math.log10(20000);
    const xAt = (frequency) => left + ((Math.log10(frequency) - minLog) / (maxLog - minLog)) * (right - left);

    context.strokeStyle = getThemeColor('--chart-grid');
    context.lineWidth = 1;
    [0, 1, 2, 3, 4].forEach((tick) => {
      const y = top + ((bottom - top) * tick) / 4;
      context.beginPath();
      context.moveTo(left, y);
      context.lineTo(right, y);
      context.stroke();
    });
    [20, 100, 1000, 10000, 20000].forEach((frequency) => {
      const x = xAt(frequency);
      context.beginPath();
      context.moveTo(x, top);
      context.lineTo(x, bottom);
      context.stroke();
    });

    const activeAmount = bypass ? 0 : intensity / 100;
    const responseAt = (frequency) => eqValues.reduce((sum, gain, index) => {
      const octaveDistance = Math.log2(frequency / FREQUENCIES[index]);
      const spread = index < 2 ? 1.05 : 1.18;
      return sum + gain * activeAmount * Math.exp(-0.5 * Math.pow(octaveDistance / spread, 2));
    }, 0);
    context.beginPath();
    for (let x = left; x <= right; x += 1) {
      const frequency = Math.pow(10, minLog + ((x - left) / (right - left)) * (maxLog - minLog));
      const response = responseAt(frequency);
      const y = zeroY - (response / 16) * (bottom - top);
      if (x === left) context.moveTo(x, y);
      else context.lineTo(x, y);
    }
    context.lineTo(right, zeroY);
    context.lineTo(left, zeroY);
    context.closePath();
    const gradient = context.createLinearGradient(0, top, 0, bottom);
    gradient.addColorStop(0, getThemeColor('--chart-fill'));
    gradient.addColorStop(.5, getThemeColor('--chart-fill-faint'));
    gradient.addColorStop(1, getThemeColor('--chart-fill-low'));
    context.fillStyle = gradient;
    context.fill();
    context.beginPath();
    for (let x = left; x <= right; x += 1) {
      const frequency = Math.pow(10, minLog + ((x - left) / (right - left)) * (maxLog - minLog));
      const y = zeroY - (responseAt(frequency) / 16) * (bottom - top);
      if (x === left) context.moveTo(x, y);
      else context.lineTo(x, y);
    }
    context.strokeStyle = getThemeColor('--accent');
    context.lineWidth = 1.7;
    context.stroke();
  }

  function drawSpectrum() {
    const canvas = $('#spectrum');
    const bounds = canvas.getBoundingClientRect();
    if (!bounds.width || !bounds.height) return;
    const ratio = window.devicePixelRatio || 1;
    const width = Math.round(bounds.width * ratio);
    const height = Math.round(bounds.height * ratio);
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const context = canvas.getContext('2d');
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, bounds.width, bounds.height);
    const columns = 64;
    const gap = 3;
    const barWidth = Math.max(1, (bounds.width - gap * (columns - 1)) / columns);
    const data = audioNodes.spectrumData;
    audioNodes.analyser.getByteFrequencyData(data);
    const gradient = context.createLinearGradient(0, bounds.height, 0, 0);
    gradient.addColorStop(0, getThemeColor('--spectrum-low'));
    gradient.addColorStop(.7, getThemeColor('--spectrum-mid'));
    gradient.addColorStop(1, getThemeColor('--spectrum-high'));
    context.fillStyle = gradient;
    for (let i = 0; i < columns; i += 1) {
      const index = Math.floor(Math.pow(i / columns, 1.8) * (data.length - 1));
      const strength = data[index] / 255;
      const barHeight = Math.max(2, strength * bounds.height * .89);
      const x = i * (barWidth + gap);
      context.globalAlpha = .38 + strength * .62;
      context.fillRect(x, bounds.height - barHeight, barWidth, barHeight);
    }
    context.globalAlpha = 1;

    const { leftSamples, rightSamples } = audioNodes;
    audioNodes.leftMeter.getFloatTimeDomainData(leftSamples);
    audioNodes.rightMeter.getFloatTimeDomainData(rightSamples);
    let leftPeak = 0;
    let rightPeak = 0;
    const channels = decodedBuffer?.numberOfChannels ?? 1;
    for (let i = 0; i < leftSamples.length; i += 1) {
      leftPeak = Math.max(leftPeak, Math.abs(leftSamples[i]));
      rightPeak = Math.max(rightPeak, Math.abs(rightSamples[i]));
    }
    if (channels < 2) rightPeak = leftPeak;
    const toDb = (value) => value < 0.000001 ? -Infinity : 20 * Math.log10(value);
    const peakDb = toDb(Math.max(leftPeak, rightPeak));
    $('#peak-value').textContent = Number.isFinite(peakDb) ? `${peakDb.toFixed(1)}` : '−∞';
    $('#meter-left').style.width = `${Math.min(100, leftPeak * 100)}%`;
    $('#meter-right').style.width = `${Math.min(100, (channels > 1 ? rightPeak : leftPeak) * 100)}%`;
  }

  function animateMeters() {
    cancelAnimationFrame(animationFrame);
    const render = () => {
      drawSpectrum();
      if (!isSeeking && decodedBuffer) drawWaveform();
      animationFrame = requestAnimationFrame(render);
    };
    animationFrame = requestAnimationFrame(render);
  }

  function updateTrackProgress() {
    if (!Number.isFinite(player.duration)) return;
    $('#current-time').textContent = formatTime(player.currentTime);
    $('#duration').textContent = formatTime(player.duration);
    $('#playhead').style.left = `${(player.currentTime / player.duration) * 100}%`;
    if (!decodedBuffer) return;
    if (!isSeeking) {
      const { startFrame, endFrame } = getTrimFrameRange();
      const start = startFrame / decodedBuffer.sampleRate;
      const end = endFrame / decodedBuffer.sampleRate;
      const progress = Math.max(0, Math.min(1, (player.currentTime - start) / (end - start)));
      seek.value = String(Math.round(progress * 1000));
      seek.style.setProperty('--fill', `${progress * 100}%`);
    }
  }

  function formatFileSize(bytes) {
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(0)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  }

  function handleFile(file) {
    if (!file) return;
    const sequence = ++loadSequence;
    setStatus('');
    if (!file.type.startsWith('audio/') && !/\.(wav|mp3|m4a|aac|flac|ogg|aif|aiff)$/i.test(file.name)) {
      setStatus('Diese Datei sieht nicht wie eine Audiodatei aus. Bitte wähle WAV, MP3, FLAC oder M4A.');
      return;
    }

    $('#track-name').textContent = file.name.replace(/\.[^.]+$/, '');
    $('#track-meta').textContent = `${formatFileSize(file.size)} · Audio wird analysiert`;
    uploadZone.hidden = true;
    trackCard.hidden = false;
    $('#track-status').textContent = 'Audio wird vorbereitet ...';
    $('#export-button').disabled = true;
    playButton.disabled = true;
    seek.disabled = true;
    trimStartSlider.disabled = true;
    trimEndSlider.disabled = true;
    $('#reset-trim').disabled = true;
    $('#ab-button').disabled = true;
    player.pause();
    player.classList.remove('playing');
    decodedBuffer = null;
    waveformPeaks = null;
    trimStartValue = 0;
    trimEndValue = 1000;

    try {
      createAudioGraph();
      if (currentObjectUrl) URL.revokeObjectURL(currentObjectUrl);
      currentObjectUrl = URL.createObjectURL(file);
      player.src = currentObjectUrl;
      player.load();
      file.arrayBuffer()
        .then((arrayBuffer) => getAudioContext().decodeAudioData(arrayBuffer))
        .then((buffer) => {
          if (sequence !== loadSequence) return;
          decodedBuffer = buffer;
          waveformPeaks = buildWaveformPeaks(buffer);
          $('#track-meta').textContent = `${formatFileSize(file.size)} · ${buffer.numberOfChannels === 1 ? 'Mono' : 'Stereo'} · ${Math.round(buffer.sampleRate / 1000)} kHz`;
          $('#track-status').textContent = `${formatTime(buffer.duration)} · Bereit für den Feinschliff`;
          player.currentTime = 0;
          playButton.disabled = false;
          seek.disabled = false;
          trimStartSlider.disabled = false;
          trimEndSlider.disabled = false;
          $('#ab-button').disabled = false;
          $('#export-button').disabled = false;
          updateMonitorState();
          updateTrimControls();
          drawWaveform();
          setStatus('');
        })
        .catch((error) => {
          if (sequence !== loadSequence) return;
          console.error('Audio konnte nicht dekodiert werden:', error);
          decodedBuffer = null;
          seek.disabled = true;
          trimStartSlider.disabled = true;
          trimEndSlider.disabled = true;
          $('#track-meta').textContent = `${formatFileSize(file.size)} · Nicht unterstützt`;
          $('#track-status').textContent = 'Datei konnte nicht dekodiert werden';
          setStatus('Diese Audiodatei kann in deinem Browser nicht geöffnet werden. Versuche es mit WAV oder MP3.');
          updateMonitorState();
        });
    } catch (error) {
      console.error('Audio-Engine konnte nicht gestartet werden:', error);
      setStatus(error.message || 'Die Audio-Engine konnte nicht gestartet werden.');
      uploadZone.hidden = false;
      trackCard.hidden = true;
    }
  }

  function buildWaveformPeaks(buffer) {
    const data = buffer.getChannelData(0);
    const bucketCount = Math.min(2048, data.length);
    const samplesPerBucket = Math.ceil(data.length / bucketCount);
    const peaks = new Float32Array(bucketCount);
    for (let bucket = 0; bucket < bucketCount; bucket += 1) {
      const start = bucket * samplesPerBucket;
      const end = Math.min(start + samplesPerBucket, data.length);
      let peak = 0;
      for (let sample = start; sample < end; sample += 1) {
        peak = Math.max(peak, Math.abs(data[sample]));
      }
      peaks[bucket] = peak;
    }
    return peaks;
  }

  function makeOfflineGraph(context, buffer, settings, trimStartSeconds, trimDurationSeconds) {
    const source = context.createBufferSource();
    source.buffer = buffer;
    if (settings.bypass || settings.intensity === 0) {
      source.connect(context.destination);
      return source;
    }
    const chain = createMasterChain(context, source, context.destination);
    configureMasterChain(chain, settings.preset, settings.eq, settings.intensity, 0);
    return source;
  }

  function createMasterChain(context, input, destination = null) {
    const highpass = context.createBiquadFilter();
    highpass.type = 'highpass';
    highpass.frequency.value = 25;
    highpass.Q.value = 0.55;

    const equalizers = FREQUENCIES.map((frequency, index) => {
      const filter = context.createBiquadFilter();
      filter.type = 'peaking';
      filter.frequency.value = frequency;
      filter.Q.value = EQ_Q[index];
      return filter;
    });
    input.connect(highpass);
    let equalized = highpass;
    for (const filter of equalizers) {
      equalized.connect(filter);
      equalized = filter;
    }

    const makePair = (type) => [0, 1].map(() => {
      const filter = context.createBiquadFilter();
      filter.type = type;
      filter.Q.value = Math.SQRT1_2;
      return filter;
    });
    const lowPasses = makePair('lowpass');
    const lowHighPasses = makePair('highpass');
    const midLowPasses = makePair('lowpass');
    const highPasses = makePair('highpass');
    const compressors = [0, 1, 2].map(() => {
      const compressor = context.createDynamicsCompressor();
      compressor.knee.value = 12;
      return compressor;
    });
    const sum = context.createGain();
    const masterGain = context.createGain();
    const limiter = context.createDynamicsCompressor();
    limiter.threshold.value = -1.5;
    limiter.knee.value = 0;
    limiter.ratio.value = 20;
    limiter.attack.value = 0.001;
    limiter.release.value = 0.12;

    let lowOutput = equalized;
    for (const filter of lowPasses) {
      lowOutput.connect(filter);
      lowOutput = filter;
    }
    lowOutput.connect(compressors[0]);

    let midOutput = equalized;
    for (const filter of lowHighPasses) {
      midOutput.connect(filter);
      midOutput = filter;
    }
    for (const filter of midLowPasses) {
      midOutput.connect(filter);
      midOutput = filter;
    }
    midOutput.connect(compressors[1]);

    let highOutput = equalized;
    for (const filter of highPasses) {
      highOutput.connect(filter);
      highOutput = filter;
    }
    highOutput.connect(compressors[2]);

    compressors.forEach((compressor) => compressor.connect(sum));
    sum.connect(masterGain);
    masterGain.connect(limiter);
    const output = destination ?? context.createGain();
    limiter.connect(output);
    return {
      output,
      highpass,
      equalizers,
      crossovers: [lowPasses, lowHighPasses, midLowPasses, highPasses],
      compressors,
      masterGain,
      limiter
    };
  }

  function setAudioParam(parameter, value, time) {
    if (time === null) {
      parameter.value = value;
    } else {
      parameter.setTargetAtTime(value, time, 0.025);
    }
  }

  function configureMasterChain(chain, preset, eq, amount, time = null) {
    chain.equalizers.forEach((filter, index) => {
      setAudioParam(filter.gain, eq[index] * amount, time);
    });

    const [lowCrossover, highCrossover] = preset.crossovers;
    chain.crossovers.forEach((stages, stageIndex) => {
      const frequency = stageIndex < 2 ? lowCrossover : highCrossover;
      stages.forEach((filter) => setAudioParam(filter.frequency, frequency, time));
    });
    chain.compressors.forEach((compressor, index) => {
      setAudioParam(compressor.threshold, -18 + (preset.thresholds[index] + 18) * amount, time);
      setAudioParam(compressor.ratio, 1 + (preset.ratios[index] - 1) * amount, time);
      compressor.attack.value = index === 0 ? 0.025 : index === 1 ? 0.012 : 0.006;
      compressor.release.value = index === 0 ? 0.18 : index === 1 ? 0.12 : 0.085;
    });
    setAudioParam(chain.masterGain.gain, dbToGain(preset.gain * amount), time);
    chain.limiter.threshold.value = -1.5;
  }

  function measureAudio(buffer) {
    const blockSize = Math.max(1, Math.round(buffer.sampleRate * 0.1));
    const channels = Array.from({ length: buffer.numberOfChannels }, (_, index) => buffer.getChannelData(index));
    let totalPower = 0;
    let activeFrames = 0;
    let peak = 0;
    for (let start = 0; start < buffer.length; start += blockSize) {
      const end = Math.min(start + blockSize, buffer.length);
      let blockPower = 0;
      for (const channel of channels) {
        for (let frame = start; frame < end; frame += 1) {
          const sample = channel[frame];
          const absolute = Math.abs(sample);
          if (absolute > peak) peak = absolute;
          blockPower += sample * sample;
        }
      }
      const blockFrames = (end - start) * channels.length;
      const blockRms = Math.sqrt(blockPower / blockFrames);
      if (blockRms >= 0.001) {
        totalPower += blockPower;
        activeFrames += blockFrames;
      }
    }
    return {
      rmsDb: activeFrames ? 10 * Math.log10(totalPower / activeFrames) : -Infinity,
      peak
    };
  }

  function applyOutputLevel(buffer) {
    const input = measureAudio(buffer);
    if (!Number.isFinite(input.rmsDb) || input.peak === 0) {
      throw new Error('Der gerenderte Master enthält kein messbares Audiosignal.');
    }
    const loudnessGain = Math.max(-12, Math.min(12, -16 - input.rmsDb));
    const peakGain = 20 * Math.log10(10 ** (-1 / 20) / input.peak);
    const gainDb = Math.min(loudnessGain, peakGain);
    const gain = dbToGain(gainDb);
    for (let channelIndex = 0; channelIndex < buffer.numberOfChannels; channelIndex += 1) {
      const channel = buffer.getChannelData(channelIndex);
      for (let frame = 0; frame < channel.length; frame += 1) {
        channel[frame] *= gain;
      }
    }
    return { gainDb, ...measureAudio(buffer) };
  }

  function encodeWav(buffer) {
    const channels = buffer.numberOfChannels;
    const frames = buffer.length;
    const bytesPerSample = 3;
    const dataSize = frames * channels * bytesPerSample;
    const arrayBuffer = new ArrayBuffer(44 + dataSize);
    const view = new DataView(arrayBuffer);
    const writeText = (offset, text) => {
      for (let i = 0; i < text.length; i += 1) view.setUint8(offset + i, text.charCodeAt(i));
    };
    writeText(0, 'RIFF');
    view.setUint32(4, 36 + dataSize, true);
    writeText(8, 'WAVE');
    writeText(12, 'fmt ');
    view.setUint32(16, 16, true);
    view.setUint16(20, 1, true);
    view.setUint16(22, channels, true);
    view.setUint32(24, buffer.sampleRate, true);
    view.setUint32(28, buffer.sampleRate * channels * bytesPerSample, true);
    view.setUint16(32, channels * bytesPerSample, true);
    view.setUint16(34, 24, true);
    writeText(36, 'data');
    view.setUint32(40, dataSize, true);

    const channelData = Array.from({ length: channels }, (_, index) => buffer.getChannelData(index));
    let offset = 44;
    for (let frame = 0; frame < frames; frame += 1) {
      for (let channel = 0; channel < channels; channel += 1) {
        const sample = Math.max(-1, Math.min(1, channelData[channel][frame]));
        const value = sample < 0 ? Math.round(sample * 0x800000) : Math.round(sample * 0x7fffff);
        const ditheredValue = Math.max(-0x800000, Math.min(0x7fffff, value + Math.random() - Math.random()));
        view.setUint8(offset, ditheredValue & 0xff);
        view.setUint8(offset + 1, (ditheredValue >> 8) & 0xff);
        view.setUint8(offset + 2, (ditheredValue >> 16) & 0xff);
        offset += bytesPerSample;
      }
    }
    return new Blob([arrayBuffer], { type: 'audio/wav' });
  }

  function safeFilename(name) {
    return name
      .replace(/\.[^.]+$/, '')
      .replace(/[<>:"/\\|?*\u0000-\u001f]/g, '-')
      .trim()
      .slice(0, 100) || 'sonora-master';
  }

  async function exportMaster() {
    if (!decodedBuffer) return;
    const button = $('#export-button');
    const originalContent = button.innerHTML;
    button.disabled = true;
    button.innerHTML = '<span>Mastering läuft ...</span>';
    setStatus('');
    try {
      const trimRange = getTrimFrameRange();
      const context = new OfflineAudioContext(
        decodedBuffer.numberOfChannels,
        trimRange.endFrame - trimRange.startFrame,
        decodedBuffer.sampleRate
      );
      const trimStartSeconds = trimRange.startFrame / decodedBuffer.sampleRate;
      const trimDurationSeconds = (trimRange.endFrame - trimRange.startFrame) / decodedBuffer.sampleRate;
      const source = makeOfflineGraph(context, decodedBuffer, {
        preset: PRESETS[currentPreset],
        eq: [...eqValues],
        intensity: intensity / 100,
        bypass
      }, trimStartSeconds, trimDurationSeconds);
      source.start(0, trimStartSeconds, trimDurationSeconds);
      const isTrimmed = trimRange.startFrame !== 0 || trimRange.endFrame !== decodedBuffer.length;
      const rendered = await context.startRendering();
      const level = bypass || intensity === 0 || !Number.isFinite(measureAudio(rendered).rmsDb)
        ? { gainDb: 0, ...measureAudio(rendered) }
        : applyOutputLevel(rendered);
      const blob = encodeWav(rendered);
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      const trimSuffix = isTrimmed
        ? `_clip_${formatTime(trimStartSeconds).replace(':', '-')}_${formatTime(trimStartSeconds + trimDurationSeconds).replace(':', '-')}`
        : '';
      link.download = `${safeFilename($('#track-name').textContent)}${trimSuffix}_sonora_master_24bit.wav`;
      document.body.append(link);
      link.click();
      link.remove();
      window.setTimeout(() => URL.revokeObjectURL(url), 30000);
      const peakDb = 20 * Math.log10(level.peak);
      const levelMessage = bypass || intensity === 0
        ? 'Dry-Vergleich'
        : !Number.isFinite(level.rmsDb)
          ? 'kein Signal zum Normalisieren'
        : Math.abs(level.gainDb) < 0.05
        ? 'Pegel bereits passend'
        : `Pegel ${level.gainDb > 0 ? '+' : ''}${level.gainDb.toFixed(1)} dB`;
      const measuredPeak = Number.isFinite(peakDb) ? `${peakDb.toFixed(1)} dBFS` : 'kein Signal';
      setStatus(`Fertig: 24-Bit-WAV · ${Math.round(rendered.sampleRate / 1000)} kHz · ${levelMessage} · Peak ${measuredPeak}.`);
    } catch (error) {
      console.error('Master-Export fehlgeschlagen:', error);
      setStatus('Der Export ist fehlgeschlagen. Bei sehr langen Tracks hilft es, eine kürzere Datei oder WAV zu verwenden.');
    } finally {
      button.disabled = !decodedBuffer;
      button.innerHTML = originalContent;
    }
  }

  function togglePlayback() {
    if (!decodedBuffer) return;
    if (player.paused) {
      const { startFrame, endFrame } = getTrimFrameRange();
      const start = startFrame / decodedBuffer.sampleRate;
      const end = endFrame / decodedBuffer.sampleRate;
      if (player.currentTime < start || player.currentTime >= end) player.currentTime = start;
      getAudioContext().resume()
        .then(() => player.play())
        .catch((error) => {
          console.error('Wiedergabe konnte nicht gestartet werden:', error);
          setStatus('Audio-Ausgabe konnte nicht aktiviert werden. Bitte prüfe die Browser-Berechtigungen und versuche es erneut.');
        });
    } else {
      player.pause();
    }
  }

  fileInput.addEventListener('change', () => {
    handleFile(fileInput.files?.[0]);
    fileInput.value = '';
  });
  uploadZone.addEventListener('click', () => fileInput.click());
  $('#replace-track').addEventListener('click', () => fileInput.click());
  $('#theme-toggle').addEventListener('click', () => {
    setColorTheme(document.documentElement.dataset.theme !== 'dark' ? 'dark' : 'light', true);
  });
  trimStartSlider.addEventListener('input', () => setTrimValue('start', trimStartSlider.value));
  trimEndSlider.addEventListener('input', () => setTrimValue('end', trimEndSlider.value));
  $('#reset-trim').addEventListener('click', () => {
    trimStartValue = 0;
    trimEndValue = 1000;
    updateTrimControls();
  });
  ['dragenter', 'dragover'].forEach((eventName) => {
    uploadZone.addEventListener(eventName, (event) => {
      event.preventDefault();
      uploadZone.classList.add('drag-over');
    });
  });
  ['dragleave', 'drop'].forEach((eventName) => {
    uploadZone.addEventListener(eventName, (event) => {
      event.preventDefault();
      uploadZone.classList.remove('drag-over');
    });
  });
  uploadZone.addEventListener('drop', (event) => {
    handleFile(event.dataTransfer?.files?.[0]);
  });
  document.addEventListener('dragover', (event) => event.preventDefault());
  document.addEventListener('drop', (event) => event.preventDefault());

  $$('.preset-card').forEach((button) => {
    button.addEventListener('click', () => selectPreset(button.dataset.preset));
  });
  intensitySlider.addEventListener('input', () => {
    intensity = Number(intensitySlider.value);
    updateIntensityUI();
    updateAudioGraph();
    drawEqGraph();
  });
  eqSliders.forEach((slider) => {
    slider.addEventListener('input', () => {
      eqValues[Number(slider.dataset.band)] = Number(slider.value);
      updateEqUI();
      updateAudioGraph();
    });
  });
  $('#reset-eq').addEventListener('click', () => {
    eqValues = [...PRESETS[currentPreset].eq];
    updateEqUI();
    updateAudioGraph();
  });
  $('#ab-button').addEventListener('click', (event) => {
    bypass = !bypass;
    event.currentTarget.setAttribute('aria-pressed', String(bypass));
    event.currentTarget.innerHTML = bypass
      ? '<span>A</span><i>/</i><span>B</span> <small>DRY AKTIV</small>'
      : '<span>A</span><i>/</i><span>B</span> <small>VERGLEICH</small>';
    updateAudioGraph();
    drawEqGraph();
  });
  playButton.addEventListener('click', togglePlayback);
  player.addEventListener('play', () => {
    playButton.classList.add('playing');
    animateMeters();
  });
  player.addEventListener('pause', () => {
    playButton.classList.remove('playing');
    cancelAnimationFrame(animationFrame);
  });
  player.addEventListener('timeupdate', updateTrackProgress);
  player.addEventListener('loadedmetadata', updateTrackProgress);
  player.addEventListener('ended', updateTrackProgress);
  player.addEventListener('timeupdate', () => {
    if (!decodedBuffer || player.paused) return;
    const { endFrame } = getTrimFrameRange();
    if (player.currentTime >= endFrame / decodedBuffer.sampleRate) {
      player.pause();
      player.currentTime = getTrimFrameRange().startFrame / decodedBuffer.sampleRate;
    }
  });
  seek.addEventListener('input', () => {
    if (!Number.isFinite(player.duration) || !decodedBuffer) return;
    isSeeking = true;
    const progress = Number(seek.value) / 1000;
    const { startFrame, endFrame } = getTrimFrameRange();
    const start = startFrame / decodedBuffer.sampleRate;
    const end = endFrame / decodedBuffer.sampleRate;
    seek.style.setProperty('--fill', `${progress * 100}%`);
    $('#current-time').textContent = formatTime(start + progress * (end - start));
    $('#playhead').style.left = `${((start + progress * (end - start)) / player.duration) * 100}%`;
  });
  seek.addEventListener('change', () => {
    if (decodedBuffer) {
      const { startFrame, endFrame } = getTrimFrameRange();
      const start = startFrame / decodedBuffer.sampleRate;
      const end = endFrame / decodedBuffer.sampleRate;
      player.currentTime = start + (Number(seek.value) / 1000) * (end - start);
    }
    isSeeking = false;
  });
  $('#export-button').addEventListener('click', exportMaster);
  document.addEventListener('keydown', (event) => {
    if (event.code !== 'Space' || event.repeat || !decodedBuffer) return;
    const target = event.target;
    if (target instanceof HTMLElement && (target.isContentEditable || ['BUTTON', 'INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName))) return;
    event.preventDefault();
    togglePlayback();
  });
  window.addEventListener('resize', () => {
    drawWaveform();
    drawEqGraph();
  });

  setColorTheme(document.documentElement.dataset.theme || 'light');
  updateIntensityUI();
  updateEqUI();
})();
