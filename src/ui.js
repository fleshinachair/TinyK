/**
 * microKORG VA (schwung-mk-va) Move Synth Module UI
 *
 * Handles 128x64 OLED display rendering and 8 hardware encoders
 * across the parameter pages (the same pages as the module.json ui_hierarchy):
 *   PERF:         Category, Program, Arp, Mode (Single / Layer), Cutoff, Resonance, Amp Release, Layer (L1 / L2)
 *   OSC:          Wave1, Control 1 (or the DWGS wave while Wave1 is DWGS), Control 2, Wave2, Sync/Ring, Semi, Tune, Osc Mix
 *   FILTER:       Type, Cutoff, Res, EG Int, Filter Atk/Dcy/Sus/Rel
 *   AMP:          Noise, Level, Distortion, Pan, Amp Atk/Dcy/Sus/Rel
 *   MOD:          LFO1 / LFO2 Rate, Mod Wheel, Filter Key Track, Voice (Mono / Poly / Unison), Portamento, Layer Balance
 *   EFFECTS:      Mod FX Type/Speed/Depth, Delay Type/Time/Feedback/Mix, Master Vol
 *   ARP SETTINGS: Type, Range, Resolution, Gate, Swing, Latch, Key Sync, Target
 *   ARP STEPS:    Step 1..8 of the arpeggiator's pattern (Rest / Play), drawn as the microKORG's 2 x 4 step LEDs
 *                 (hollow = Rest, filled = Play, inverted = sounding, dotted = past the pattern's length), as
 *                 canvas.js draws them on the Schwung param pages
 *   BANK:         the active bank file
 * The per-layer controls edit the layer selected by Layer on Perf (in Layer mode): in Layer mode their labels
 * name it ("L1.CUT" / "L2.CUT", pages marked `layerLabels`) and the sound-design pages (Osc, Filter,
 * Amp, marked `layerBadge`) show [L1] or [L2] in the header. Single mode has one layer: plain labels,
 * [L1], and Layer reads "N/A". "Voice" means polyphony only (Mono / Poly / Unison).
 */

// Cutoff knob -> Hz, the engine's measured mapping (knob_fc in dsp.c): 32.67 Hz doubling every 12.03 knob steps
// to knob 70, then levelling off (the coefficient table, read as Hz at 44.1 kHz)
const cutoffHz = (v) => {
    const k = Math.max(0, Math.min(1, v)) * 127;
    const K = [70, 80, 85, 90, 95, 100, 105, 110], FC = [0.2606, 0.4192, 0.5141, 0.6054, 0.7006, 0.7959, 0.8804, 0.9140];
    let hz;
    if (k < 70) hz = 32.67 * Math.pow(2, k * 10.56 / 127);
    else {
        let fc = FC[7];
        for (let i = 1; i < 8; i++) if (k <= K[i]) { fc = FC[i - 1] + (FC[i] - FC[i - 1]) * (k - K[i - 1]) / (K[i] - K[i - 1]); break; }
        hz = fc * 44100 / (2 * Math.PI);
    }
    return hz >= 1000 ? `${(hz / 1000).toFixed(1)}k` : `${Math.round(hz)}Hz`;
};
const pct = (v) => `${Math.round(v * 100)}%`;
// LFO rate as the engine plays it (LFO_RATE_HZ in dsp.c: the microKORG's curve, measured every 8 knob steps)
const LFO_RATE_HZ = [0.0157, 0.09, 0.1696, 0.2502, 0.8302, 1.7502, 2.7506, 3.75, 4.9978, 8.1793, 12.9755, 20.5917, 32.7045, 51.9048, 82.4155, 130.8, 196.0];
const lfoHz = (v) => {
    const k = Math.max(0, Math.min(1, v)) * 127;
    const i = Math.min(15, Math.floor(k / 8));
    const hz = k >= 120 ? LFO_RATE_HZ[15] + (LFO_RATE_HZ[16] - LFO_RATE_HZ[15]) * (k - 120) / 7
        : LFO_RATE_HZ[i] + (LFO_RATE_HZ[i + 1] - LFO_RATE_HZ[i]) * (k / 8 - i);
    return hz < 10 ? `${hz.toFixed(2)}Hz` : `${Math.round(hz)}Hz`;
};
// Delay Time as the engine plays it (delay_time_s in dsp.c: the microKORG's curve, 14 ms .. 1.64 s; a tempo-synced
// program steps note values instead, which this readout does not show)
const delayMs = (v) => {
    const k = Math.max(0, Math.min(1, v)) * 127;
    const frac = k <= 64 ? 0.01 + 0.15 * k / 64 : k <= 110 ? 0.16 + 0.24 * (k - 64) / 46
        : k <= 120 ? 0.40 + 0.02 * (k - 110) : k <= 125 ? 0.60 + 0.04 * (k - 120) : 1.0 + 0.2 * (k - 126);
    const ms = frac * 65536 / 48;
    return ms < 1000 ? `${Math.round(ms)}ms` : `${(ms / 1000).toFixed(2)}s`;
};
const bipolar = (v) => `${Math.round((v - 0.5) * 200)}%`;
const signed = (unit) => (v) => `${v > 0 ? "+" : ""}${Math.round(v)}${unit}`;

