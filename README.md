# SONORA Mastering Studio

A browser-based audio mastering tool. Tracks stay on your device: the app uses the Web Audio API for live processing and renders a 24-bit PCM WAV locally when you export.

## Run

Open `index.html` in a current desktop browser. For the most reliable audio decoding, serve this folder with any static web server and open its local URL.

## Included

- Twelve selectable sound profiles inspired by common online mastering workflows, with adjustable intensity
- Five-band EQ with a live frequency-curve display
- Real-time playback, waveform, spectrum, level meters, and A/B dry comparison
- Light and dark themes, with the selected theme remembered on the device
- Start/end trimming with playback and export restricted to the selected range
- Three-band dynamics, peak limiting, and restrained RMS-based output level adjustment
- Offline WAV export at the source sample rate, as 24-bit PCM

The signal chain runs locally in your browser. The output level adjustment targets approximately -16 dBFS RMS with a -1 dBFS sample-peak ceiling and a maximum 12 dB gain change. This is not a LUFS or true-peak meter, and the browser DSP cannot replace a dedicated mastering service, a great source mix, or a final listening check.
