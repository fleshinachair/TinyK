# TinyK - microKORG Synth Module for Ableton Move (Schwung Framework)

## Project Overview
- A 4-voice polyphonic / 2-voice dual-timbre virtual analog synth engine running on Ableton Move via the Schwung runtime.
- DSP core is C: `src/dsp/dsp.c` (engine), `src/dsp/dsp.h`, `src/dsp/syx_bank.h` (runtime .syx loader, included by dsp.c), `src/dsp/presets.h` (built-in bank, generated). `src/dsp.c` / `src/dsp.h` are one-line include shims.
- UI: `src/ui.js`. Manifest: `src/module.json`. The engine also serves its own `ui_hierarchy` JSON (`MK_UI_HIERARCHY` in dsp.c): keep the two in step.

## Build, Install & Test
- `./scripts/build.sh` cross-compiles `build/dsp.so` (ARM64, Cortex-A72) and packages `dist/TinyK/` + `TinyK.tar.gz`. Toolchain order: Docker (scripts/Dockerfile, only if `docker` is on PATH) -> CMake when `$CROSS_PREFIX` is set -> `aarch64-linux-gnu-gcc` -> Zig. This Windows machine has neither Docker nor a GNU cross-gcc, so the build always takes the Zig path with the bundled `.toolchain/zig-windows-x86_64-0.13.0/zig.exe` (gitignored; a `zig` on PATH also works):
  `zig cc -target aarch64-linux-gnu.2.35 -mcpu=cortex_a72 -O3 -fPIC -shared -Wall -Wextra -Isrc -Isrc/dsp src/dsp/dsp.c -lm -o build/dsp.so`
  (glibc 2.35 target, matching the Move; the result must be warning-free). The host-side tools build the same sources natively with the same zig.exe (`zig cc` without `-target`), see Verification tools.
- Package contents: dsp.so, module.json, ui.js, presets.json, help.json, presets.h, release.json and an EMPTY `banks/` (README.txt only): a release never redistributes .syx banks. `./scripts/install.sh` copies your local `banks/*.syx` to the Move separately.
- `./scripts/install.sh` copies the module to the Move over SSH: default `ableton@move.local` (key-based login), override with `MOVE_HOST=ableton@<ip>`. Destination `/data/UserData/schwung/modules/sound_generators/TinyK`; dsp.so is swapped via `dsp.so.new`; banks already on the Move are kept. Reload the slot (or restart Schwung) afterwards.
- Release: bump `version` in `src/module.json` and `release.json` together (1.0.0 now). `release.json` (repo root, Schwung catalog format `{version, download_url}`) points at the GitHub release asset `https://github.com/fleshinachair/TinyK/releases/download/v<version>/TinyK.tar.gz`; the catalog entry needs `github_repo` fleshinachair/TinyK and `asset_name` TinyK.tar.gz.
- Shell scripts must stay LF and executable (`.gitattributes`, git mode 100755).
- Preset generator: `python tools/extracts_presets.py tools/FactoryBackUpDoResetAfter.syx` writes presets.h (4 copies) and presets.json (2 copies). The committed presets.h comes from the local backup dump (anonymous names, labels from `FACTORY_LABELS`) and is the compiled-in default bank; the .syx itself is gitignored.

## Architecture Rules & Guidelines
1. **SysEx Parsing:**
   - Always open and write files using explicit UTF-8 (`encoding="utf-8"`) to prevent Windows `cp1252` encoding errors.
   - Values extracted into `presets.json` and `presets.h` must normalize strictly between `0.0f` and `1.0f`.
   - Never overwrite valid patch data with placeholder or copy-pasted floats.
   - `tools/extracts_presets.py` and `src/dsp/syx_bank.h` are the same decoder in two languages: change both together and run `tools/test_bank_loader.py`.

2. **DSP & Voice Architecture:**
   - Cutoff, key tracking, filter EG and virtual patches are summed in control pitch (octaves), converted to Hz once: `fc = cutoff_base_hz * 2^(knob * cutoff_octaves + keytrack + EG + patches)`.
   - Filter Envelope Intensity (`env_int`) is bipolar (`0.5` = neutral, `<0.5` = negative modulation, `>0.5` = positive modulation).
   - In Layer mode (`voice_mode == 1`), the engine allocates two voices: Timbre 1 (Voice A, panned left) and Timbre 2 (Voice B, panned right), mixed by `timbre_balance`.
   - The voice mix is soft-clipped (`tanh`) so stacked voices / both timbres cannot clip digitally.
   - Real-time path: no heap allocation, no file I/O. Renders must be deterministic (the noise generator is reseeded in `synth_init`).

