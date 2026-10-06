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
- Vocoder programs (16) do not use the synth timbre layout, so the extractor gives them a generic carrier and marks them `data_valid: false`. The delay has no on/off bit: depth (byte 21) 0 = off, otherwise it is a send added to the unity dry signal.
- Mixer: osc1/osc2 levels and the amp level byte become `osc_mix` (balance, 0.5 = both full) plus `level`; the oscillators add, like the hardware.

## Verification tools
- `python tools/audit_all_presets.py` renders all 128 presets at note 60 and flags SILENT / VERY QUIET / CLICK ONLY / UNSTABLE (build uses `-DTINYK_DIAG` to count NaN/Inf).
- `tools/test_behavior.c` checks Layer triggering, timbre-edit routing and the envelope time curves through the public param API.

## Calibration against hardware recordings (tools/calibrate_dsp.py) - findings
- `python tools/calibrate_dsp.py baseline | identify | tune [--exclude KEY] [--apply]`; engine constants are in `tinyk_tuning_t` (compile-time in the shipped build, adjustable with -DTINYK_TUNING).
- The six references in `tools/reference/` do NOT all match the dump's programs: only A.12 and A.28 rank near the top of 128 (at MIDI 48, not 60); A.21 ranks 116th and the dump's B.11 is a copy of A.11. Two tuning runs (all 6, and the 4 plausible) were rejected by leave-one-out validation, so no constants were changed.
- Before tuning again, re-verify that the dump is the factory bank (or re-record references from the dumped programs) and settle which MIDI note "C3" is.