// `index`: an enum the engine reports and takes as an index; `int`: an integer range; `layerLabels`: per-layer controls; `layerBadge`: [L1] / [L2] in the header
export const PAGES = [
    {
        id: "perf",
        name: "PERF",
        layerLabels: true,
        params: [
            // Category = matrix row (genre); Program = the row's 16 patches, A1..A8 then B1..B8
            { key: "category",       label: "Category", short: "Cat",  index: true, values: ["Trance", "Techno", "Electr", "DnB", "Hiphop", "Retro", "SE/Hit"], categories: true },
            // Program shows the full patch code and name ("B.12 ARPEJMATR"), see formatValue
            { key: "patch",          label: "Program",  short: "Prog", index: true, values: ["A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8"], presetName: true, patches: true },
            // the program's arpeggiator (stored on / off until changed here)
            { key: "arp_on",         label: "Arp",     short: "Arp",  values: ["Off", "On"] },
            // Single (one layer, 4 voices) or Layer (two layers, 2 voices each)
            { key: "voice_mode",     label: "Mode",    short: "Mode", values: ["Single", "Layer"] },
            // the live macros: filter cutoff, resonance and amp release of the edited layer
            { key: "cutoff",         label: "Cutoff",  short: "Cut",   l2: "L2.CUT",  format: cutoffHz },
            { key: "resonance",      label: "Res",     short: "Res",   l2: "L2.RES",  format: pct },
            { key: "release2",       label: "AmpRel",  short: "ARel",  l2: "L2.REL",  format: pct },
            // which layer the per-layer controls edit (Layer mode; "N/A" in Single)
            { key: "timbre_edit",    label: "Layer",   short: "Layer", values: ["L1", "L2"] }
        ]
    },
    {
        id: "osc",
        name: "OSC",
        layerLabels: true,
        layerBadge: true,
        params: [
            { key: "wave1",          label: "Wave1",   short: "Wav1", l2: "L2.WV1", index: true, values: ["SAW", "SQR", "TRI", "SIN", "VOX", "DWG", "NZ"] },
            // Osc 1's Control 1 (0..127; what it does depends on the wave); while Wave 1 is DWGS this encoder
            // picks the DWGS waveform (1..64) instead: see slotDef
            { key: "osc1_ctrl1",     label: "Ctrl1",   short: "Ctl1", l2: "L2.CT1", int: [0, 127], format: (v) => `${Math.round(v)}`,
              dwgs: { key: "dwgs_wave", label: "DWGS", short: "DWGS", l2: "L2.DWGS", int: [0, 63], format: (v) => `${Math.round(v) + 1}` } },
            { key: "osc1_ctrl2",     label: "Ctrl2",   short: "Ctl2", l2: "L2.CT2", int: [0, 127], format: (v) => `${Math.round(v)}` },
            { key: "wave2",          label: "Wave2",   short: "Wav2", l2: "L2.WV2", index: true, values: ["SAW", "SQR", "TRI"] },
            // option indices, in the hardware's order (the engine remaps Sync / Ring to its own)
            { key: "sync_ring",      label: "SyncR",   short: "SyncR", l2: "L2.SYNC", index: true, values: ["OFF", "RING", "SYNC", "R.SNC"] },
            { key: "osc2_semi",      label: "Semi",    short: "Semi", l2: "L2.SEMI", int: [-24, 24], format: signed("st") },
            { key: "osc2_tune",      label: "Tune",    short: "Tune", l2: "L2.TUNE", int: [-50, 50], format: signed("ct") },
            { key: "osc_mix",        label: "Mix",     short: "Mix",  l2: "L2.MIX",  format: pct }
        ]
    },
    {
        id: "filter",
        name: "FILTER",
        layerLabels: true,
        layerBadge: true,
        params: [
            { key: "filter_type", label: "Type",   short: "Type",  l2: "L2.FTYP", index: true, values: ["LPF24", "LPF12", "BPF12", "HPF12"] },
            { key: "cutoff",      label: "Cutoff", short: "Cut",   l2: "L2.CUT",  format: cutoffHz },
            { key: "resonance",   label: "Res",    short: "Res",   l2: "L2.RES",  format: pct },
            { key: "env_int",     label: "EG Int", short: "EGInt", l2: "L2.EGIN", format: bipolar },
            { key: "attack1",     label: "FltAtk", short: "FAtk",  l2: "L2.FATK", format: pct },
            { key: "decay1",      label: "FltDcy", short: "FDcy",  l2: "L2.FDCY", format: pct },
            { key: "sustain1",    label: "FltSus", short: "FSus",  l2: "L2.FSU",  format: pct },
            { key: "release1",    label: "FltRel", short: "FRel",  l2: "L2.FRL",  format: pct }
        ]
    },
    {
        id: "amp",
        name: "AMP",
        layerLabels: true,
        layerBadge: true,
        params: [
            { key: "noise_level", label: "Noise",  short: "Noise", l2: "L2.NOIS", format: pct },
            { key: "level",       label: "Level",  short: "Level", l2: "L2.LVL",  format: pct },
            { key: "drive",       label: "Dist",   short: "Dist",  l2: "L2.DIST", index: true, values: ["OFF", "ON"] },
            { key: "pan",         label: "Pan",    short: "Pan",   format: (v) => v < 0.48 ? `L${Math.round((0.5 - v) * 200)}` : (v > 0.52 ? `R${Math.round((v - 0.5) * 200)}` : "C") },
            { key: "attack2",     label: "AmpAtk", short: "AAtk",  l2: "L2.ATK",  format: pct },
            { key: "decay2",      label: "AmpDcy", short: "ADcy",  l2: "L2.ADCY", format: pct },
            { key: "sustain2",    label: "AmpSus", short: "ASus",  l2: "L2.ASU",  format: pct },
            { key: "release2",    label: "AmpRel", short: "ARel",  l2: "L2.REL",  format: pct }
        ]
    },
    {
        id: "mod",
        name: "MOD",
        layerLabels: true,
        params: [
            { key: "lfo1_rate",   label: "LFO1",   short: "LFO1",  format: lfoHz },
            { key: "lfo2_rate",   label: "LFO2",   short: "LFO2",  format: lfoHz },
            // stands in for the mod wheel the Move lacks: virtual patch source 7, same as CC1
            { key: "mod_wheel",   label: "ModWhl", short: "MOD",   int: [0, 127], format: (v) => `${Math.round(v)}` },
            { key: "keytrack",    label: "KeyTr",  short: "KeyTr", l2: "L2.KTRK", format: bipolar },
            // the layer's polyphony (Mono / Poly / Unison), its glide, and the layers' balance
            { key: "voice_assign",   label: "Voice",   short: "Voice", l2: "L2.VOIC", index: true, values: ["Mono", "Poly", "Unison"] },
            { key: "portamento",     label: "Porta",   short: "Porta", l2: "L2.PORT", format: pct },
            { key: "timbre_balance", label: "LayerBal", short: "Bal",  format: (v) => `${Math.round((1 - v) * 100)}:${Math.round(v * 100)}` }
        ]
    },
    {
        id: "fx",
        name: "EFFECTS",
        params: [
            { key: "modfx_type",  label: "ModFX", short: "FX",    index: true, values: ["CHO", "ENS", "PHS"] },
            { key: "modfx_speed", label: "Speed", short: "Speed", format: pct },
            { key: "chorus_mix",  label: "Depth", short: "Depth", format: pct },
            { key: "delay_type",  label: "D.Typ", short: "D.Typ", index: true, values: ["ST", "CRS", "L/R"] },
            { key: "delay_time",  label: "Time",  short: "Time",  format: delayMs },
            { key: "delay_feedback",label: "Fdbk",short: "Fdbk",  format: pct },
            { key: "delay_mix",   label: "D.Mix", short: "D.Mix", format: pct },
            { key: "master_vol",  label: "Vol",   short: "Vol",   format: pct }
        ]
    },
    {
        id: "arpset",
        name: "ARP SETTINGS",
        // the program's arpeggiator; a turn changes the running arpeggio at once
        params: [
            { key: "arp_type",       label: "Type",   short: "Type",  index: true, values: ["UP", "DOWN", "ALT1", "ALT2", "RND", "TRIG"] },
            { key: "arp_range",      label: "Range",  short: "Range", index: true, values: ["1 Oct", "2 Oct", "3 Oct", "4 Oct"] },
            { key: "arp_resolution", label: "Reso",   short: "Reso",  index: true, values: ["1/24", "1/16", "1/12", "1/8", "1/6", "1/4"] },
            { key: "arp_gate",       label: "Gate",   short: "Gate",  int: [0, 100], format: (v) => `${Math.round(v)}%` },
            { key: "arp_swing",      label: "Swing",  short: "Swing", int: [-100, 100], format: (v) => `${v > 0 ? "+" : ""}${Math.round(v)}%` },
            { key: "arp_latch",      label: "Latch",  short: "Latch", index: true, values: ["Off", "On"] },
            { key: "arp_key_sync",   label: "KeySync", short: "KSync", index: true, values: ["Off", "On"] },
            { key: "arp_target",     label: "Target", short: "Targ",  index: true, values: ["Both", "L1", "L2"] }
        ]
    },
    {
        id: "steps",
        name: "ARP STEPS",
        // the arpeggiator's 8-step trigger pattern: each step plays or rests, live; drawn as LEDs (drawStepLeds)
        leds: true,
        params: [1, 2, 3, 4, 5, 6, 7, 8].map((n) => ({ key: `arp_step${n}`, label: `Step${n}`, short: `St${n}`, values: ["Rest", "Play"] }))
    },
    {
        id: "bank",
        name: "BANK",
        params: [
            // 0 = built-in, 1..N = .syx dumps in the module's banks/ folder; shows the file name
            { key: "bank_file",      label: "Bank",    short: "Bank", bank: true }
        ]
    }
];

