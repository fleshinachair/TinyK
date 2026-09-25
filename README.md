# TinyK

A polyphonic virtual analog (VA) synthesizer module for Ableton Move running the **Schwung / Move-Anything** environment.

Inspired by classic early-2000s dual-oscillator VA hardware (such as the microKORG), **TinyK** delivers authentic vintage leads, snappy resonant basslines, and lush layered pads tailored specifically for Move's 8-encoder interface and Cortex-A72 hardware.

---

## Features

- **4-Voice Polyphonic VA Engine:** Band-limited PolyBLEP oscillators with Ring Modulation and Hard Sync modes.
- **Dual-Timbre Architecture:** Toggle between **Single Mode** (4-voice polyphony) and **Layer Mode** (2-voice stacked dual-timbre).
- **Nonlinear Resonant Filter:** 2-pole / 4-pole low-pass and high-pass modeling with pre-filter drive and analog-style saturation feedback.
- **Exponential Envelopes:** Snappy analog-curve Filter (EG1) and Amp (EG2) stages.
- **Onboard Stereo Multi-FX:** Authentic Ensemble/Chorus and dark-decay analog stereo delay.
- **128-Preset Bank:** Flat 8-genre navigation organized into 16 programs each, with original coordinate mapping (`A.11`–`B.88`) for instant patch referencing.

---

## Hardware Navigation (Move Controls)

| Control | Function | Description |
| :--- | :--- | :--- |
| **Encoder 1** | **Genre** | Selects 1 of 8 musical categories (Trance, Techno, DnB, Vintage, etc.) |
| **Encoder 2** | **Program** | Selects patch `1`–`16` (`1`–`8` = Bank A, `9`–`16` = Bank B) |
| **Encoder 3** | **Voice Mode** | Switch between `Single (4-Voice)` and `Layer (2-Voice)` |
| **Encoder 4** | **Cutoff** | Filter cutoff frequency |
| **Encoder 5** | **Resonance** | Filter resonance (self-oscillating with saturation) |
| **Encoder 6** | **Drive** | Pre-filter saturation amount |
| **Encoder 7** | **Amp Attack** | Amp envelope attack time |
| **Encoder 8** | **Amp Release** | Amp envelope release time |

*Secondary pages expose full oscillator shapes, pulse width, sync/ring mod, filter EG routing, and FX controls.*

---

## Installation

### Via Schwung Web UI (Recommended)
1. Ensure your Move is powered on and connected to your local network.
2. Open your browser and navigate to:  
   `http://move.local:7700`
3. Go to the **Modules** tab.
4. Click **Upload / Install** and select `tinyk.tar.gz`.

### Via Terminal (SSH)
```bash
scp tinyk.tar.gz ableton@move.local:/data/UserData/move-anything/modules/sound_generators/
ssh ableton@move.local
cd /data/UserData/move-anything/modules/sound_generators/
tar -xzf tinyk.tar.gz
rm tinyk.tar.gz
sudo systemctl restart move-anything