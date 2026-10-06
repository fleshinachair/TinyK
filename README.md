# TinyK for Ableton Move

A lightweight, microKORG-inspired virtual analog synth engine for Ableton Move, built on the [Schwung](https://github.com/charlesvestal/schwung) module runtime.

## Features
- **4-voice polyphonic / 2-voice dual-timbre** virtual analog engine (Single and Layer modes).
- **TPT zero-delay-feedback state-variable filter** (LPF24, LPF12, BPF12, HPF12) with a pre-filter drive stage, stable from low cutoffs up to 19 kHz at any resonance.
- **Virtual Patch matrix and two LFOs per timbre**, decoded from the patch data.
- **microKORG category navigation:** pick one of 8 categories, then scroll its 16 programs (A1–A8, B1–B8) on a single knob, with the patch name in the header. No separate A/B toggle.
- **Dynamic SysEx bank loader:** drop microKORG/MS2000 `.syx` bank dumps into the module's `banks/` folder and switch between them by name. Banks are decoded once at start-up, so switching never allocates memory or touches files while audio runs.
- **Built-in bank:** plays out of the box with no external files.

## Requirements
- An Ableton Move with Schwung installed and on your network (`move.local`).
- For a manual install: a terminal with `ssh`/`scp` and SSH access to the Move as user `ableton`.

## Installation

### From the Schwung module manager
If TinyK is listed in the Schwung module catalog, install it from there; updates are picked up from this repository's `release.json`.

### Manually
1. Download `TinyK.tar.gz` from the [Releases](https://github.com/fleshinachair/TinyK/releases) page.
2. Copy it to the Move and unpack it into the sound generator modules folder:
   ```bash
   scp TinyK.tar.gz ableton@move.local:/data/UserData/schwung/modules/sound_generators/
   ssh ableton@move.local
   cd /data/UserData/schwung/modules/sound_generators/
   tar -xzf TinyK.tar.gz && rm TinyK.tar.gz
   ```
   This creates `sound_generators/TinyK/`.
3. Reload a track slot with TinyK (or restart Schwung).

## Playing
- **Main page:** the first two knobs are **Category** (Trance, Techno/House, Electronica, DnB/Breaks, Hiphop/Vintage, Retro, SE/Hit, Vocoder) and **Program** (A1–A8, B1–B8 within that category). The header shows the current patch name; the jog wheel steps through all 128 programs. The remaining knobs edit the voice: mode, timbre, oscillators, filter, envelopes and effects.
- **Bank page:** open **Bank** from the main page. The **Bank** knob shows "Built-in" and the file name of each bank found in `banks/`; **Browse banks** lists them, and picking one returns to the main page. Changing bank keeps the selected category and program.

## Adding your own banks
TinyK reads microKORG bank dumps: SysEx files holding all 128 programs, either an *All Program Data* dump (about 37 KB) or an *All Data* dump. Files exported by the microKORG, its editor, or patch-bank collections in that format work.

1. Copy the `.syx` files into the module's `banks/` folder on the Move:
   ```bash
   scp MyBank.syx ableton@move.local:/data/UserData/schwung/modules/sound_generators/TinyK/banks/
   ```
2. Reload the TinyK slot. The banks appear on the Bank page in alphabetical order (up to 16), named after their files.

Notes:
- Program names stored in the dump are shown; otherwise programs are labelled by position (A.11–B.88).
- Single-program dumps and any file that is not a 128-program microKORG bank are skipped.
- Vocoder programs play with a generic carrier sound: TinyK has no vocoder.
- When building from source, `.syx` files in the repository's `banks/` folder are copied to the Move by `./scripts/install.sh`. They are never committed or included in a release package.

## Building from source
The module is cross-compiled for the Move's ARM64 (Cortex-A72) Linux. `./scripts/build.sh` uses the first toolchain it finds: Docker (`scripts/Dockerfile`), CMake with `CROSS_PREFIX`, `aarch64-linux-gnu-gcc`, or Zig.

### With Zig (no Docker needed)
1. Install [Zig 0.13.0](https://ziglang.org/download/) and either put `zig` on your `PATH` or unpack the Windows build to `.toolchain/zig-windows-x86_64-0.13.0/` in this repository.
2. Build and package:
   ```bash
   ./scripts/build.sh
   ```
   This runs `zig cc -target aarch64-linux-gnu.2.35 -mcpu=cortex_a72 -O3 -fPIC -shared ...` and writes `build/dsp.so`, `dist/TinyK/` and `TinyK.tar.gz`. On Windows, run it from Git Bash.
3. Install to the Move over SSH:
   ```bash
   ./scripts/install.sh
   # if move.local does not resolve on your network:
   MOVE_HOST=ableton@<move-ip> ./scripts/install.sh
   ```
   Then reload the TinyK slot.

### Development tools
The `tools/` folder holds the calibration and test tooling (Python 3 with NumPy and SciPy; C sources built with the same Zig): `test_behavior.c`, `test_bank_loader.py`, `audit_all_presets.py`, `render_demo_sweeps.py` (renders audition WAVs without hardware) and `calibrate_dsp.py`. See `CLAUDE.md` for the architecture notes.

## License
MIT. See `src/module.json`.

## Disclaimer and trademarks
TinyK is an independent, unofficial project. It is not affiliated with, endorsed by, or sponsored by KORG Inc. or Ableton AG.

- KORG, microKORG and MS2000 are trademarks or registered trademarks of KORG Inc.
- Ableton and Move are trademarks of Ableton AG.
- Schwung is a separate open-source project by its own authors.
- All other product names are the property of their respective owners and are used only to describe compatibility.

TinyK contains no KORG firmware, samples or audio. It does not include third-party `.syx` banks; you are responsible for having the right to use any bank files you load.

The software is provided "as is", without warranty of any kind.
