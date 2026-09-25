/**
 * microKORG VA (schwung-mk-va) Move Synth Module UI
 *
 * Handles 128x64 OLED display rendering and 8 hardware encoders
 * across 4 parameter pages:
 *   Page 1: OSC (Wave1, PulseWidth, Wave2, Detune, Sync/Ring, Mix, Sub, Portamento)
 *   Page 2: FILTER (Cutoff, Res, Type, KeyTrack, EnvInt, Drive, ModInt, VelSens)
 *   Page 3: ENVs (Attack1, Decay1, Sustain1, Release1, Attack2, Decay2, Sustain2, Release2)
 *   Page 4: FX/MOD (LFO1 Rate, LFO2 Rate, Chorus Mix, Delay Time, Delay Feedback, Delay Mix, Master Vol, Pan)
 */

export const PAGES = [
    {
        id: "preset",
        name: "0: PRESET",
        params: [
            { key: "genre_category", label: "Genre",   short: "Genr", values: ["Trance", "Techno", "Electr", "DnB", "Hiphop", "Retro", "SE/Hit", "Vocod"] },
            { key: "program_num",    label: "Program", short: "Prog", values: ["A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8"] },
            { key: "voice_mode",     label: "Mode",    short: "Mode", values: ["Single", "Layer"] },
            { key: "timbre_edit",    label: "Edit",    short: "Edit", values: ["Timb 1", "Timb 2"] },
            { key: "timbre_balance", label: "Balance", short: "Bal",  format: (v) => `${Math.round((1 - v) * 100)}:${Math.round(v * 100)}` },
            { key: "cutoff",         label: "Cutoff",  short: "Cut",  format: (v) => `${Math.round(20 * Math.pow(900, v))}Hz` },
            { key: "resonance",      label: "Res",     short: "Res",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "drive",          label: "Drive",   short: "Drive",format: (v) => `${Math.round(v * 100)}%` }
        ]
    },
    {
        id: "osc",
        name: "1: OSC",
        params: [
            { key: "wave1",       label: "Wave1", short: "Wave1", values: ["Saw", "Sqr", "Tri", "Sin"] },
            { key: "pulse_width", label: "Width", short: "Width", format: (v) => `${Math.round(v * 100)}%` },
            { key: "wave2",       label: "Wave2", short: "Wave2", values: ["Saw", "Sqr", "Tri"] },
            { key: "detune",      label: "Detun", short: "Detun", format: (v) => `${Math.round((v - 0.5) * 48)}st` },
            { key: "sync_ring",   label: "SyncR", short: "SyncR", values: ["Off", "Sync", "Ring", "Both"] },
            { key: "osc_mix",     label: "Mix",   short: "Mix",   format: (v) => `${Math.round(v * 100)}%` },
            { key: "sub_level",   label: "Sub",   short: "Sub",   format: (v) => `${Math.round(v * 100)}%` },
            { key: "portamento",  label: "Porta", short: "Porta", format: (v) => `${Math.round(v * 100)}%` }
        ]
    },
    {
        id: "filter",
        name: "2: FILTER",
        params: [
            { key: "cutoff",      label: "Cut",   short: "Cut",   format: (v) => `${Math.round(20 * Math.pow(900, v))}Hz` },
            { key: "resonance",   label: "Res",   short: "Res",   format: (v) => `${Math.round(v * 100)}%` },
            { key: "filter_type", label: "Type",  short: "Type",  values: ["24LP", "12LP", "12BP", "12HP"] },
            { key: "keytrack",    label: "KeyTr", short: "KeyTr", format: (v) => `${Math.round(v * 100)}%` },
            { key: "env_int",     label: "EnvIn", short: "EnvIn", format: (v) => `${Math.round((v - 0.5) * 200)}%` },
            { key: "drive",       label: "Drive", short: "Drive", format: (v) => `${Math.round(v * 100)}%` },
            { key: "mod_int",     label: "ModIn", short: "ModIn", format: (v) => `${Math.round(v * 100)}%` },
            { key: "vel_sens",    label: "VelSn", short: "VelSn", format: (v) => `${Math.round(v * 100)}%` }
        ]
    },
    {
        id: "envs",
        name: "3: ENVs",
        params: [
            { key: "attack1",     label: "Atk1",  short: "Atk1",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "decay1",      label: "Dcy1",  short: "Dcy1",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "sustain1",    label: "Sus1",  short: "Sus1",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "release1",    label: "Rel1",  short: "Rel1",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "attack2",     label: "Atk2",  short: "Atk2",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "decay2",      label: "Dcy2",  short: "Dcy2",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "sustain2",    label: "Sus2",  short: "Sus2",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "release2",    label: "Rel2",  short: "Rel2",  format: (v) => `${Math.round(v * 100)}%` }
        ]
    },
    {
        id: "fx_mod",
        name: "4: FX/MOD",
        params: [
            { key: "lfo1_rate",   label: "LFO1",  short: "LFO1",  format: (v) => `${(0.05 * Math.pow(600, v)).toFixed(1)}Hz` },
            { key: "lfo2_rate",   label: "LFO2",  short: "LFO2",  format: (v) => `${(0.05 * Math.pow(600, v)).toFixed(1)}Hz` },
            { key: "chorus_mix",  label: "Chor",  short: "Chor",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "delay_time",  label: "Time",  short: "Time",  format: (v) => `${Math.round(v * 1000)}ms` },
            { key: "delay_feedback",label: "Fdbk",short: "Fdbk",  format: (v) => `${Math.round(v * 100)}%` },
            { key: "delay_mix",   label: "D.Mix", short: "D.Mix", format: (v) => `${Math.round(v * 100)}%` },
            { key: "master_vol",  label: "Vol",   short: "Vol",   format: (v) => `${Math.round(v * 100)}%` },
            { key: "pan",         label: "Pan",   short: "Pan",   format: (v) => v < 0.48 ? `L${Math.round((0.5 - v) * 200)}` : (v > 0.52 ? `R${Math.round((v - 0.5) * 200)}` : "C") }
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
        this.state.params["genre_category"] = 0;
        this.state.params["program_num"] = 1;
        this.state.params["voice_mode"] = 0;
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
            this.host.setParam(key, value.toString());
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

        if (paramDef.key === "genre_category") {
            const cur = parseInt(this.getParam("genre_category")) || 0;
            const next = Math.max(0, Math.min(7, cur + (delta > 0 ? 1 : -1)));
            this.setParam("genre_category", next);
            return;
        }
        if (paramDef.key === "program_num") {
            const cur = parseInt(this.getParam("program_num")) || 1;
            const next = Math.max(1, Math.min(16, cur + (delta > 0 ? 1 : -1)));
            this.setParam("program_num", next);
            return;
        }
        if (paramDef.key === "voice_mode") {
            const cur = parseInt(this.getParam("voice_mode")) || 0;
            const next = Math.max(0, Math.min(1, cur + (delta > 0 ? 1 : -1)));
            this.setParam("voice_mode", next);
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
        if (paramDef.values) {
            const idx = Math.min(paramDef.values.length - 1, Math.floor(val * paramDef.values.length));
            return paramDef.values[idx];
        }
        if (typeof paramDef.format === "function") {
            return paramDef.format(val);
        }
        return `${Math.round(val * 100)}%`;
    }

    // Render OLED Display (128x64 pixels)
    drawUI(display) {
        if (!display) return;

        // Clear display buffer
        if (typeof display.clear === "function") {
            display.clear();
        }

        const page = PAGES[this.currentPageIndex];

        // 1. Header Bar (y: 0 to 12)
        // Module title, Active Page, and Current Preset
        const currentName = this.getPresetName();
        const headerText = `${page.name}  [${currentName}]`;
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

            // Parameter short label (e.g. "Wave1", "Cut", "Atk1")
            if (typeof display.print === "function") {
                display.print(x + 2, y, paramDef.short);
            }

            // Value label or mini bar
            const valStr = this.formatValue(paramDef, val);
            if (typeof display.print === "function") {
                display.print(x + 2, y + 10, valStr);
            }

            // Mini bar indicator
            if (typeof display.fill_rect === "function") {
                const barWidth = Math.max(1, Math.round(val * (colWidth - 4)));
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