export class MicroKorgUI {
    constructor(host) {
        this.host = host;
        this.currentPageIndex = 0;
        this.currentPresetIndex = 0;
        this.state = {
            presetName: "A.11 Saw Lead",
            params: {}
        };

        // Initialize default parameter values
        for (const page of PAGES) {
            for (const param of page.params) {
                this.state.params[param.key] = 0.5;
            }
        }
        this.state.params["bank_file"] = 0;
        this.state.params["category"] = 0;
        this.state.params["patch"] = 0;
        this.state.params["voice_mode"] = 0.0;
        this.state.params["voice_assign"] = 1;
        this.state.params["timbre_edit"] = 0;
        this.state.params["timbre_balance"] = 0.5;
    }

    getPresetName() {
        if (this.host && typeof this.host.getParam === "function") {
            const name = this.host.getParam("preset_name");
            if (name) return name;
        }
        return this.state.presetName;
    }

    // Set parameter value scaled 0.0 to 1.0 or raw string
    setParam(key, value) {
        this.state.params[key] = value;
        if (this.host && typeof this.host.setParam === "function") {
            const strVal = (key === "voice_mode")
                ? (value >= 0.5 ? "1.0" : "0.0")
                : value.toString();
            this.host.setParam(key, strVal);
        }
        const name = this.getPresetName();
        if (name) this.state.presetName = name;
    }

