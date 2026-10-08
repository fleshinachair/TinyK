# TinyK User Guide

TinyK is a microKORG-style virtual analog synth for the Ableton Move (Schwung). It has **4 voices** and two modes:

- **Single**: one layer plays all 4 voices.
- **Layer**: two layers, **L1** and **L2**, play 2 voices each. Per-layer controls edit whichever layer **LAYER** (Perf, encoder 8) points at.

**Voice** always means polyphony (Mono, Poly, Unison). A **layer** is one of the two sounds in Layer mode.

## How to read the pages

The Move's display shows a header (page name and the current patch) above a 2 x 4 grid of encoder cells. Each cell has a label, a value and a small bar. The number in brackets under each cell is its encoder (1-4 on the top row, 5-8 below).

- **Jog wheel**: browse all the bank's programs (112 in the built-in bank). **Shift+Click**: open the page picker and jump to any page. **Shift+jog**: step through pages.
- The sound-design pages (**Osc**, **Envelopes**, **Mix/Filter**) show `[L1]` or `[L2]` in the header; in Layer mode their labels read `L1.CUT` / `L2.CUT` and so on.
- Per-layer values are stored separately, so flipping LAYER between L1 and L2 never changes the other layer.
- Several Move slots can each run their own TinyK; each keeps its own program, bank and edits.