3. **Filter (svf_process_2pole):**
   - Zero-delay-feedback (TPT) SVF with the correct solution `hp = (x - (k + g) * s1 - s2) / (1 + g (g + k))`. The old `k * s1` (missing g) plus tanh on s1/u and +-2 state clamps went unstable near the cutoff ceiling and sprayed kHz of hiss (a 131 Hz saw through LPF12 res 0.35 at 12.9 kHz had an 8 kHz centroid). The loop is linear (stable for any k > 0); drive saturation (`tanh`) is on the filter input only; states have a wide safety clamp (`SVF_STATE_LIMIT` 64).
   - Damping: LPF24 stages start Butterworth (k0 = sqrt 2, measured on the VST), LPF12/HPF k0 = 2, both `k = k0 * (1 - 0.5 * res_damping_range * res)`. BPF: `k = bpf_k0 * 2^(-4.38 res)` (Q 0.33 at res 0, 1.47 at res 63, measured).
   - Cutoff ceiling `cutoff_ceil_hz` = 19 kHz; the SVF clamps fc to 0.45 fs internally.
   - Key tracking: `(note - KEYTRACK_PIVOT) * (2 * keytrack - 1) * KEYTRACK_SLOPE / 12` octaves (keytrack 0..1, 0.5 = off), pivot 60, slope 1.0. (The VST measured ~2.0 about ~62 at +63, but that made the +48 presets worse; kept 1:1 until intermediate values are measured.)

4. **Brightness tilt:** a first-order high shelf on the voice mix, after the filters and before the soft clip (`tilt_db` 26.7 dB, `tilt_hz` 20 kHz): ~+2 dB at 3 kHz, +7 at 8 kHz, +18 at 17 kHz. It matches the VST's open saw (co_127) within 1.6 dB rms. Post-filter is deliberate: it is linear there and leaves the pre-filter drive alone; a per-voice pre-filter tilt scored worse (106.8 vs 104.1).

5. **Virtual patch & LFOs:** per timbre, 2 LFOs (LFO1 saw/square/triangle/S&H, LFO2 saw/square/sine/S&H; tempo sync at `lfo_tempo_bpm` 120; key sync restarts at the positive peak) and 4 patch slots (sources EG1, EG2, LFO1, LFO2, velocity, keyboard, MIDI1 = CC1, MIDI2 = CC2; destinations pitch, osc2 pitch, osc1 ctrl1, noise, cutoff, amp, pan, LFO2 rate). Depth = full scale * (|int|/63)^`patch_int_curve` (2.4 measured); full scale 24 semitones (pitch, measured) and 10.61 octaves (cutoff, lower bound). MIDI sources are 0 until a CC arrives.

## Presets, banks and selection
- Built-in bank: `FACTORY_PRESETS` in presets.h (used when there are no banks).
- Runtime banks (`syx_bank.h`): `v2_create_instance` scans `<module_dir>/banks/*.syx` once, decodes up to 16 banks (alphabetical, case-insensitive) into static storage (~54 KB each). Accepted: microKORG `F0 42 3g 58` or microKORG S `F0 42 3g 00 01 40`, function `4C` (ALL PROGRAM DATA, 128 programs, 37163 bytes) or `50` (ALL DATA). Anything else (single-program `40` dumps, truncated/oversized/random files) is skipped; with several SysEx messages in a file the first valid bank is used. Selecting a bank later is a pointer swap (`active_presets()`), never file I/O.
- Labels: the stored 12-character name when present ("A.11 Trancey" style names keep their code; plain names get it prepended), else the code ("A.11").

