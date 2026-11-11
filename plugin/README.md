# SONORA VST3 plug-ins (v0.2 "Rap Overhaul")

Three Windows x64 VST3 plug-ins for rap, trap, drill and boom-bap production in FL Studio (they also run in other
VST3 hosts): **SONORA Master**, **SONORA Shutter** and **SONORA Drowner**. The user interfaces are in German and use
a dark SONORA look; every knob shows its live value. The DSP is classic, deterministic signal processing – no AI
model, no stem separation and no promise of a "perfect" master.

## Download and installation

- [Download SONORA Master VST3](https://github.com/simon-of-course/sonora-mastering/releases/latest/download/SONORA_Master.vst3.zip)
- [Download SONORA Shutter VST3](https://github.com/simon-of-course/sonora-mastering/releases/latest/download/SONORA_Shutter.vst3.zip)
- [Download SONORA Drowner VST3](https://github.com/simon-of-course/sonora-mastering/releases/latest/download/SONORA_Drowner.vst3.zip)

1. Extract the ZIP file.
2. Copy the complete `SONORA_….vst3` folder to `C:\Program Files\Common Files\VST3` (`%CommonProgramFiles%\VST3`).
3. In FL Studio open **Options › Manage plugins** and click **Find installed plugins**.

Requirements: 64-bit Windows and a VST3 host. The bundles link the MSVC runtime statically, so no extra
Visual C++ runtime has to be installed. Do not rename the `.vst3` binary to `.dll`; VST2 and VST3 are different
formats. Projects saved with v0.1 load in v0.2; old settings and preset choices are migrated.

## FL Studio quick start for rap

Recommended mixer routing (all three buses use SONORA Master; only the **Bus-Modus** differs):

```
Vocal tracks ──► Vocal bus  (SONORA Master · Vocal) ──┬──────────────► Master (SONORA Master · Master)
                     │ sidechain only                 │
Beat tracks  ──► Beat bus   (SONORA Master · Beat)  ──┘
```

1. Route all vocal tracks to a **Vocal bus** and insert SONORA Master in **Vocal** mode.
2. Route all beat tracks to a **Beat bus** and insert SONORA Master in **Beat** mode.
3. Vocal sidechain: select the Vocal bus in the mixer, right-click the send knob on the Beat bus and choose
   **Sidechain to this track**. Then open the plug-in wrapper of the Beat-bus instance (gear menu › *Processing*)
   and assign the sidechain mixer track to the plug-in's **Vocal Sidechain** input port. **Vocal-Fokus** sets how
   much the beat makes room for the voice. The sidechain is only a detector signal; it is never mixed into the output.
4. Route both buses to the Master and insert SONORA Master in **Master** mode. Pick a preset (e.g. **Trap 808**),
   raise **Lautheit** while watching the LUFS display and keep **Ceiling** at −1 dBTP for streaming.
5. Put SONORA Shutter or SONORA Drowner directly on a vocal or ad-lib insert. Right-click **Shutter aktiv** or
   **Unterwasser-Anteil** › **Create automation clip** and draw the clip on the bars where the effect should play.

**Latency:** SONORA Master reports a small constant latency (below 3 ms at 48 kHz: 4× oversampling plus the
1.5 ms limiter look-ahead). It is identical in every mode and in bypass (the dry path is delayed), so FL Studio's
automatic plug-in delay compensation keeps everything in time and mode/bypass switches never shift the timing.
Shutter and Drowner report zero latency.

**Loudness:** Spotify, Apple Music, YouTube and most other services normalise loudness and turn loud masters down.
A −8 LUFS club master will not play louder there, only denser. Compare with reference tracks at matched loudness
and listen on several systems.

## SONORA Master

One bus processor with three modes. All modes process audio (in v0.1 Vocal and Beat were pass-through):

- **Vocal:** high-pass 80 Hz → EQ, Nasalität, Resonanzen → De-Esser → Stimmen-Präsenz → fast vocal compressor
  (Glue) → Sättigung → safety soft clip → Ausgang.
- **Beat:** high-pass 20 Hz → EQ, Bass, Beat-Höhen → vocal-sidechain ducking → 808-Punch → Glue → Stereo-Breite and
  Bass-Mono → Sättigung → Ausgang.
- **Master:** Beat chain plus De-Esser → Sättigung → Lautheit drive → oversampled soft Clipper → Ausgang (trim) →
  look-ahead true-peak limiter (4× oversampled true-peak detection, program-dependent release) → Ceiling.
  The ceiling always holds, whatever Ausgang is set to.

Saturation, drive and clipper run 4× oversampled to keep aliasing low. Processing is per sample and therefore
independent of the host buffer size.

**Presets** (character macros scaled by **Stärke** and added on top of your own knob settings): Natural, Warm,
Punch, Air, Bass, Vocal Clarity, Trap 808, Drill, Boom Bap, Cloud Rap, Vocal Upfront, Club Loud. The rap presets
combine EQ, glue, clipper, 808 punch, width, de-essing, saturation and a small loudness push.

**Meters:** input and output peak, true-peak hold, LUFS momentary (Momentan), short-term (Kurzzeit) and integrated
(Integriert, ITU-R BS.1770-4 gating; resets on transport start or with *Reset*), glue and limiter gain reduction,
clipper activity and stereo correlation.

| Parameter | Range (default) | Function |
|---|---|---|
| Bus-Modus | Vocal / Beat / Master (Master) | Selects the processing chain |
| Preset | 12 presets (Natural) | Character macro, see above |
| Stärke | 0–100 % (50 %) | How much of the preset character is applied |
| Eingang | ±12 dB (0) | Input gain |
| Lautheit | 0 to +18 dB (0) | Drive into clipper and limiter (Master mode) |
| Ceiling | −3 to 0 dBTP (−1.0) | True-peak ceiling of the limiter |
| Ausgang | ±6 dB (0) | Master: trim before the limiter; Vocal/Beat: final gain |
| Bypass | on/off | Latency-compensated bypass |
| Glue | 0–100 % (0) | Bus compressor (fast vocal compressor in Vocal mode) |
| Clipper | 0–100 % (0) | Oversampled soft clipper before the limiter |
| Sättigung | 0–100 % (0) | Level-compensated, oversampled saturation |
| 808-Punch | 0–100 % (0) | Sub harmonics and low transient so 808s carry on phones and laptops |
| Bass | ±6 dB (0) | Low shelf |
| Beat-Höhen | ±6 dB (0) | High shelf |
| Bass-Mono | 0–250 Hz (0 = Aus) | Makes everything below the frequency mono |
| Stereo-Breite | 0–200 % (100 %) | Mid/side width above ~250 Hz |
| De-Esser | 0–100 % (0) | Dynamic reduction at 5–10 kHz |
| Nasalität | 0–100 % (0) | Reduces the nasal range around 900 Hz |
| Resonanzen | 0–100 % (0) | Tames harsh resonances around 3.4 kHz |
| Stimmen-Präsenz | ±4 dB (0) | Presence peak around 3.5 kHz |
| EQ 60 Hz / 250 Hz / 1 kHz / 4 kHz / 12 kHz | ±8 dB (0) | 5-band EQ (low shelf, three peaks, high shelf) |
| Vocal-Fokus | 0–100 % (50 %) | Depth of the vocal-sidechain ducking in the beat |
| Analyse starten | button | Starts the 8-second analysis |

The EQ display draws the real response of the active filters.

### Vocal sidechain

The sidechain detects vocal energy around 900 Hz and 3.2 kHz and dynamically lowers exactly those ranges in the
beat while the voice is present (depth set by **Vocal-Fokus**). It does not separate stems or mix the song for you;
without an active sidechain Vocal-Fokus has no effect.

### Local analysis

In **Beat** mode (beat on the main input, vocal on the sidechain) or in **Master** mode (sidechain optional), start
playback and click **Analysieren**. The plug-in measures eight seconds, shows the levels of beat and vocal in five
bands and sets small corrective EQ cuts (shown as Auto-EQ in the EQ display). Without a sidechain in Master mode it
analyses the mix only. Vocal mode does not run the analysis. Re-run it when the arrangement or routing changes. This
is a deterministic local measurement, not an AI model; listen to the result and adjust or bypass it.

## SONORA Shutter

Tempo-synced volume gate for stutters, chops and trap pumps on vocals and ad-libs. The pattern is locked to the
song position of the host, so it lands on the grid at every block size. Default is off – automate **Shutter aktiv**
to switch it on exactly where it should play. When it is off or playback is stopped, the output is the untouched
input; enable/disable and start/stop use a short fade to avoid clicks.

| Parameter | Range (default) | Function |
|---|---|---|
| Shutter aktiv | on/off (off) | Turns the gate on; meant to be automated |
| Tempo Sync | on/off (on) | Follow the DAW tempo and position |
| Notenwert | 1/1, 1/2, 1/4, 1/8, 1/16, 1/32, 1/8 T, 1/16 T, 1/32 T, 1/4 P, 1/8 P, 1/16 P (1/8) | Step length (T = triplet, P = dotted) |
| Rate | 0.5–20 Hz (4 Hz) | Free-running speed when Tempo Sync is off |
| Tiefe | 0–100 % (85 %) | How far the closed part is reduced |
| Mix | 0–100 % (100 %) | Dry/wet |
| Glättung | 0–12 ms (2 ms) | Edge smoothing against clicks |
| Pulsbreite | 10–90 % (50 %) | Open portion of each step |
| Swing | 0–75 % (0) | Delays every second step (50 % = triplet shuffle) |
| Versatz | 0–100 % (0) | Shifts the pattern later by up to one step |
| Form | Hart / Weich / Pumpe (Hart) | Hard gate, soft gate or trap-style pump |
| Stereo | Mono / Ping-Pong (Mono) | Ping-Pong alternates left and right (stereo buses) |
| Gate-Klang | Stumm / Telefon / Dumpf (Stumm) | The closed part becomes silence, a telephone band or a dull low-pass |
| Bypass (ID 13) | on/off (off) | VST3 host bypass with a click-free 10 ms crossfade to dry; no UI control – use FL Studio's plug-in bypass or automate it |

## SONORA Drowner

Pulls the vocal under water and lets it surface again. Automate **Unterwasser-Anteil**; **Abtauchzeit** and
**Auftauchzeit** set how fast the sound dives and comes back. A 24 dB/oct modulation-stable low-pass (cascaded TPT
state-variable filters) sweeps from above 20 kHz down to 350 Hz, with gentle automatic make-up gain. At
Unterwasser-Anteil 0 % the output is a bit-exact copy of the input (once a Wasserhall tail has died away) – whatever
**Ausgang** is set to, because Ausgang only scales the wet sound.

| Parameter | Range (default) | Function |
|---|---|---|
| Unterwasser-Anteil | 0–100 % (0) | Depth of the effect; meant to be automated |
| Abtauchzeit | 0.01–2 s, logarithmic (0.05 s) | Time to dive in |
| Auftauchzeit | 0.2–6 s (2 s) | Time to come back up |
| Resonanz | 0–100 % (30 %) | Filter resonance |
| Wobble | 0–100 % (35 %) | Moving cutoff and slight stereo detune |
| Wasserhall | 0–100 % (30 %) | Dark, small diffuse reverb |
| Körper | 0–6 dB (2.5 dB) | Low-body shelf, scaled by depth |
| Mix | 0–100 % (100 %) | Dry/wet |
| Ausgang | −12 to +6 dB (0) | Level of the wet/underwater sound only – the dry signal is never touched, so 0 % stays clean |
| Bypass (ID 9) | on/off (off) | VST3 host bypass with a click-free 10 ms crossfade to dry; no UI control – use FL Studio's plug-in bypass or automate it |

## Build

The DSP lives in SDK-independent header-only engines in `src/dsp/` (`master_engine.h`, `loudness.h`,
`shutter_engine.h`, `drowner_engine.h`); the VST3 glue is in `src/sonora_*.cpp`, the shared VSTGUI views in
`src/ui/`, the editor layouts in `resource/*.uidesc`. The HTML previews in `resource/*-preview.html`
([Master](./resource/ui-preview.html), [Shutter](./resource/shutter-preview.html),
[Drowner](./resource/drowner-preview.html)) mirror the editors and are shown on the website.

CMake 3.25 or newer fetches the pinned [Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk) and its
submodules on the first configure.

### Windows (MSVC)

Install Visual Studio 2022 or newer with the C++ desktop workload. From this folder:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target SONORA_Master SONORA_Shutter SONORA_Drowner
```

The bundles are in `build\VST3\Release\SONORA_*.vst3`.

### Linux

```bash
plugin/tools/build_linux.sh --deps   # --deps installs the apt packages once (needs root/sudo)
```

The script configures and builds all three plug-ins plus Steinberg's `validator` and `editorhost`, runs the
validator on every bundle and runs CTest. Options: `[build-dir] [--clean] [--debug] [--no-tests]`.

### Tests

```bash
plugin/tests/run_tests.sh            # optional filter, e.g. run_tests.sh master
```

Compiles every `plugin/tests/test_*.cpp` without the SDK and runs it twice: with `-O2 -Wall -Wextra -Werror` and
with AddressSanitizer/UndefinedBehaviorSanitizer. The tests cover bypass null, block-size invariance, sample
rates 44.1–192 kHz, true-peak ceiling, LUFS metering, loudness targets of the presets, clipper aliasing, CPU load,
sidechain ducking, click-free automation, Drowner depth-0 null and Shutter grid alignment (incl. triplets and dotted
values).

### CI

`.github/workflows/build.yml` runs on every push and pull request: Linux unit tests, plug-in build, Steinberg
validator and editor screenshots (Xvfb), plus a Windows x64 MSVC build with validator and a check that the bundles
need no VC++ runtime DLLs. It uploads `SONORA_Master.vst3.zip`, `SONORA_Shutter.vst3.zip` and
`SONORA_Drowner.vst3.zip` as artifacts. Pushing a tag `vst3-v*` publishes them as a GitHub release, which the
`releases/latest/download/…` links above point to.
