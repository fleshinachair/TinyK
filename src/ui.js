/**
 * microKORG VA (schwung-mk-va) Move Synth Module UI
 *
 * Handles 128x64 OLED display rendering and 8 hardware encoders
 * across the parameter pages (the same pages as the module.json ui_hierarchy):
 *   PERF:         Category, Program, Cutoff, Res, Amp Attack, Amp Release, Arp, Mod Wheel
 *   ARP SETTINGS: Type, Range, Resolution, Gate, Swing, Latch, Key Sync, Target
 *   ARP STEPS:    Step 1..8 of the arpeggiator's pattern (Rest / Play)
 *   OSC / TIMBRE: Wave1, Pulse Width, Wave2, Semi, Tune, Voice Mode, Timbre Edit, Timbre Balance
 *   ENVELOPES:    Filter Atk/Dcy/Sus/Rel, Amp Dcy/Sus, Key Track, EG Int
 *   MIX / FILTER: Osc Mix, Noise, Sync/Ring, Filter Type, Portamento, Level, Drive
 *   EFFECTS:      Chorus Mix, Delay Time/Feedback/Mix, LFO1/LFO2 Rate, Master Vol, Pan
 *   BANK:         the active bank file
 * Pages marked `timbre` edit the timbre selected by Timbre Edit (in Layer mode): their header shows
 * [T1] or [T2], and with Timbre 2 their per-timbre labels read "T2.CUT" etc.
 */

// Cutoff knob -> Hz, the engine's measured mapping (cutoff_base_hz * 2^(knob * cutoff_octaves))
const cutoffHz = (v) => {
    const hz = 37.46 * Math.pow(2, v * 10.61);
    return hz >= 1000 ? `${(hz / 1000).toFixed(1)}k` : `${Math.round(hz)}Hz`;
};
const pct = (v) => `${Math.round(v * 100)}%`;
const bipolar = (v) => `${Math.round((v - 0.5) * 200)}%`;
const signed = (unit) => (v) => `${v > 0 ? "+" : ""}${Math.round(v)}${unit}`;

