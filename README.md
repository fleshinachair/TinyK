# TinyK for Ableton Move

A lightweight, microKORG-inspired virtual analog synth engine for Ableton Move, built on the [Schwung](https://github.com/charlesvestal/schwung) module runtime.

## Features
- **4-voice polyphonic / 2-voice dual-timbre** virtual analog engine (Single and Layer modes).
- **TPT zero-delay-feedback state-variable filter** (LPF24, LPF12, BPF12, HPF12) with an amp-stage drive circuit, stable from low cutoffs up to 19 kHz at any resonance.
- **Virtual Patch matrix and two LFOs per timbre**, decoded from the patch data.
- **microKORG category navigation:** pick one of 8 categories, then scroll its 16 programs (A1–A8, B1–B8) on a single knob, with the patch name in the header. No separate A/B toggle.
- **Dynamic SysEx bank loader:** drop microKORG/MS2000 `.syx` bank dumps into the module's `banks/` folder and switch between them by name. Banks are decoded once at start-up, so switching never allocates memory or touches files while audio runs.
- **Built-in factory bank:** 128 production-ready performance presets covering classic leads, pads, basses, and arpeggios, playable immediately without external files.

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
   This creates `sound_generators/tinyk/`. Upgrading from a build that installed `sound_generators/TinyK/` (upper case, which slots could not restore after a reboot): move any `.syx` files from `TinyK/banks/` to `tinyk/banks/`, then delete `TinyK/` (`./scripts/install.sh` does this for you).
3. Reload a track slot with TinyK (or restart Schwung).

## Playing
- **Pages,** in order: **Perf** (Category, Program, Cutoff, Resonance, Amp Attack, Amp Release, Arp, Mod Wheel), **Arp Settings** (Type, Range, Resolution, Gate, Swing, Latch, Key Sync, Target), **Arp Steps** (Step 1-8: Rest / Play), **Osc/Timbre** (Wave 1, Pulse Width, Wave 2, Semi, Tune, Voice Mode, Timbre Edit, Timbre Balance), **Envelopes** (filter EG, amp decay/sustain, Key Track, EG Int), **Mix/Filter** (Osc Mix, Noise, Sync/Ring, Filter Type, Portamento, Level, Drive), **Effects** (Chorus, Delay, LFO rates, Master Vol, Pan) and **Bank**. Shift+Click opens Schwung's page picker to jump straight to any page. The Move has no mod wheel, so **Mod Wheel** (0–127) on Perf plays its part for patches that route it; a wheel on an external controller (CC1) works too, and whichever moved last wins. Category picks one of the 8 categories (Trance, Techno/House, Electronica, DnB/Breaks, Hiphop/Vintage, Retro, SE/Hit, Vocoder) and Program its 16 programs (A1–A8, B1–B8); touching or turning Program shows the full code and name, e.g. "B.17 Flashin'Pad". The header shows the current patch name; the jog wheel steps through all 128 programs. The wave knobs show the waveform.
- **Arpeggiator:** each program plays its own microKORG arpeggio (type, octave range, resolution, gate, swing, step pattern, latch) when its arpeggiator is on, in time with the Move's tempo and, while the transport runs, on its beat grid. **Arp** on the Perf page turns it on or off for the current program, **Arp Settings** changes its type, range, resolution, gate, swing, latch, key sync and target timbre, and **Arp Steps** sets which of its 8 steps play, all live (the slot remembers them).
- **Layer mode:** Timbre Edit chooses which timbre the per-timbre pages edit. Their header shows [T1] or [T2], and with Timbre 2 their knob labels read T2.CUT, T2.RES, and so on.
- **Bank page:** open **Bank** from the main page. The **Bank** knob shows "Built-in" and the file name of each bank found in `banks/`; **Browse banks** lists them, and picking one returns to the main page. Changing bank keeps the selected category and program.

## Adding your own banks
TinyK reads microKORG bank dumps: SysEx files holding all 128 programs, either an *All Program Data* dump (about 37 KB) or an *All Data* dump. Files exported by the microKORG, its editor, or patch-bank collections in that format work.

1. Copy the `.syx` files into the module's `banks/` folder on the Move:
   ```bash
   scp MyBank.syx ableton@move.local:/data/UserData/schwung/modules/sound_generators/tinyk/banks/
   ```
2. Reload the TinyK slot. The banks appear on the Bank page in alphabetical order (up to 16), named after their files.

Notes:
- Program names stored in the dump are shown; otherwise programs are labelled by position (A.11–B.88).
- Single-program dumps and any file that is not a 128-program microKORG bank are skipped.
- Vocoder programs play with a generic carrier sound: TinyK has no vocoder.
- `.syx` files in the repository's `banks/` folder are never committed, packaged or installed: `./scripts/install.sh` leaves the Move's `banks/` folder alone, so a clean install shows only "Built-in". To send your local banks too, run `INSTALL_BANKS=1 ./scripts/install.sh` (it skips `TinyK_Default.syx`, which is the built-in bank).

## Building from source
The module is cross-compiled for the Move's ARM64 (Cortex-A72) Linux. `./scripts/build.sh` uses the first toolchain it finds: Docker (`scripts/Dockerfile`), CMake with `CROSS_PREFIX`, `aarch64-linux-gnu-gcc`, or Zig.

### With Zig (no Docker needed)
1. Install [Zig 0.13.0](https://ziglang.org/download/) and either put `zig` on your `PATH` or unpack the Windows build to `.toolchain/zig-windows-x86_64-0.13.0/` in this repository.
2. Build and package:
   ```bash
   ./scripts/build.sh
   ```
   This runs `zig cc -target aarch64-linux-gnu.2.35 -mcpu=cortex_a72 -O3 -fPIC -shared ...` and writes `build/dsp.so`, `dist/tinyk/` and `TinyK.tar.gz`. On Windows, run it from Git Bash.
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

TinyK contains no Korg code, firmware, samples, or audio recordings, and ships no third-party `.syx` files. Its DSP engine is independently developed. Users are responsible for ensuring they possess the appropriate rights to any third-party `.syx` bank dumps loaded into the device.

The software is provided "as is", without warranty of any kind.
