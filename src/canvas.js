/*
 * TinyK canvas widgets for the Schwung param pages (shared/param_pages/widget_registry.mjs).
 *
 * custom:tinyk_step draws each Arp Steps cell as one LED of the microKORG's 2 x 4 step grid: the page's top row of
 * knobs is steps 1-4, the bottom row 5-8, and each cell's picture sits above its knob label.
 *   Rest      hollow box
 *   Play      filled box
 *   playhead  the sounding step, inverted: a lit block with the box cut out of it
 *   past the pattern's length: a dotted box (turning such a step extends the pattern)
 *
 * Draw hooks get cached values only (the host reads params on its own rotation, never on the draw path). The
 * playhead comes from the read-only arp_playhead key, which the host refreshes about four times a second: it carries
 * the next step, the time to it, the step length and the swing, so the widget runs the playhead on by itself between
 * reads and the LEDs keep time with the arpeggio. Format (src/dsp/dsp.c format_arp_playhead):
 *   "0,<length>"                                         stopped
 *   "1,<length>,<next>,<ms to next>,<step ms>,<swing>"   running; the step lit is the one before <next>
 */
(function () {
    const WRAP = 1680;          /* the engine reports step indices mod 1680 (even: swing parity survives) */
    const STALE_MS = 1500;      /* no fresh reading for this long (page hidden, arp stopped meanwhile): no playhead */
    const BACKSTEP_MS = 60;     /* a reading that would move the playhead back one step this close to a boundary is
                                 * the read's own latency, not the arpeggio: keep the running estimate */

    const head = { raw: null, running: false, len: 8, next: 0, tNext: 0, stepMs: 125, swing: 0, seenAt: 0 };

    const mod = (a, n) => ((a % n) + n) % n;

    /* How long step k lasts: arp_step_time puts every odd step swing / 3 of a step late */
    function stepDuration(k) {
        const d = head.stepMs * ((k & 1) ? 1 - head.swing / 3 : 1 + head.swing / 3);
        return d > 1 ? d : 1;
    }

    function advance(now) {
        if (!head.running) return;
        for (let guard = 0; guard < 256 && now >= head.tNext; guard++) {
            head.tNext += stepDuration(head.next);
            head.next = mod(head.next + 1, WRAP);
        }
    }

    function update(raw, now) {
        if (typeof raw !== "string" || raw === head.raw) { advance(now); return; }
        head.raw = raw;
        head.seenAt = now;
        const f = raw.split(",").map(Number);
        const len = f[1] >= 1 && f[1] <= 8 ? Math.round(f[1]) : 8;
        head.len = len;
        if (f[0] !== 1 || f.length < 6 || !f.slice(2, 6).every(Number.isFinite)) { head.running = false; return; }
        const next = mod(Math.round(f[2]), WRAP), toNext = Math.max(0, f[3]);
        head.stepMs = f[4] > 1 ? f[4] : 1;
        head.swing = Math.max(-1, Math.min(1, f[5]));
        if (head.running) {
            advance(now);
            if (mod(head.next - next, WRAP) === 1 && toNext < BACKSTEP_MS) return;
        }
        head.running = true;
        head.next = next;
        head.tNext = now + toNext;
    }

    /* The lit step, or -1 */
    function litStep(now) {
        advance(now);
        if (!head.running || now - head.seenAt > STALE_MS) return -1;
        return mod(head.next - 1, WRAP) % head.len;
    }

    function isPlay(v) {
        if (v === 1 || v === true) return true;
        const s = String(v === undefined || v === null ? "" : v).trim();
        return s === "1" || /^play$/i.test(s) || (s !== "" && s !== "0" && !/^rest$/i.test(s) && Number(s) >= 0.5);
    }

    function outline(ctx, x, y, w, h, color) {
        ctx.fillRect(x, y, w, 1, color);
        ctx.fillRect(x, y + h - 1, w, 1, color);
        ctx.fillRect(x, y + 1, 1, h - 2, color);
        ctx.fillRect(x + w - 1, y + 1, 1, h - 2, color);
    }

    function dotted(ctx, x, y, w, h, color) {
        for (let i = 0; i < w; i += 2) { ctx.fillRect(x + i, y, 1, 1, color); ctx.fillRect(x + i, y + h - 1, 1, 1, color); }
        for (let j = 2; j < h - 1; j += 2) { ctx.fillRect(x, y + j, 1, 1, color); ctx.fillRect(x + w - 1, y + j, 1, 1, color); }
    }

    function drawStep(ctx, payload) {
        const p = payload || {};
        const group = p.group || {};
        const values = p.values || {};
        const key = (group.roles && group.roles.value) || (group.keys && group.keys[0]) || "";
        const m = /arp_step([1-8])$/.exec(key);
        if (!m) return;
        const step = Number(m[1]) - 1;
        const now = typeof p.nowMs === "number" ? p.nowMs : Date.now();
        update(values.arp_playhead, now);

        const W = ctx.width, H = ctx.height;
        const B = Math.max(5, Math.min(11, H - 4, W - 4));
        const x = Math.floor((W - B) / 2), y = Math.floor((H - B) / 2);
        const play = isPlay(values[key]);
        const inPattern = step < head.len;

        if (litStep(now) === step) {
            ctx.fillRect(x - 2, y - 2, B + 4, B + 4, 1);   /* inverted: the box is drawn in black on a lit block */
            if (play) ctx.fillRect(x, y, B, B, 0);
            else outline(ctx, x, y, B, B, 0);
            return;
        }
        if (!inPattern) dotted(ctx, x, y, B, B, 1);
        else if (play) ctx.fillRect(x, y, B, B, 1);
        else outline(ctx, x, y, B, B, 1);
    }

    globalThis.canvas_overlay = {
        widgetKinds: {
            "custom:tinyk_step": { draw: drawStep, nominal: { w: 32, h: 15 } },
        },
    };
    /* test seam (tools/test_canvas.mjs) */
    globalThis.TINYK_STEP_WIDGET_FOR_TEST = { drawStep, head, litStep, update };
})();
