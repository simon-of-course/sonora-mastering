# SONORA Mastering Studio

A browser-based audio mastering tool. Tracks stay on your device: the app uses the Web Audio API for live processing and renders a 24-bit PCM WAV locally when you export.

## Run

Open `index.html` in a current desktop browser. For the most reliable audio decoding, serve this folder with any static web server and open its local URL.

## Included

- Automatic, track-specific EQ and mastering-strength recommendations based on local frequency-balance and dynamics analysis
- Optional manual EQ and mastering-strength adjustments
- Artist-controlled bass balance, vocal-presence range, and gentle-to-strong mix saturation
- Separate Vocal, Beat, and Main Master control tabs, including nasal-range reduction and analyzed resonance suppression
- Five-band EQ with a live frequency-curve display
- Real-time playback, waveform, spectrum, level meters, and A/B dry comparison
- Light and dark themes, with the selected theme remembered on the device
- Start/end trimming with playback and export restricted to the selected range
- Three-band dynamics, peak limiting, and integrated-loudness output adjustment
- Broad low/high shelves, adaptive five-region tonal analysis, and gentle harmonic saturation
- Offline WAV export at the source sample rate, as 24-bit PCM

All analysis and processing run locally in your browser; songs are not sent to an AI service. Automatic settings use measured frequency balance and crest factor, not a trained AI model or genre classifier. The Vocal, Beat, and Master tabs organize the controls but do not separate stems: nasal reduction, resonance suppression, bass, and presence processing can affect other sounds in the same frequency range. Resonance suppression analyzes the mix for persistent narrow peaks between 2.2 and 10 kHz and applies up to four restrained notches; it cannot identify every intermittent squeal or replace dedicated denoising. Export loudness targets -9 LUFS with an estimated -1 dBTP ceiling, independently from the mastering-strength control. The output-level stage allows up to 36 dB gain and true-peak limiting where needed; extremely quiet or unusually dynamic mixes may remain below target to avoid excessive gain or limiting. -9 LUFS is a loud contemporary reference, not a universal streaming requirement; services may turn down louder masters. Automatic processing cannot guarantee a perfect master for every mix, so compare the result with the original and listen on more than one system.
