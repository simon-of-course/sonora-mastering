# SONORA Mastering Studio

A browser-based audio mastering tool. Tracks stay on your device: the app uses the Web Audio API for live processing and renders a 24-bit PCM WAV locally when you export.

## Run

Open `index.html` for the SONORA Audio landing page. Open `mastering.html` to use the browser mastering tool, or `plugins.html` to see the VST3 plugin previews and downloads. For the most reliable audio decoding, serve this folder with any static web server and open its local URL.

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

All analysis and processing run locally in your browser; songs are not sent to an AI service. Automatic settings use measured frequency balance and crest factor, not a trained AI model or genre classifier. The automatic "Natural" character starts with low processing strength and only recommends small EQ cuts; it does not infer that a sparse frequency band should be boosted. The Vocal, Beat, and Master tabs organize the controls but do not separate stems: nasal reduction, resonance suppression, bass, and presence processing can affect other sounds in the same frequency range. Resonance suppression analyzes the mix for persistent narrow peaks between 2.2 and 10 kHz and applies up to four restrained notches; it cannot identify every intermittent squeal or replace dedicated denoising. Mastering strength scales the processing parameters without mixing dry and phase-shifted wet signals, which can cause comb-filter coloration. Export loudness targets -9 LUFS with an estimated -1 dBTP ceiling, independently from the mastering-strength control. The output-level stage allows up to 36 dB gain and true-peak limiting where needed; extremely quiet or unusually dynamic mixes may remain below target to avoid excessive gain or limiting. -9 LUFS is a loud contemporary reference, not a universal streaming requirement; services may turn down louder masters. Automatic processing cannot guarantee a perfect master for every mix, so compare the result with the original and listen on more than one system.

## SONORA VST3 plug-ins (v0.2 "Rap Overhaul")

Three separate Windows x64 VST3 plug-ins for rap production in FL Studio live in [plugin/](./plugin/); `plugins.html` shows their interfaces and download links. No extra runtime is required.

- **SONORA Master** – bus processor with three modes that all process audio: **Vocal** (de-esser, nasal/resonance control, presence, vocal compressor), **Beat** (vocal-sidechain ducking, 808-Punch, glue, Bass-Mono, Stereo-Breite) and **Master** (full chain plus Lautheit drive, oversampled clipper and a look-ahead true-peak limiter with adjustable Ceiling). 12 presets incl. Trap 808, Drill, Boom Bap, Cloud Rap, Vocal Upfront and Club Loud; LUFS (momentary, short-term, integrated) and true-peak meters; 5-band EQ with real response curve and a local 8-second beat + vocal analysis.
- **SONORA Shutter** – tempo-synced vocal gate with 12 note values (incl. triplets and dotted), Pulsbreite, Swing, Versatz, Form (Hart/Weich/Pumpe), Ping-Pong stereo and Gate-Klang (Stumm/Telefon/Dumpf); automate "Shutter aktiv".
- **SONORA Drowner** – 24 dB/oct underwater filter with Abtauchzeit/Auftauchzeit, Resonanz, Wobble, Wasserhall and Körper; bit-exact clean at 0 % (once a reverb tail has faded).

Recommended FL Studio routing: vocals → Vocal bus (Master in Vocal mode), beat → Beat bus (Master in Beat mode, vocal bus as sidechain), both → Master (Master in Master mode). SONORA Master reports a small constant latency that FL Studio compensates automatically. The plug-ins use deterministic DSP, not AI, and do not separate stems. See [plugin/README.md](./plugin/README.md) for all parameters, the full FL Studio setup, build instructions (Windows and `plugin/tools/build_linux.sh`), tests (`plugin/tests/run_tests.sh`) and CI.
