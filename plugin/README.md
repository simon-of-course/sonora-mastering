# SONORA Master VST3

SONORA Master is an experimental Windows VST3 bus processor for FL Studio. The interface follows the SONORA mastering website.

## Download and installation

[Download SONORA Master VST3](https://github.com/simon-of-course/sonora-mastering/releases/latest/download/SONORA_Master.vst3.zip).

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

## Build on Windows

Install Visual Studio 2022/2026 with the C++ desktop workload and CMake 3.25 or newer. From this folder, run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target SONORA_Master
```

CMake fetches the pinned [Steinberg VST3 SDK](https://github.com/steinbergmedia/vst3sdk) and its required submodules on the first configure. The resulting `SONORA_Master.vst3` bundle is under `build\VST3\Release`.

The plugin UI is in German, and the DAW saves the plugin state and host-automatable controls. This prototype does not use AI or guarantee a finished master.