    // Get parameter value
    getParam(key) {
        if (this.host && typeof this.host.getParam === "function") {
            const val = this.host.getParam(key);
            if (val !== undefined && val !== null && !isNaN(val)) {
                this.state.params[key] = parseFloat(val);
            }
        }
        return this.state.params[key] ?? 0.5;
    }

    // The control an encoder slot holds right now: the Osc page's third encoder is the DWGS waveform while
    // Wave 1 (index 5) is DWGS, Pulse Width otherwise, as on the host's Osc page
    slotDef(paramDef) {
        return (paramDef && paramDef.dwgs && Math.round(this.getParam("wave1")) === 5) ? paramDef.dwgs : paramDef;
    }

    // The values of an index control: Category follows the active bank (the engine leaves vocoder programs out, so
    // the factory layout has 7 categories, and a category lists as many programs as it has playable ones)
    valuesOf(paramDef) {
        if (paramDef.patches && this.host && typeof this.host.getParam === "function") {
            const n = parseInt(this.host.getParam("patch_count"));
            if (n >= 1 && n <= paramDef.values.length) return paramDef.values.slice(0, n);
        }
        if (paramDef.categories && this.host && typeof this.host.getParam === "function") {
            try {
                const names = JSON.parse(this.host.getParam("category_names"));
                if (Array.isArray(names) && names.length > 0) return names;
            } catch (e) { /* the engine has no category_names: the static list stands */ }
        }
        return paramDef.values;
    }

