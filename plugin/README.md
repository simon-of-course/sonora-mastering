# SONORA Master VST3

SONORA Master is an experimental Windows VST3 bus processor for FL Studio. The interface follows the SONORA mastering website.

## Download and installation

- [Download SONORA Master VST3](https://github.com/simon-of-course/sonora-mastering/releases/latest/download/SONORA_Master.vst3.zip).
- [Download SONORA Shutter VST3](https://github.com/simon-of-course/sonora-mastering/releases/latest/download/SONORA_Shutter.vst3.zip).
- [Download SONORA Drowner VST3](https://github.com/simon-of-course/sonora-mastering/releases/latest/download/SONORA_Drowner.vst3.zip).

1. Extract the ZIP file.
2. Copy the complete `SONORA_Master.vst3` folder to `%CommonProgramFiles%\VST3` (usually `C:\Program Files\Common Files\VST3`).
3. Restart FL Studio and use **Find installed plugins**.

Do not rename the `.vst3` plugin binary to `.dll`; VST2 and VST3 are different formats.

## Controls and limitations

The plugin includes Natural, Warm, Punch, Air, Bass, and Vocal Clarity starting points; mastering strength; vocal focus; nasal and harsh-resonance controls; vocal presence; bass and beat-high balance; saturation; a five-band EQ with a logarithmic frequency-response display and vertical gain faders; output level; and bypass. Presets are original SONORA starting points.

The Vocal and Beat modes are routing indicators and pass audio through unchanged. Without a sidechain, the Master instance receives the combined mix and cannot access or process the Vocal and Beat buses separately.

## Optional vocal sidechain

The sidechain detects vocal energy around 900 Hz and 3.2 kHz and gently lowers those presence ranges in the Beat signal by up to 2.5 dB. It does not separate stems or automatically mix a song. Without an active sidechain, the Vocal-Fokus control has no effect.

1. Route vocal tracks to a Vocal bus and instrumental tracks to a Beat bus.
2. Route both buses to the final Master bus.
3. Create a **sidechain-only** send from the Vocal bus to the Beat bus. Do not route the vocal audio into the Beat bus.
4. Insert SONORA Master on the Beat bus, select Master mode, and adjust Vocal-Fokus.

The processing instance must receive only the Beat on its main input. The vocal sidechain is a detector reference and is not mixed into the plugin output. Do not put this sidechain-processing instance on the final Master bus.

## Local analysis and automatic suggestions

With the Beat on the main input and the Vocal routed to the sidechain, select **Master**, start playback, and click **BEAT + VOCAL ANALYSIEREN**. The plugin measures eight seconds of both signals, reports their levels in five frequency bands, and then applies restrained EQ cuts and a small dynamics adjustment to the Beat signal. Re-run the analysis if the arrangement or routing changes. The vocal is measured as a reference and is not processed by this Beat-bus instance.

This is a deterministic, local DSP analysis, not a neural AI model. It does not learn a genre, separate stems, set the final combined loudness, or guarantee a perfect-sounding master. Listen to the result and adjust or bypass the suggestions as needed. For final loudness and peak control, use a separate mastering stage on the combined mix.

## Build on Windows

Install Visual Studio 2022/2026 with the C++ desktop workload and CMake 3.25 or newer. From this folder, run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target SONORA_Master
```

CMake fetches the pinned [Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk) and its required submodules on the first configure. The resulting `SONORA_Master.vst3` bundle is under `build\VST3\Release`.

The plugin UI is in German, and the DAW saves the plugin state and host-automatable controls. This prototype does not use AI or guarantee a finished master.

## SONORA Shutter

The separate SONORA Shutter VST3 is an automated vocal gate effect. Put it directly on the vocal mixer channel, then create automation for **Shutter aktiv** and turn it on only for the phrases or bars where the effect should play. The default state is off. Its UI button is a toggle and can also be automated by the DAW.

Tempo Sync follows the DAW tempo and musical play position. Choose a straight note value from whole note through 1/32, or a triplet value. With Sync off, use the Rate control for a free-running 0.5–20 Hz shutter. Depth sets how far the gated half-cycle is reduced, Mix blends the effect, and Glattung softens transitions to help avoid clicks. Stopping playback returns the signal to dry.

Build the separate effect with `cmake --build build --config Release --target SONORA_Shutter`. The bundle is `build\VST3\Release\SONORA_Shutter.vst3`. An HTML interface preview is available at [resource/shutter-preview.html](./resource/shutter-preview.html).

## SONORA Drowner

SONORA Drowner is a continuous underwater low-pass effect for vocals. Automate **Unterwasser-Anteil** from zero for a natural vocal to a fully muffled underwater tone. When the automated amount comes back down, the filter recovers gradually; **Auftauchzeit** adjusts the return from 0.2 to 6 seconds. The parameter automation can also be drawn gradually in the DAW. The plugin does not add reverb or artificial water noises.

Build it with `cmake --build build --config Release --target SONORA_Drowner`. The bundle is `build\VST3\Release\SONORA_Drowner.vst3`. An HTML interface preview is available at [resource/drowner-preview.html](./resource/drowner-preview.html).
