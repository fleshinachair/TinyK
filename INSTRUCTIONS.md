# Project: schwung-mk-va (Ableton Move Synth Module)

## Architecture Overview
Build a 4-voice polyphonic virtual analog synth module inspired by the 2000s dual-oscillator VA engine (microKORG/MS2000 style).

## Hardware & Environment Constraints
- Target: ARM64 Cortex-A72 Linux (Ableton Move).
- Build system: Docker cross-compilation via `./scripts/build.sh`.
- Real-time Audio Rule: Zero dynamic allocations (`malloc`, `calloc`, `new`) in `dsp.c` audio processing paths. All voice states and buffers must be statically allocated.
- Parameter Range: All parameters in `module.json` and `ui.js` scale 0.0 to 1.0 (mapped internally in C).

## Engine Specifications (dsp.c / dsp.h)
1. **Polyphony**: 4 voices, simple oldest/quietest voice-stealing.
2. **Dual Oscillators per voice**:
   - Osc 1: PolyBLEP Saw, Square (variable pulse width), Triangle, Sine.
   - Osc 2: PolyBLEP Saw, Square, Triangle + Detune (-24 to +24 semitones) + Hard Sync & Ring Mod against Osc 1.
3. **Filter**:
   - 2-pole/4-pole multimode filter (24dB Lowpass, 12dB Lowpass, 12dB Bandpass, 12dB Highpass).
   - Resonance with simple tanh soft-clipping saturation in the feedback path.
4. **Modulation**:
   - Filter ADSR + Amp ADSR.
   - 2x LFOs (Triangle, Square, Saw, S&H) routable to Pitch, Cutoff, and PWM.
5. **Effects**:
   - Simple stereo chorus/ensemble + tempo-syncable digital delay.

## Control & UI (ui.js & module.json)
- Parameter Pages mapped to Move's 8 encoders:
  - Page 1: OSC (Wave1, PulseWidth, Wave2, Detune, Sync/Ring, Mix, Sub, Portamento)
  - Page 2: FILTER (Cutoff, Res, Type, KeyTrack, EnvInt, Drive, ModInt, VelSens)
  - Page 3: ENVs (Attack1, Decay1, Sustain1, Release1, Attack2, Decay2, Sustain2, Release2)
  - Page 4: FX/MOD (LFO1 Rate, LFO2 Rate, Chorus Mix, Delay Time, Delay Feedback, Delay Mix, Master Vol, Pan)
- Preset system:
  - Embed a `presets.json` table with 8 starter archetype patches (Bass, Lead, Pad, Pluck, Strings, Poly, Arp, FX).

## Execution Task
1. Inspect the repository template structure.
2. Implement the engine in `dsp.c` and header declarations.
3. Update `module.json` with all parameter IDs, minimums, maximums, and page groupings.
4. Update `ui.js` to handle encoder input and OLED display pages.
5. Run `./scripts/build.sh` in the terminal to verify the ARM64 shared library builds cleanly.