    // Handle Move encoder turns (encoder 0 to 7)
    onEncoder(index, delta) {
        if (index < 0 || index >= 8) return;
        const page = PAGES[this.currentPageIndex];
        const paramDef = this.slotDef(page.params[index]);
        if (!paramDef) return;

        if (paramDef.key === "bank_file") {
            const count = parseInt(this.host && this.host.getParam ? this.host.getParam("bank_file_count") : 1) || 1;
            const cur = parseInt(this.getParam("bank_file")) || 0;
            this.setParam("bank_file", Math.max(0, Math.min(count - 1, cur + (delta > 0 ? 1 : -1))));
            return;
        }
        if (paramDef.index) {
            const last = this.valuesOf(paramDef).length - 1;
            const cur = parseInt(this.getParam(paramDef.key)) || 0;
            this.setParam(paramDef.key, Math.max(0, Math.min(last, cur + (delta > 0 ? 1 : -1))));
            return;
        }
        if (paramDef.int) {
            const [lo, hi] = paramDef.int;
            const cur = Math.round(this.getParam(paramDef.key)) || 0;
            this.setParam(paramDef.key, Math.max(lo, Math.min(hi, cur + (delta > 0 ? 1 : -1))));
            return;
        }
        if (paramDef.key === "voice_mode") {
            const rawVal = this.getParam("voice_mode");
            const cur = (parseFloat(rawVal) >= 0.5 || rawVal === "Layer" || rawVal === "1") ? 1 : 0;
            const next = Math.max(0, Math.min(1, cur + (delta > 0 ? 1 : -1)));
            this.setParam("voice_mode", next === 1 ? 1.0 : 0.0);
            return;
        }
        if (paramDef.key === "timbre_edit") {
            const cur = parseInt(this.getParam("timbre_edit")) || 0;
            const next = Math.max(0, Math.min(1, cur + (delta > 0 ? 1 : -1)));
            this.setParam("timbre_edit", next);
            return;
        }

        const currentVal = this.getParam(paramDef.key);
        const step = 0.02 * (delta > 0 ? 1 : -1);
        const newVal = Math.max(0.0, Math.min(1.0, currentVal + step));
        this.setParam(paramDef.key, newVal);
    }

    // Handle Page switching & Navigation
    nextPage() {
        this.currentPageIndex = (this.currentPageIndex + 1) % PAGES.length;
    }

    prevPage() {
        this.currentPageIndex = (this.currentPageIndex - 1 + PAGES.length) % PAGES.length;
    }

    nextPreset() {
        this.currentPresetIndex = (this.currentPresetIndex + 1) % 128;
        this.loadPreset(this.currentPresetIndex);
    }

    prevPreset() {
        this.currentPresetIndex = (this.currentPresetIndex - 1 + 128) % 128;
        this.loadPreset(this.currentPresetIndex);
    }

