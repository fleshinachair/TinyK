# TinyK User Guide

TinyK is a microKORG-style virtual analog synth for the Ableton Move (Schwung). It has **8 voices** and two modes:

- **Single**: one layer plays all 8 voices.
- **Layer**: two layers, **L1** and **L2**, play 4 voices each. Per-layer controls edit whichever layer **LAYER** (Perf, encoder 8) points at.

**Voice** always means polyphony (Mono, Poly, Unison). A **layer** is one of the two sounds in Layer mode.

## How to read the pages

The Move's display shows a header (page name and the current patch) above a 2 x 4 grid of encoder cells. Each cell has a label, a value and a small bar. The number in brackets under each cell is its encoder (1-4 on the top row, 5-8 below).

- **Jog wheel**: browse all the bank's programs (112 in the built-in bank). **Shift+Click**: open the page picker and jump to any page. **Shift+jog**: step through pages.
- The sound-design pages (**Osc**, **Filter**, **Amp**) show `[L1]` or `[L2]` in the header; in Layer mode their labels read `L1.CUT` / `L2.CUT` and so on.
- Per-layer values are stored separately, so flipping LAYER between L1 and L2 never changes the other layer.
- Several Move slots can each run their own TinyK; each keeps its own program, bank and edits.

| # | Page | Encoders |
|---|------|----------|
| 1 | [Perf](#1-perf) | Cat, Prog, Arp, Mode, Layer, Voice, Porta, Layer Bal |
| 2 | [Osc](#2-osc) | Wave 1, Control 1 / DWGS wave, Control 2, Wave 2, Sync/Ring, Semi, Tune, Osc Mix |
| 3 | [Filter](#3-filter) | Type, Cutoff, Res, EG Int, Filter A/D/S/R |
| 4 | [Amp](#4-amp) | Noise, Level, Distortion, Pan, Amp A/D/S/R |
| 5 | [Mod](#5-mod) | LFO1 rate, LFO2 rate, Mod Wheel, Filter Key Track |
| 6 | [Effects](#6-effects) | Mod FX Type/Speed/Depth, Delay Type/Time/Fdbk/Mix, Master Vol |
| 7 | [Arp Settings](#7-arp-settings) | Type, Range, Resolution, Gate, Swing, Latch, Key Sync, Target |
| 8 | [Arp Steps](#8-arp-steps) | Steps 1-8 (Rest / Play) |
| 9 | [Bank](#9-bank) | Bank |

---

## 1. Perf

Everything about the program as a whole: pick a sound, switch the arpeggiator and layer mode, choose the layer you are editing, and set how it plays (polyphony, glide, layer balance). The jog wheel also steps through all the programs of the bank.

```
+-----------------------------------------------+
| PERF  [A.11 Trancey]                          |
+-----------+-----------+-----------+-----------+
| Cat       | Prog      | Arp       | Mode      |
| Trance    | A1        | Off       | Single    |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| Layer     | Voice     | Porta     | Bal       |
| N/A       | Poly      | 0%        | 50:50     |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | CAT | Category | 7 choices: Trance, Techno, Electr, DnB, Hiphop, Retro, SE/Hit. Each is one row of the program matrix (up to 16 programs). Vocoder programs are not offered, so a category may hold fewer than 16, and a bank without vocoders in the last row gets an eighth category, `Other`. | Changing it keeps the Program position, so A1 in Trance becomes A1 in Techno. |
| 2 | PROG | Program | The category's programs, A1-A8 then B1-B8, each with its own matrix code (a skipped vocoder leaves a gap in the codes). Touching or turning shows the full patch code and name, e.g. `B.12 ARPEJMATR`. | Category and Program together address every program of the bank; the jog wheel steps through them in order. |
| 3 | ARP | Arpeggiator on / off | Off / On. Each program stores its own arpeggio; this overrides it until you pick another program. | Shape the pattern on the Arp Settings and Arp Steps pages. |
| 4 | MODE | Single / Layer | Single: one layer plays all 8 voices. Layer: two layers (L1, L2) play 4 voices each, blended by Layer Bal on this page. | Switching mode cuts any sounding notes (a short fade, no click). Programs set their own mode when loaded. |
| 5 | LAYER | Which layer the per-layer knobs edit | Layer mode: `L1` or `L2`. Single mode: reads `N/A` (there is only one layer). | The choice is remembered while you flip between Single and Layer. |
| 6 | VOICE | Voice assign (polyphony) | Mono (one note at a time), Poly (each key its own voice), Unison (four voices stacked on one note, detuned). Per layer. | The unison detune amount comes from the program. "Voice" always means polyphony, never layers. |
| 7 | PORTA | Portamento | 0-100 %: glide time between notes. | Most useful with Voice set to Mono. |
| 8 | BAL | Layer balance | Shown as L1:L2, from 100:0 to 0:100. | Only matters in Layer mode. |

- In Layer mode, set LAYER to L2 and every per-layer knob on the Osc, Filter and Amp pages edits Layer 2; L1 keeps its own values.

---

## 2. Osc

The two oscillators and how they are combined. Control 1 and Control 2 are Oscillator 1's two wave controls, as on the microKORG: what they do depends on Wave 1.

```
+-----------------------------------------------+
| OSC [L1]  [A.11 Trancey]                      |
+-----------+-----------+-----------+-----------+
| Wave1     | Ctl1      | Ctl2      | Wave2     |
| SAW       | 0         | 0         | SAW       |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| SyncR     | Semi      | Tune      | Mix       |
| OFF       | 0st       | 0ct       | 50%       |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | WAV1 | Oscillator 1 wave | Saw, Square, Triangle, Sine, Vox, DWGS, Noise (the display draws the wave). | Vox is the microKORG's formant wave: Control 1 moves its formant. DWGS plays one of 64 digital waveforms, chosen with encoder 3. |
| 2 | CTL1 | Osc 1 Control 1, or the DWGS waveform while Wave 1 is DWGS | 0-127. Saw: adds a second, phase-shifted saw. Square: pulse width. Triangle: folds the wave into brighter shapes. Sine: cross-modulation by Osc 2. Vox: formant, from about 300 Hz up to 4.5 kHz. Noise: the noise's own low-pass cutoff. With DWGS the knob reads `DWGS` and picks one of the 64 waves, shown by number and name. | On Vox and Noise this is the main tone control. |
| 3 | CTL2 | Osc 1 Control 2 | 0-127. Saw, Square, Triangle, Vox, Sine: how much LFO 1 moves Control 1. Noise: the resonance of the noise's low-pass. | Pulse-width modulation is Square + Control 2. |
| 4 | WAV2 | Oscillator 2 wave | Saw, Square, Triangle. | Pair with Semi / Tune, and with Sync / Ring next to it. |
| 5 | SYNCR | Sync / Ring | Off, Ring, Sync, R.SNC (ring + sync). | Sync with a detuned Osc 2 gives the tearing lead sound; sweep Semi. |
| 6 | SEMI | Osc 2 semitones | -24 to +24 st relative to Osc 1. | +7 gives a fifth, +12 an octave. |
| 7 | TUNE | Osc 2 fine tune | -50 to +50 cents. | A few cents of detune thickens Saw + Saw. |
| 8 | MIX | Osc mix | 0-100 %: 0 = Osc 1 only, 50 % = both at full level, 100 % = Osc 2 only. | The centre is the usual starting point. |

- A few DWGS waves (SynSine3, Digi2, Digi8, Endless, Bell3, Bell4) carry tones below or between the note's harmonics, as on the hardware.

---

## 3. Filter

The filter and its envelope on one page.

```
+-----------------------------------------------+
| FILTER [L1]  [A.11 Trancey]                   |
+-----------+-----------+-----------+-----------+
| Type      | Cut       | Res       | EGInt     |
| LPF24     | 7.2k      | 20%       | 0%        |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| FAtk      | FDcy      | FSus      | FRel      |
| 1%        | 40%       | 50%       | 20%       |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | TYPE | Filter type | LPF24, LPF12, BPF12, HPF12. | LPF24 is the classic low-pass; HPF12 thins a sound out. |
| 2 | CUT | Filter cutoff | 0-100 % (about 37 Hz up to the 19 kHz ceiling), shown in Hz / kHz. In Layer mode the label reads `L1.CUT` or `L2.CUT`. | Edits the layer chosen by LAYER (encoder 8). |
| 3 | RES | Filter resonance | 0-100 %. Label `L1.RES` / `L2.RES` in Layer mode. | Low settings are flat; high settings ring around the cutoff. |
| 4 | EGINT | Filter EG intensity | -100 % to +100 %; 0 % is neutral, negative inverts the envelope. | Combine with a moderate Cut and some Res for a classic sweep. |
| 5 | FATK | Filter EG attack | 0-100 %. | A long attack opens the filter slowly after the key press. |
| 6 | FDCY | Filter EG decay | 0-100 %. | Time to fall from the peak to the sustain level. |
| 7 | FSUS | Filter EG sustain | 0-100 %. | Level held while the key is down. |
| 8 | FREL | Filter EG release | 0-100 %. | How the filter closes after the key is released. |

- EG Int sets how far the envelope moves the cutoff; with it at 0 % the four envelope knobs do nothing.
- Filter key tracking is on the Mod page.

---

## 4. Amp

Level, distortion and the amp envelope. Noise is here too: it is the mixer's third source next to the two oscillators.

```
+-----------------------------------------------+
| AMP [L1]  [A.11 Trancey]                      |
+-----------+-----------+-----------+-----------+
| Noise     | Level     | Dist      | Pan       |
| 0%        | 90%       | OFF       | C         |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| AAtk      | ADcy      | ASus      | ARel      |
| 1%        | 50%       | 80%       | 20%       |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | NOISE | Noise level | 0-100 %. | A little for breathy leads, a lot for hats and effects. |
| 2 | LEVEL | Layer level | 0-100 %. | Balance L1 against L2 with this as well as Layer Bal on Perf. |
| 3 | DIST | Distortion | Off / On (the microKORG's distortion is a switch). A hard clipper after the amp. | Level and the amp envelope are its drive: turn Level down for a cleaner, quieter sound, up for a squarer one. The low-pass and band-pass types hit it harder than the high-pass. |
| 4 | PAN | Master pan | L100 ... C ... R100. The whole instrument, not one layer: the layers' own pans from the program are kept. | The layers' own pans from the program are kept. |
| 5 | AATK | Amp EG attack | 0-100 %. | Short for plucks, long for pads. |
| 6 | ADCY | Amp EG decay | 0-100 %. | Short decay with low sustain gives plucks. |
| 7 | ASUS | Amp EG sustain | 0-100 %. | 100 % holds full level while the key is down. |
| 8 | AREL | Amp EG release | 0-100 %: how long a note fades after the key is released. Label `L1.REL` / `L2.REL` in Layer mode. | Long for pads, short for tight basses. |

- Short attack and decay with sustain at 0 % gives plucks; a long attack and release gives pads.

---

## 5. Mod

The modulation sources you can reach from the Move. What the LFOs and the mod wheel modulate is set by the program's virtual patches.

```
+-----------------------------------------------+
| MOD  [A.11 Trancey]                           |
+-----------+-----------+-----------+-----------+
| LFO1      | LFO2      | MOD       | KeyTr     |
| 1.2Hz     | 1.2Hz     | 0         | 0%        |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | LFO1 | LFO 1 rate | About 0.05 Hz to 30 Hz. | Has no effect when the program's LFO is tempo-synced. |
| 2 | LFO2 | LFO 2 rate | About 0.05 Hz to 30 Hz. | What the LFOs modulate is set by the program's virtual patches. |
| 3 | MOD | Mod wheel | 0-127. Stands in for the wheel the Move lacks and drives the same patch source as MIDI CC1; whichever moved last wins. | Only patches that route the Mod Wheel respond. |
| 4 | KEYTR | Filter key tracking | -100 % to +100 %; 0 % is off (the centre of the knob). | Positive values open the filter as you play higher. |

- Only four encoders are used on this page.

---

## 6. Effects

The program's two effects and the master volume. These are shared by both layers.

```
+-----------------------------------------------+
| EFFECTS  [A.11 Trancey]                       |
+-----------+-----------+-----------+-----------+
| ModFX     | Speed     | Depth     | D.Typ     |
| CHO       | 30%       | 0%        | ST        |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| Time      | Fdbk      | D.Mix     | Vol       |
| 218ms     | 30%       | 0%        | 80%       |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | FX | Mod FX type | Chorus/Flanger, Ensemble, Phaser. | A program loads its own type; this changes it. |
| 2 | SPEED | Mod FX speed | 0-100 %: the effect's LFO rate. | Slow for Ensemble pads, faster for a vibrato-like chorus. |
| 3 | DEPTH | Mod FX depth | 0-100 %; 0 = off. | Wide and shimmery above about 50 %. |
| 4 | D.TYP | Delay type | Stereo (each side repeats on its own side), Cross (repeats swap sides), L/R (repeats alternate left and right). |  |
| 5 | TIME | Delay time | 14 ms to 1.64 s on the microKORG's own curve (218 ms at the centre; the top of the knob covers the long times). In programs with a tempo-synced delay the knob steps through note values (1/32 ... 1/1) at the Set tempo. | Delay Mix at 0 % means the delay is off. |
| 6 | FDBK | Delay feedback | 0-100 %. | Above 80 % the repeats run on for a long time. A program's delay type (Stereo, Cross, or L/R ping-pong) comes from the program. |
| 7 | D.MIX | Delay mix | 0-100 %. | A send added to the dry signal. |
| 8 | VOL | Master volume | 0-100 %. | The voice mix is soft-clipped, so stacked voices will not clip digitally. |

- The program EQ plays as stored but has no knobs.

---

## 7. Arp Settings

The arpeggiator. Every turn changes the running arpeggio at once, and everything is saved with the slot. Switch the arpeggiator on with ARP on Perf.

```
+-----------------------------------------------+
| ARP SETTINGS  [A.21 AutoHouse]                |
+-----------+-----------+-----------+-----------+
| Type      | Range     | Reso      | Gate      |
| UP        | 1 Oct     | 1/16      | 80%       |
| =         | =         | ==        | =======   |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| Swing     | Latch     | KSync     | Targ      |
| 0%        | Off       | On        | Both      |
| ====      | =         | ========= | =         |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | TYPE | Pattern type | UP, DOWN, ALT1, ALT2, RND, TRIG (all held keys together). | ALT1 ends once at the top, ALT2 twice. |
| 2 | RANGE | Octave range | 1-4 Oct. | More octaves make a longer run. |
| 3 | RESO | Resolution | 1/24, 1/16, 1/12, 1/8, 1/6, 1/4 at the Set tempo (the Move's tempo). | Locks to the transport's beat grid while it plays. |
| 4 | GATE | Gate length | 0-100 % of a step (100 % = until the next step). | Short gates are staccato. |
| 5 | SWING | Swing | -100 to +100 %. | Shifts every second step. |
| 6 | LATCH | Latch | Off / On: keep sounding after you let go; the next key replaces the set. | Handy for hands-free patterns. |
| 7 | KSYNC | Key sync | Off / On: restart the pattern and key-synced LFOs on each new key press. | Off locks the pattern to the beat grid while the transport runs. |
| 8 | TARG | Target layer | Both, L1, L2 (matters in Layer mode). | The other layer plays the keys normally, e.g. an L1 bass arpeggio under a held L2 drum. |

---

## 8. Arp Steps

The arpeggiator's trigger pattern, drawn as the microKORG's two rows of four step LEDs. Turning an encoder toggles that step.

```
+-----------------------------------------------+
| ARP STEPS  [A.21 AutoHouse]                   |
+-----------+-----------+-----------+-----------+
| St1       | St2       | St3       | St4       |
| [#]       | [ ]       | [#]       | [#]       |
|           |           |           |           |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| St5       | St6       | St7       | St8       |
| [ ]       | [#]       | [ ]       | [#]       |
|           |           |           |           |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

On the real display each cell is a small picture above its label instead of text. Here `[#]` stands for a filled (Play) box and `[ ]` for a hollow (Rest) box.

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1-4 | ST1-ST4 | Steps 1-4 (top row) | Rest / Play. | Hollow box = Rest, filled box = Play. |
| 5-8 | ST5-ST8 | Steps 5-8 (bottom row) | Rest / Play. | The sounding step is drawn inverted (a lit block with the box cut out). Steps beyond the pattern length are dotted; turning one extends the pattern. |

- The canvas interpolates between Schwung reads using local step-time prediction, keeping the playhead in sync with every 16th step in real time.

---

## 9. Bank

Which bank of programs is loaded.

```
+-----------------------------------------------+
| BANK  [A.11 Trancey]                          |
+-----------+-----------+-----------+-----------+
| Bank      |           |           |           |
| Built-in  |           |           |           |
|           |           |           |           |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
|           |           |           |           |
|           |           |           |           |
|           |           |           |           |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | BANK | Bank | `Built-in` or any microKORG / MS2000 `.syx` bank found in the module's `banks/` folder when the slot started. | Changing bank keeps the current program number. |
| - | Browse banks | Bank list | A menu entry under the Bank knob, not an encoder. Select it on the Bank page and click the jog wheel to open the list; turn the jog wheel to highlight a bank and click to pick it. You return to the main page afterwards. | Turning the Bank knob (encoder 1) switches banks without opening the list. Put `.syx` files in `banks/` on the Move and reload the slot to see them. |

---

## Quick tips

- Start from a program in Perf, then use Cut / Res / Rel to shape it before diving into the other pages.
- For layered sounds: set MODE to Layer, set LAYER to L2, change its wave on Osc, then blend with Layer Bal.
- Programs that load their own arpeggio start with ARP on; turn it off on Perf if you want to play them directly.