| # | Page | Encoders |
|---|------|----------|
| 1 | [Perf](#1-perf) | Cat, Prog, Arp, Mode, Cut, Res, Rel, Layer |
| 2 | [Osc](#2-osc) | Wave 1, Wave 2, Pulse Width, Semi, Tune, Voice, Layer Bal, Mod Wheel |
| 3 | [Envelopes](#3-envelopes) | Filter A/D/S/R, Amp D/S, Key Track, EG Int |
| 4 | [Mix/Filter](#4-mixfilter) | Osc Mix, Noise, Sync/Ring, Filter Type, Drive, Level, Portamento, Amp Atk |
| 5 | [Effects](#5-effects) | Chorus, Delay Time/Fdbk/Mix, Master Vol, Pan, LFO1/LFO2 rate |
| 6 | [Arp Settings](#6-arp-settings) | Type, Range, Resolution, Gate, Swing, Latch, Key Sync, Target |
| 7 | [Arp Steps](#7-arp-steps) | Steps 1-8 (Rest / Play) |
| 8 | [Bank](#8-bank) | Bank |

---

## 1. Perf

Everything you reach for while playing: pick a sound, switch the arpeggiator and layer mode, and shape the sound with the three macros. The jog wheel also steps through all the programs of the bank (112 in the built-in bank).

```
+-----------------------------------------------+
| PERF  [A.11 Trancey]                          |
+-----------+-----------+-----------+-----------+
| Cat       | Prog      | Arp       | Mode      |
| Trance    | A1        | Off       | Single    |
| =         | =         | =         | =         |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| Cut       | Res       | Rel       | Layer     |
| 7.2k      | 20%       | 20%       | N/A       |
| ======    | ==        | ==        |           |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | CAT | Category | 7 choices: Trance, Techno, Electr, DnB, Hiphop, Retro, SE/Hit. Each is one row of the program matrix (16 programs). Vocoder programs are not offered; banks laid out differently are split into groups of 16 (`P.001-016` ...). | Changing it keeps the Program position, so A1 in Trance becomes A1 in Techno. |
| 2 | PROG | Program | A1-A8, then B1-B8 inside the category. Touching or turning shows the full patch code and name, e.g. `B.12 ARPEJMATR`. | Category and Program together address every program of the bank; the jog wheel steps through them in order. |
| 3 | ARP | Arpeggiator on / off | Off / On. Each program stores its own arpeggio; this overrides it until you pick another program. | Shape the pattern on the Arp Settings and Arp Steps pages. |
| 4 | MODE | Single / Layer | Single: one layer plays all 4 voices. Layer: two layers (L1, L2) play 2 voices each, blended by Layer Bal on Osc. | Switching mode cuts any sounding notes (a short fade, no click). Programs set their own mode when loaded. |
| 5 | CUT | Filter cutoff | 0-100 % (about 37 Hz up to the 19 kHz ceiling), shown in Hz / kHz. In Layer mode the label reads `L1.CUT` or `L2.CUT`. | Edits the layer chosen by LAYER (encoder 8). |
| 6 | RES | Filter resonance | 0-100 %. Label `L1.RES` / `L2.RES` in Layer mode. | Low settings are flat; high settings ring around the cutoff. |
| 7 | REL | Amp envelope release | 0-100 %: how long a note fades after the key is released. Label `L1.REL` / `L2.REL` in Layer mode. | Amp Attack is on Mix/Filter, Amp Decay / Sustain on Envelopes. |
| 8 | LAYER | Which layer the per-layer knobs edit | Layer mode: `L1` or `L2`. Single mode: reads `N/A` (there is only one layer). | The choice is remembered while you flip between Single and Layer. |

- Make a sound brighter or darker without leaving this page: Cut and Res are the two knobs you will use most.
- In Layer mode, set LAYER to L2, tweak Cut / Res / Rel, then switch back: L1 keeps its own values.

---

## 2. Osc

The oscillators of the layer being edited (the header shows `[L1]` or `[L2]`; in Layer mode the labels become `L1.WV1` / `L2.WV1` and so on).

```
+-----------------------------------------------+
| OSC [L1]  [A.11 Trancey]                      |
+-----------+-----------+-----------+-----------+
| Wav1      | Wav2      | PulW      | Semi      |
| Saw       | Saw       | 50%       | 0st       |
| =         | =         | =         | ====      |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| Tune      | Voice     | Bal       | MOD       |
| 0ct       | Poly      | 50:50     | 0         |
| ====      | ====      | ====      | =         |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | WAV1 | Oscillator 1 wave | Saw, Square, Triangle, Sine, Vox, DWGS, Noise (the display draws the wave). | Vox and DWGS are additive approximations of the microKORG's sampled waves. |
| 2 | WAV2 | Oscillator 2 wave | Saw, Square, Triangle. | Pair with Semi / Tune below, and with Sync / Ring on Mix/Filter. |
| 3 | PULW | Pulse width | 0-100 % of the knob (displayed as 50-95 % duty). Audible on the Square wave. | Set it by ear: narrower pulses sound thinner and more nasal. |
| 4 | SEMI | Osc 2 semitones | -24 to +24 st relative to Osc 1. | +7 gives a fifth, +12 an octave. |
| 5 | TUNE | Osc 2 fine tune | -50 to +50 cents. | A few cents of detune thickens Saw + Saw. |
| 6 | VOICE | Voice assign (polyphony) | Mono (one note at a time), Poly (each key its own voice), Unison (all of the layer's voices stacked on one note, detuned and spread across the stereo field). | The unison detune amount comes from the program. "Voice" always means polyphony, never layers. |
| 7 | BAL | Layer balance | Shown as L1:L2, from 100:0 to 0:100. | Only matters in Layer mode. |
| 8 | MOD | Mod wheel | 0-127. Stands in for the wheel the Move lacks and drives the same patch source as MIDI CC1; whichever moved last wins. | Only patches that route the Mod Wheel respond. |

- Give L1 and L2 different waves, then blend them with Bal for layered sounds.

---

## 3. Envelopes

The filter envelope (EG1) and the amp envelope's decay and sustain, plus how the filter follows the keyboard and the envelope. Per layer: the header shows `[L1]` or `[L2]`, and labels read `L1.FATK` and so on in Layer mode.

```
+-----------------------------------------------+
| ENVELOPES [L1]  [A.11 Trancey]                |
+-----------+-----------+-----------+-----------+
| FAtk      | FDcy      | FSus      | FRel      |
| 1%        | 40%       | 50%       | 20%       |
| =         | ====      | ====      | ==        |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| ADcy      | ASus      | KeyTr     | EGInt     |
| 40%       | 80%       | 0%        | 0%        |
| ====      | =======   | ====      | ====      |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | FATK | Filter EG attack | 0-100 %. | A long attack opens the filter slowly after the key press. |
| 2 | FDCY | Filter EG decay | 0-100 %. | Time to fall from the peak to the sustain level. |
| 3 | FSUS | Filter EG sustain | 0-100 %. | Level held while the key is down. |
| 4 | FREL | Filter EG release | 0-100 %. | How the filter closes after the key is released. |
| 5 | ADCY | Amp EG decay | 0-100 %. | Short decay with low sustain gives plucks. |
| 6 | ASUS | Amp EG sustain | 0-100 %. | 100 % holds full level while the key is down. |
| 7 | KEYTR | Filter key tracking | -100 % to +100 %; 0 % is off (the centre of the knob). | Positive values open the filter as you play higher. |
| 8 | EGINT | Filter EG intensity | -100 % to +100 %; 0 % is neutral, negative inverts the envelope. | Combine with a moderate Cut and some Res for a classic sweep. |

- Amp Attack is on Mix/Filter and Amp Release on Perf.

---

## 4. Mix/Filter

Oscillator balance, filter type, drive and output level of the layer being edited, plus portamento and amp attack. Labels read `L1.MIX` ... `L2.PORT`, `L1.ATK` in Layer mode.

```
+-----------------------------------------------+
| MIX/FILTER [L1]  [A.11 Trancey]               |
+-----------+-----------+-----------+-----------+
| Mix       | Noise     | SyncR     | Type      |
| 50%       | 0%        | OFF       | LPF24     |
| ====      | =         | =         | =         |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| Drive     | Level     | Porta     | Atk       |
| 0%        | 90%       | 0%        | 1%        |
| =         | ========  | =         | =         |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | MIX | Osc mix | 0-100 %: 0 = Osc 1 only, 50 % = both at full level, 100 % = Osc 2 only. | The centre is the usual starting point. |
| 2 | NOISE | Noise level | 0-100 %. | A little for breathy leads, a lot for hats and effects. |
| 3 | SYNCR | Sync / Ring | Off, Ring, Sync, R.SNC (ring + sync). | Sync with a detuned Osc 2 gives the tearing lead sound; sweep Semi on Osc. |
| 4 | TYPE | Filter type | LPF24, LPF12, BPF12, HPF12. | LPF24 is the classic low-pass; HPF12 thins a sound out. |
| 5 | DRIVE | Distortion | 0-100 %, after the filter. | Mostly adds level and a little grit. |
| 6 | LEVEL | Layer level | 0-100 %. | Balance L1 against L2 with this as well as Bal on Osc. |
| 7 | PORTA | Portamento | 0-100 %: glide time between notes. | Most useful with Voice set to Mono. |
| 8 | ATK | Amp attack | 0-100 %. | Short for plucks, long for pads. |

- Amp Release is on Perf (REL); Amp Decay and Sustain are on Envelopes.

---

## 5. Effects

Effects and master section, shared by both layers.

```
+-----------------------------------------------+
| EFFECTS  [A.11 Trancey]                       |
+-----------+-----------+-----------+-----------+
| Chor      | Time      | Fdbk      | D.Mix     |
| 0%        | 300ms     | 30%       | 0%        |
| =         | ===       | ===       | =         |
|    (1)    |    (2)    |    (3)    |    (4)    |
+-----------+-----------+-----------+-----------+
| Vol       | Pan       | LFO1      | LFO2      |
| 80%       | C         | 5.0Hz     | 5.0Hz     |
| =======   | ====      | =====     | =====     |
|    (5)    |    (6)    |    (7)    |    (8)    |
+-----------+-----------+-----------+-----------+
```

| Enc | Label | Controls | Range / values | Tip |
|-----|-------|----------|----------------|-----|
| 1 | CHOR | Mod FX depth | 0-100 %; 0 = off. The effect type (Chorus/Flanger, Ensemble or Phaser) and its speed come from the program. | Wide and shimmery above about 50 %. |
| 2 | TIME | Delay time | 0-1000 ms. In programs with a tempo-synced delay the knob steps through note values (1/32 ... 1/1) at the Set tempo. | Delay Mix at 0 % means the delay is off. |
| 3 | FDBK | Delay feedback | 0-100 %. | Above 80 % the repeats run on for a long time. |
| 4 | D.MIX | Delay mix | 0-100 %. | A send added to the dry signal. |
| 5 | VOL | Master volume | 0-100 %. | The voice mix is soft-clipped, so stacked voices will not clip digitally. |
| 6 | PAN | Master pan | L100 ... C ... R100. | The layers' own pans from the program are kept. |
| 7 | LFO1 | LFO 1 rate | About 0.05 Hz to 30 Hz. | Has no effect when the program's LFO is tempo-synced. |
| 8 | LFO2 | LFO 2 rate | About 0.05 Hz to 30 Hz. | What the LFOs modulate is set by the program's virtual patches. |

---

## 6. Arp Settings

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

## 7. Arp Steps

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

## 8. Bank

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