    loadPreset(idx) {
        this.currentPresetIndex = idx;
        if (this.host && typeof this.host.setParam === "function") {
            this.host.setParam("preset", idx.toString());
            const name = this.host.getParam("preset_name");
            if (name) this.state.presetName = name;
        }
    }

    // Handle Move hardware button events
    onButton(button, pressed) {
        if (!pressed) return;
        switch (button) {
            case "left":
                this.prevPage();
                break;
            case "right":
                this.nextPage();
                break;
            case "up":
                this.nextPreset();
                break;
            case "down":
                this.prevPreset();
                break;
            case "encoder_click_0":
            case "page":
                this.nextPage();
                break;
        }
    }

    // Format parameter display value
    formatValue(paramDef, val) {
        if (paramDef.bank) {
            const name = this.host && this.host.getParam ? this.host.getParam("bank_file_name") : null;
            return name ? String(name) : "Built-in";
        }
        if (paramDef.presetName) {
            return this.getPresetName();
        }
        if (paramDef.key === "timbre_edit" && !this.isLayer()) {
            return "N/A"; // Single mode: Layer 2 is not playing
        }
        if (paramDef.values) {
            const values = this.valuesOf(paramDef);
            const num = parseFloat(val);
            let idx = 0;
            if (!isNaN(num)) {
                if (paramDef.index) {
                    idx = Math.round(num);
                } else if (paramDef.key === "voice_mode" || paramDef.key === "timbre_edit") {
                    idx = (num >= 0.5) ? 1 : 0;
                } else if (num >= 1.0) {
                    // Safe clamp so it doesn't display out of bounds
                    idx = values.length - 1;
                } else if (num <= 0.0) {
                    idx = 0;
                } else {
                    // Matches the engine: index = round(value * (count - 1))
                    idx = Math.min(values.length - 1, Math.max(0, Math.round(num * (values.length - 1))));
                }
            } else if (typeof val === "string") {
                const found = values.indexOf(val);
                if (found >= 0) idx = found;
            }
            idx = Math.max(0, Math.min(values.length - 1, idx));
            return values[idx];
        }
        if (typeof paramDef.format === "function") {
            return paramDef.format(val);
        }
        return `${Math.round(val * 100)}%`;
    }

    isLayer() {
        return parseFloat(this.getParam("voice_mode")) >= 0.5;
    }

    // The layer the per-layer controls edit, as the engine routes them: 2 only for Layer 2 in Layer mode
    editLayer() {
        return (this.isLayer() && parseInt(this.getParam("timbre_edit")) === 1) ? 2 : 1;
    }

    // The step lit on the Arp Steps LEDs (-1: none) and the pattern length, from the engine's arp_playhead
    // ("1,<length>,<next>,..." while running, "0,<length>" stopped; the lit step is the one before <next>)
    arpPlayhead() {
        const raw = this.host && this.host.getParam ? this.host.getParam("arp_playhead") : null;
        const f = String(raw || "").split(",").map(Number);
        const len = f[1] >= 1 && f[1] <= 8 ? Math.round(f[1]) : 8;
        if (f[0] !== 1 || !Number.isFinite(f[2])) return { lit: -1, len };
        return { lit: (((Math.round(f[2]) - 1) % 1680) + 1680) % 1680 % len, len };
    }

    // Arp Steps: one LED box per encoder, steps 1-4 on the top row and 5-8 below
    drawStepLeds(display, page) {
        if (typeof display.fill_rect !== "function") return;
        const { lit, len } = this.arpPlayhead();
        const B = 11;
        const outline = (x, y, c) => {
            display.fill_rect(x, y, B, 1, c);
            display.fill_rect(x, y + B - 1, B, 1, c);
            display.fill_rect(x, y + 1, 1, B - 2, c);
            display.fill_rect(x + B - 1, y + 1, 1, B - 2, c);
        };
        for (let i = 0; i < 8; i++) {
            const paramDef = page.params[i];
            const x0 = (i % 4) * 32, y0 = 16 + Math.floor(i / 4) * 24;
            if (typeof display.print === "function") display.print(x0 + 2, y0, paramDef.short);
            const x = x0 + 18, y = y0 + 1;
            const play = parseFloat(this.getParam(paramDef.key)) >= 0.5;
            if (i === lit) {
                display.fill_rect(x - 2, y - 2, B + 4, B + 4, 1);
                if (play) display.fill_rect(x, y, B, B, 0);
                else outline(x, y, 0);
            } else if (i >= len) {
                for (let d = 0; d < B; d += 2) {
                    display.fill_rect(x + d, y, 1, 1, 1);
                    display.fill_rect(x + d, y + B - 1, 1, 1, 1);
                    display.fill_rect(x, y + d, 1, 1, 1);
                    display.fill_rect(x + B - 1, y + d, 1, 1, 1);
                }
            } else if (play) {
                display.fill_rect(x, y, B, B, 1);
            } else {
                outline(x, y, 1);
            }
        }
    }