// `index`: an enum the engine reports and takes as an index; `int`: an integer range; `timbre`: per-timbre
export const PAGES = [
    {
        id: "perf",
        name: "PERF",
        timbre: true,
        params: [
            // Category = matrix row (genre); Program = the row's 16 patches, A1..A8 then B1..B8
            { key: "category",       label: "Category", short: "Cat",  index: true, values: ["Trance", "Techno", "Electr", "DnB", "Hiphop", "Retro", "SE/Hit", "Vocod"] },
            // Program shows the full patch code and name ("B.12 ARPEJMATR"), see formatValue
            { key: "patch",          label: "Program",  short: "Prog", index: true, values: ["A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8"], presetName: true },
            { key: "cutoff",         label: "Cutoff",  short: "Cut",  t2: "T2.CUT", format: cutoffHz },
            { key: "resonance",      label: "Res",     short: "Res",  t2: "T2.RES", format: pct },
            { key: "attack2",        label: "AmpAtk",  short: "Atk",  t2: "T2.ATK", format: pct },
            { key: "release2",       label: "AmpRel",  short: "Rel",  t2: "T2.REL", format: pct },
            // the program's arpeggiator (stored on / off until changed here)
            { key: "arp_on",         label: "Arp",     short: "Arp",   values: ["Off", "On"] },
            // stands in for the mod wheel the Move lacks: virtual patch source 7, same as CC1
            { key: "mod_wheel",      label: "ModWhl",  short: "MOD",   int: [0, 127], format: (v) => `${Math.round(v)}` }
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
            { key: "arp_target",     label: "Target", short: "Targ",  index: true, values: ["Both", "T1", "T2"] }
        ]
    },
    {
        id: "steps",
        name: "ARP STEPS",
        // the arpeggiator's 8-step trigger pattern: each step plays or rests, live
        params: [1, 2, 3, 4, 5, 6, 7, 8].map((n) => ({ key: `arp_step${n}`, label: `Step${n}`, short: `St${n}`, values: ["Rest", "Play"] }))
    },
    {
        id: "osc",
        name: "OSC / TIMBRE",
        timbre: true,
        params: [
            { key: "wave1",          label: "Wave1",   short: "Wav1", t2: "T2.WV1", index: true, values: ["SAW", "SQR", "TRI", "SIN", "VOX", "DWG", "NZ"] },
            { key: "pulse_width",    label: "Width",   short: "PulW", t2: "T2.PW",  format: (v) => `${Math.round(50 + 45 * v)}%` },
            { key: "wave2",          label: "Wave2",   short: "Wav2", t2: "T2.WV2", index: true, values: ["SAW", "SQR", "TRI"] },
            { key: "osc2_semi",      label: "Semi",    short: "Semi", t2: "T2.SEMI", int: [-24, 24], format: signed("st") },
            { key: "osc2_tune",      label: "Tune",    short: "Tune", t2: "T2.TUNE", int: [-50, 50], format: signed("ct") },
            // 2-position voice/timbre switches
            { key: "voice_mode",     label: "Mode",    short: "Mode", values: ["Single", "Layer"] },
            { key: "timbre_edit",    label: "Edit",    short: "Edit", values: ["Timb 1", "Timb 2"] },
            { key: "timbre_balance", label: "Balance", short: "Bal",  format: (v) => `${Math.round((1 - v) * 100)}:${Math.round(v * 100)}` }
        ]
    },
    {
        id: "env",
        name: "ENVELOPES",
        timbre: true,
        params: [
            { key: "attack1",     label: "FltAtk", short: "FAtk", t2: "T2.FATK", format: pct },
            { key: "decay1",      label: "FltDcy", short: "FDcy", t2: "T2.FDCY", format: pct },
            { key: "sustain1",    label: "FltSus", short: "FSus", t2: "T2.FSU",  format: pct },
            { key: "release1",    label: "FltRel", short: "FRel", t2: "T2.FRL",  format: pct },
            { key: "decay2",      label: "AmpDcy", short: "ADcy", t2: "T2.ADCY", format: pct },
            { key: "sustain2",    label: "AmpSus", short: "ASus", t2: "T2.ASU",  format: pct },
            { key: "keytrack",    label: "KeyTr",  short: "KeyTr", t2: "T2.KTRK", format: bipolar },
            { key: "env_int",     label: "EG Int", short: "EGInt", t2: "T2.EGIN", format: bipolar }
        ]
    },
    {
        id: "mix",
        name: "MIX / FILTER",
        timbre: true,
        params: [
            { key: "osc_mix",     label: "Mix",   short: "Mix",   t2: "T2.MIX",  format: pct },
            { key: "noise_level", label: "Noise", short: "Noise", t2: "T2.NOIS", format: pct },
            // option indices, in the hardware's order (the engine remaps Sync / Ring to its own)
            { key: "sync_ring",   label: "SyncR", short: "SyncR", t2: "T2.SYNC", index: true, values: ["OFF", "RING", "SYNC", "R.SNC"] },
            { key: "filter_type", label: "Type",  short: "Type",  t2: "T2.FTYP", index: true, values: ["LPF24", "LPF12", "BPF12", "HPF12"] },
            { key: "portamento",  label: "Porta", short: "Porta", t2: "T2.PORT", format: pct },
            { key: "level",       label: "Level", short: "Level", t2: "T2.LVL",  format: pct },
            { key: "drive",       label: "Drive", short: "Drive", t2: "T2.DRV", format: pct }
        ]
    },
    {
        id: "fx",
        name: "EFFECTS",
        params: [
            { key: "chorus_mix",  label: "Chor",  short: "Chor",  format: pct },
            { key: "delay_time",  label: "Time",  short: "Time",  format: (v) => `${Math.round(v * 1000)}ms` },
            { key: "delay_feedback",label: "Fdbk",short: "Fdbk",  format: pct },
            { key: "delay_mix",   label: "D.Mix", short: "D.Mix", format: pct },
            { key: "lfo1_rate",   label: "LFO1",  short: "LFO1",  format: (v) => `${(0.05 * Math.pow(600, v)).toFixed(1)}Hz` },
            { key: "lfo2_rate",   label: "LFO2",  short: "LFO2",  format: (v) => `${(0.05 * Math.pow(600, v)).toFixed(1)}Hz` },
            { key: "master_vol",  label: "Vol",   short: "Vol",   format: pct },
            { key: "pan",         label: "Pan",   short: "Pan",   format: (v) => v < 0.48 ? `L${Math.round((0.5 - v) * 200)}` : (v > 0.52 ? `R${Math.round((v - 0.5) * 200)}` : "C") }
        ]
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

    // Handle Move encoder turns (encoder 0 to 7)
    onEncoder(index, delta) {
        if (index < 0 || index >= 8) return;
        const page = PAGES[this.currentPageIndex];
        const paramDef = page.params[index];
        if (!paramDef) return;

        if (paramDef.key === "bank_file") {
            const count = parseInt(this.host && this.host.getParam ? this.host.getParam("bank_file_count") : 1) || 1;
            const cur = parseInt(this.getParam("bank_file")) || 0;
            this.setParam("bank_file", Math.max(0, Math.min(count - 1, cur + (delta > 0 ? 1 : -1))));
            return;
        }
        if (paramDef.index) {
            const last = paramDef.values.length - 1;
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
        if (paramDef.values) {
            const num = parseFloat(val);
            let idx = 0;
            if (!isNaN(num)) {
                if (paramDef.index) {
                    idx = Math.round(num);
                } else if (paramDef.key === "voice_mode" || paramDef.key === "timbre_edit") {
                    idx = (num >= 0.5) ? 1 : 0;
                } else if (num >= 1.0) {
                    // Safe clamp so it doesn't display out of bounds
                    idx = paramDef.values.length - 1;
                } else if (num <= 0.0) {
                    idx = 0;
                } else {
                    // Matches the engine: index = round(value * (count - 1))
                    idx = Math.min(paramDef.values.length - 1, Math.max(0, Math.round(num * (paramDef.values.length - 1))));
                }
            } else if (typeof val === "string") {
                const found = paramDef.values.indexOf(val);
                if (found >= 0) idx = found;
            }
            idx = Math.max(0, Math.min(paramDef.values.length - 1, idx));
            return paramDef.values[idx];
        }
        if (typeof paramDef.format === "function") {
            return paramDef.format(val);
        }
        return `${Math.round(val * 100)}%`;
    }

    // The timbre the per-timbre controls edit, as the engine routes them: 2 only for Timbre 2 in Layer mode
    editTimbre() {
        const layer = parseFloat(this.getParam("voice_mode")) >= 0.5;
        return (layer && parseInt(this.getParam("timbre_edit")) === 1) ? 2 : 1;
    }

    // Render OLED Display (128x64 pixels)
    drawUI(display) {
        if (!display) return;

        // Clear display buffer
        if (typeof display.clear === "function") {
            display.clear();
        }

        const page = PAGES[this.currentPageIndex];
        const timbre = page.timbre ? this.editTimbre() : 0;

        // 1. Header Bar (y: 0 to 12)
        // Active page with its timbre badge ([T1] / [T2] on per-timbre pages), and the current preset
        const currentName = this.getPresetName();
        const badge = timbre ? ` [T${timbre}]` : "";
        const headerText = `${page.name}${badge}  [${currentName}]`;
        if (typeof display.print === "function") {
            display.print(2, 2, headerText);
        }

        // Horizontal separator line under header
        if (typeof display.draw_line === "function") {
            display.draw_line(0, 13, 127, 13);
        }

        // 2. Encoder Slots Grid (8 parameters arranged in 2 rows of 4 columns)
        // Row 1: Encoders 0-3 (y: 16 to 36)
        // Row 2: Encoders 4-7 (y: 38 to 58)
        const colWidth = 32; // 128 / 4 = 32 pixels per column

        for (let i = 0; i < 8; i++) {
            const paramDef = page.params[i];
            if (!paramDef) continue;

            const val = this.getParam(paramDef.key);
            const col = i % 4;
            const row = Math.floor(i / 4);

            const x = col * colWidth;
            const y = 16 + row * 24;

            // Parameter short label (e.g. "Wav1", "Cut"; "T2.CUT" while Timbre 2 is edited)
            if (typeof display.print === "function") {
                display.print(x + 2, y, (timbre === 2 && paramDef.t2) ? paramDef.t2 : paramDef.short);
            }

            // Value label or mini bar
            const valStr = this.formatValue(paramDef, val);
            if (typeof display.print === "function") {
                display.print(x + 2, y + 10, valStr);
            }

            // Mini bar indicator (selector controls are drawn as their position in the range)
            if (typeof display.fill_rect === "function") {
                let frac = val;
                if (paramDef.index) frac = val / (paramDef.values.length - 1);
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