## Controls & UI (Schwung host)
- The host builds the screens from `ui_hierarchy` (src/module.json, also served by the engine as MK_UI_HIERARCHY, generated from it and checked identical by test_bank_loader.py) plus the engine's live `get_param("chain_params")`, which overrides inline metadata. Enums display their option names and travel as indices; set_param also accepts option names. Host code for reference: shadow/ and shared/param_pages/ under /data/UserData/schwung on the Move.
- Main page (root level): a preset browser (`list_param` preset, `count_param` preset_count, `name_param` preset_name): the header shows the live patch name and the jog wheel steps all 128 programs. Its first knobs are **Category** (`category`, enum of 8: Trance, Techno/House, Electronica, DnB/Breaks, Hiphop/Vintage, Retro, SE/Hit, Vocoder = matrix rows 1-8) and **Program** (`patch`, enum of 16: A1..A8, B1..B8 = both bank sides of that row). Preset index = (patch / 8) * 64 + category * 8 + patch % 8. There is no standalone A/B knob any more: Program 1-16 covers both sides.
- Bank page (`bank` level, entered from the root's "Bank" entry): the **Bank** knob (`bank_file`) whose enum options are "Built-in" plus the .syx file names found at start-up (from the live chain_params, JSON-escaped); `bank_file_name` returns the active name, `bank_file_count` the number of choices. "Browse banks" opens `bank_list`, an items level (`items_param` bank_list returning `[{"index","label"}]`, `select_param` bank_file, back to root). Changing bank keeps the program.
- src/ui.js mirrors this for the module's own screen: a PRESET page (Category, Program, Mode, Edit, Balance, Cutoff, Res, EnvInt) and a BANK page showing the file name.
- Legacy keys still accepted: `bank_side`, `program` (11..88), `genre_category`, `program_num` (1..16), `preset` (0..127; integer strings are indices). Other getters: `bank_file_name:<n>`, `preset_name:<idx>`.

## SysEx Format Notes (verified)
- 128 programs x 254 bytes after 7-to-8 unpacking. Timbre 1 at byte 38, Timbre 2 at 146.
- Voice mode is byte 16 bits 4-5 (0 = Single, 2 = Layer, 3 = Vocoder). Byte 14 is NOT the voice mode.
- Timbre offsets: 7 osc1 wave, 12 osc2 wave/mod select, 13/14 osc2 semitone/tune, 16/17/18 osc1/osc2/noise level, 19 filter type, 20 cutoff, 21 resonance, 22 EG1 int, 24 key track, 25 amp level, 27 bit 0 distortion, 30-33 EG1 (filter) ADSR, 34-37 EG2 (amp) ADSR (checked: swapping or ADRS order scores far worse), 38-40 LFO1 (wave/key sync, rate, tempo sync), 41-43 LFO2, 44-51 four patches as (route: bits 0-3 source, 4-7 destination; intensity 64 = 0).
- The backup dump's names (bytes 0-11) are blank; VST exports (e.g. banks/MicroKorgFactory.syx) carry real names.
- `wave1` is index/6 over Saw, Pulse, Tri, Sine, Vox, DWGS, Noise. Vox/DWGS are additive approximations, not the sampled waves.
- Vocoder programs get a generic carrier (`data_valid: false`). The delay has no on/off bit: depth 0 = off, otherwise a send on the unity dry signal. The VST's Mod FX / Delay are likewise off only at depth 0.

## Verification tools
- `python tools/audit_all_presets.py`: renders all 128 built-in presets, flags SILENT / VERY QUIET / CLICK ONLY / UNSTABLE.
- `tools/test_behavior.c`: Layer triggering, timbre-edit routing, envelope time curves (build: `zig cc -O2 -Isrc/dsp tools/test_behavior.c src/dsp/dsp.c -lm`).
- `python tools/test_bank_loader.py`: C loader vs presets.h parity, scanning/skipping, names, selection, every bank in `banks/` cross-checked against extracts_presets.py, and the host API (v2 create/set/get: chain_params, bank_list, Category/Program, ui_hierarchy == module.json).
- `python tools/fit_filter_sweep.py [--selftest]`: fits cutoff curve / EG scale from VST sweeps (`--cut KNOB=FILE`, `--eg INT=FILE`, `--stage-q`); `--selftest` must recover the engine's own constants.
- `python tools/render_demo_sweeps.py [--all]`: demo WAVs with a cutoff sweep (`output/demos/`), or 4 s auditions of all 128 built-in presets (`output/all_presets/`). `output/` is gitignored.

## Calibration (tools/calibrate_dsp.py)
- `python tools/calibrate_dsp.py check-syx | baseline | identify | tune [--exclude KEY] [--no-freeze] [--apply]`. Engine constants live in `tinyk_tuning_t` / `TINYK_TUNING_DEFAULTS` (compile-time in the shipped build, adjustable with -DTINYK_TUNING via the ctypes library built from tools/test_render.c).
- References: `tools/reference/ref_<KEY>_C3.wav` with the program in `mk_<key>.syx` (A11, A12, A14, A28, B11; A21 dropped: its take is a drum click). All re-recorded dry (Mod FX / Delay depth 0), so `DRY_REFS` renders them without FX. Sounding notes (`NOTES`): A11 60, A12 36, A14 48, A28 48, B11 48 (the VST octave shift differed between takes).
- Measured on the VST (fit_filter_sweep.py and spot recordings), frozen by `tune` by default: cutoff_base_hz 37.46, cutoff_octaves 10.61 (LPF24, Q 0.707 stages), env_octaves 8.88 (linear in intensity), bpf_k0 3.06; EG sustain is linear.
- Current baseline total 104.09 (A11 26.4, A12 15.9, A14 18.2, A28 13.5, B11 30.1). A11/A14/A28 still render darker than their references; B11's free-running LFO2 starts at phase 0 every render. A tune of the 6 unmeasured constants on these 5 references was rejected by leave-one-out (held-out 100.6 -> 101.2): do not apply constants that leave-one-out rejects.
