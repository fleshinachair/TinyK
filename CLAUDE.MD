# TinyK - microKORG Synth Module for Ableton Move (Schwung Framework)

## Project Overview
- A 4-voice polyphonic / 2-voice dual-timbre virtual analog synth engine running on Ableton Move via the Schwung runtime.
- DSP core is written in C (`dsp/dsp.c`, `presets.h`).
- UI layer runs via JavaScript (`ui.js`).
- Manifest configuration is defined in `module.json`.

## Build & Test Instructions
- Compile command: `./scripts/build.sh`
- The build script compiles the ARM64 shared object (`dsp.so`) and packages `TinyK.tar.gz`.
- Python preset generator: `python tools/extracts_presets.py tools/FactoryBackUpDoResetAfter.syx`

## Architecture Rules & Guidelines
1. **SysEx Parsing:**
   - Always open and write files using explicit UTF-8 (`encoding="utf-8"`) to prevent Windows `cp1252` encoding errors.
   - Values extracted into `presets.json` and `presets.h` must normalize strictly between `0.0f` and `1.0f`.
   - Never overwrite valid patch data with placeholder or copy-pasted floats.

2. **DSP & Voice Architecture:**
   - Filter cutoff mapping must follow exponential frequency scaling rather than linear interpolation.
   - Filter Envelope Intensity (`env_int`) is bipolar (`0.5` = neutral, `<0.5` = negative modulation, `>0.5` = positive modulation).
   - In Layer mode (`voice_mode == 1`), the engine must allocate two distinct voices: Timbre 1 (Voice A, panned left) and Timbre 2 (Voice B, panned right), mixed according to `timbre_balance`.
   - Apply a soft-clipping saturation stage (e.g., cubic or `tanh`) to prevent digital clipping when both timbres trigger simultaneously.
## SysEx Format Notes (verified against tools/FactoryBackUpDoResetAfter.syx)
- The dump is `F0 42 3g 58 50 ...`: 128 programs x 254 bytes after 7-to-8 bit unpacking. Timbre 1 starts at byte 38, Timbre 2 at byte 146.
- Voice mode is byte 16 bits 4-5 (0 = Single, 2 = Layer, 3 = Vocoder). Byte 14 is NOT the voice mode; it is `7` in 126 of 128 programs.
- The program name bytes (0-11) are all spaces in this dump, so labels come from `FACTORY_LABELS` in the extractor (the hardware has only a 3-digit LED).
- `wave1` is normalized as index/6 over Saw, Pulse, Tri, Sine, Vox, DWGS, Noise. Vox/DWGS are additive approximations (build_wavetable in dsp.c), not the sampled hardware waves.
