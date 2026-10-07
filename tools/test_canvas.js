/*
 * Checks src/canvas.js (the Arp Steps LED widget) against a fake frame context, the way the Schwung host calls it:
 * draw(ctx, { group, values, nowMs }) per cell, ctx = a 32 x 15 frame with fillRect(x, y, w, h, color).
 *
 *   node tools/test_canvas.js            (exit code 0 = all pass)
 *   node tools/test_canvas.js --preview  (also prints the 2 x 4 grid as the Move's screen would show it)
 */
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

vm.runInThisContext(fs.readFileSync(path.join(__dirname, "..", "src", "canvas.js"), "utf-8"), { filename: "canvas.js" });
const ov = globalThis.canvas_overlay;
const T = globalThis.TINYK_STEP_WIDGET_FOR_TEST;

let failures = 0;
function check(ok, what) {
    console.log(`  [${ok ? "PASS" : "FAIL"}] ${what}`);
    if (!ok) failures++;
}

function frame(w, h) {
    const px = Array.from({ length: h }, () => new Array(w).fill(0));
    return {
        width: w, height: h, px,
        fillRect(x, y, rw, rh, c) {
            for (let j = y; j < y + rh; j++) for (let i = x; i < x + rw; i++) {
                if (i >= 0 && j >= 0 && i < w && j < h) px[j][i] = c ? 1 : 0;
            }
        },
    };
}

/* Draws cell `step` (0..7) and returns its pixels */
function cell(step, values, nowMs) {
    const f = frame(32, 15);
    const key = `arp_step${step + 1}`;
    ov.widgetKinds["custom:tinyk_step"].draw(f, { group: { kind: "custom:tinyk_step", roles: { value: key }, keys: [key] }, values, nowMs });
    return f.px;
}
const lit = (px) => px.reduce((n, r) => n + r.reduce((a, b) => a + b, 0), 0);
const centre = (px) => px[7][15];
const blockCorner = (px) => px[0][8];   /* inside the playhead block, outside the box */

function reset() {
    T.head.raw = null; T.head.running = false; T.head.len = 8; T.head.seenAt = 0;
}

console.log("Arp Steps LED widget (src/canvas.js):");
check(ov && ov.widgetKinds && typeof ov.widgetKinds["custom:tinyk_step"].draw === "function",
      "registers custom:tinyk_step on globalThis.canvas_overlay.widgetKinds");

reset();
const stopped = { arp_playhead: "0,6", arp_step1: "1", arp_step2: "0", arp_step7: "1" };
const play = cell(0, stopped, 1000), rest = cell(1, stopped, 1000), past = cell(6, stopped, 1000);
check(centre(play) === 1 && lit(play) === 121, `Play: an 11 x 11 filled box (${lit(play)} px)`);
check(centre(rest) === 0 && lit(rest) === 40, `Rest: a hollow 11 x 11 outline (${lit(rest)} px)`);
check(centre(past) === 0 && lit(past) > 0 && lit(past) < 40, `step 7 past a 6-step pattern: a dotted box (${lit(past)} px)`);
check(blockCorner(play) === 0, "stopped: no playhead");
check(lit(cell(0, { arp_step1: "Play" }, 0)) === 121 && lit(cell(0, { arp_step1: "Rest" }, 0)) === 40,
      "values as option names (Play / Rest) draw the same");

/* running: next step 3, so step 3 (index 2) is lit; 125 ms steps, 50 ms to the next */
reset();
const v = { arp_playhead: "1,8,3,50.0,125.00,0.000", arp_step3: "1", arp_step4: "0" };
let px = cell(2, v, 1000);
check(blockCorner(px) === 1 && centre(px) === 0, "the lit Play step is inverted: a lit block, the box cut out");
check(blockCorner(cell(3, v, 1000)) === 0, "...and the next step is not lit yet");
px = cell(3, v, 1051);
check(blockCorner(px) === 1 && centre(px) === 1, "51 ms later the next step (a Rest) is lit, its outline cut out");
check(T.litStep(1051 + 125) === 4, "one step (125 ms) on: step 5, extrapolated without a new reading");
check(T.litStep(1051 + 125 * 5) === 0, "the playhead wraps at the pattern length");

/* a reading one step behind, near the boundary, is read latency: ignored */
reset();
T.update("1,8,5,30.0,125.00,0.000", 1000);
check(T.litStep(1040) === 5, "running estimate: step 6 at 1040 ms");
T.update("1,8,5,10.0,125.00,0.000", 1050);
check(T.litStep(1050) === 5, "a reading one step behind, 10 ms from its boundary, keeps the estimate");
T.update("1,8,3,100.0,125.00,0.000", 1060);
check(T.litStep(1060) === 2, "a reading further off re-anchors");
check(T.litStep(1060 + 1600) === -1, "no fresh reading for 1.5 s: no playhead");

/* swing +100 %: even -> odd steps take 4/3 of a step, odd -> even 2/3 */
reset();
T.update("1,8,0,10.0,120.00,1.000", 0);
const lits = [5, 15, 165, 175, 245, 255].map((t) => T.litStep(t));
check(JSON.stringify(lits) === JSON.stringify([7, 0, 0, 1, 1, 2]),
      `swing +100 %: step boundaries at 10, 170, 250 ms (lit ${lits.join(" ")})`);

reset();
check(T.update("0,8", 0) === undefined && T.litStep(0) === -1, "stopped reading: no playhead");

if (process.argv.includes("--preview")) {
    /* The Move's grid: cells 32 px wide, picture rows at y 9 and 33 (15 px), labels below */
    reset();
    const pv = { arp_playhead: "1,7,2,60,125,0", arp_step1: "1", arp_step2: "0", arp_step3: "1", arp_step4: "1",
                 arp_step5: "0", arp_step6: "1", arp_step7: "1", arp_step8: "1" };
    const screen = Array.from({ length: 64 }, () => new Array(128).fill(" "));
    for (let s = 0; s < 8; s++) {
        const c = cell(s, pv, 500), ox = (s % 4) * 32, oy = s < 4 ? 9 : 33;
        for (let j = 0; j < 15; j++) for (let i = 0; i < 32; i++) if (c[j][i]) screen[oy + j][ox + i] = "#";
        const label = `ST${s + 1}`;
        for (let i = 0; i < label.length; i++) screen[oy + 16][ox + 12 + i] = label[i];
    }
    console.log("\nPreview (7-step pattern, step 2 sounding; rows 1-4 / 5-8):");
    for (let j = 8; j < 52; j++) console.log("|" + screen[j].join("") + "|");
}

console.log(failures ? `\n${failures} FAILED` : "\nALL PASSED");
process.exit(failures ? 1 : 0);