    // Render OLED Display (128x64 pixels)
    drawUI(display) {
        if (!display) return;

        // Clear display buffer
        if (typeof display.clear === "function") {
            display.clear();
        }

        const page = PAGES[this.currentPageIndex];
        const layer = this.editLayer();

        // 1. Header Bar (y: 0 to 12)
        // Active page with its layer badge ([L1] / [L2] on the sound-design pages), and the current preset
        const currentName = this.getPresetName();
        const badge = page.layerBadge ? ` [L${layer}]` : "";
        const headerText = `${page.name}${badge}  [${currentName}]`;
        if (typeof display.print === "function") {
            display.print(2, 2, headerText);
        }

        // Horizontal separator line under header
        if (typeof display.draw_line === "function") {
            display.draw_line(0, 13, 127, 13);
        }

        if (page.leds) {
            this.drawStepLeds(display, page);
            return;
        }

        // 2. Encoder Slots Grid (8 parameters arranged in 2 rows of 4 columns)
        // Row 1: Encoders 0-3 (y: 16 to 36)
        // Row 2: Encoders 4-7 (y: 38 to 58)
        const colWidth = 32; // 128 / 4 = 32 pixels per column

        for (let i = 0; i < 8; i++) {
            const paramDef = this.slotDef(page.params[i]);
            if (!paramDef) continue;

            const val = this.getParam(paramDef.key);
            const col = i % 4;
            const row = Math.floor(i / 4);

            const x = col * colWidth;
            const y = 16 + row * 24;

            // Parameter short label (e.g. "Wav1", "Cut"; "L1.CUT" / "L2.CUT" in Layer mode)
            if (typeof display.print === "function") {
                const label = (page.layerLabels && paramDef.l2 && this.isLayer()) ? paramDef.l2.replace("L2", `L${layer}`) : paramDef.short;
                display.print(x + 2, y, label);
            }

            // Value label or mini bar
            const valStr = this.formatValue(paramDef, val);
            if (typeof display.print === "function") {
                display.print(x + 2, y + 10, valStr);
            }

            // Mini bar indicator (selector controls are drawn as their position in the range)
            if (typeof display.fill_rect === "function") {
                let frac = val;
                if (paramDef.index) frac = val / (this.valuesOf(paramDef).length - 1);
                else if (paramDef.int) frac = (val - paramDef.int[0]) / (paramDef.int[1] - paramDef.int[0]);
                else if (paramDef.bank) {
                    const count = parseInt(this.host && this.host.getParam ? this.host.getParam("bank_file_count") : 1) || 1;
                    frac = count > 1 ? val / (count - 1) : 0;
                }
                const barWidth = Math.max(1, Math.round(Math.max(0, Math.min(1, frac)) * (colWidth - 4)));
                display.fill_rect(x + 2, y + 20, barWidth, 2);
            }
        }
    }
}

// Schwung Standalone / Move UI Factory Function
export function createSoundGeneratorUI(host) {
    const ui = new MicroKorgUI(host);
    return {
        showOctave: true,
        pages: PAGES,
        presets: [],
        onEncoder: (idx, delta) => ui.onEncoder(idx, delta),
        onButton: (btn, pressed) => ui.onButton(btn, pressed),
        drawUI: (display) => ui.drawUI(display),
        render: (display) => ui.drawUI(display)
    };
}

export default createSoundGeneratorUI;
