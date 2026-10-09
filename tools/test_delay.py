#!/usr/bin/env python3
"""Checks the delay against the numbers measured on the microKORG plug-in (see delay_time_s / delay_gain in dsp.c).

    python tools/test_delay.py        (exit code 0 = all pass)

Engine only: a 10 ms noise burst through each delay type, read back as a train of repeats (time, gain per side).
`python tools/vst_ab.py delay` runs the same bursts through the plug-in for a side-by-side.
"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate_dsp as cal
from extracts_presets import VOCODER_CARRIER
from vst_ab import echo_train

failures = 0
# knob -> seconds, each measured on the plug-in
TIMES = {0: 0.01365, 20: 0.0776, 40: 0.1417, 64: 0.2184, 80: 0.3324, 110: 0.5461, 115: 0.6827, 120: 0.8192,
         123: 0.9830, 125: 1.0922, 126: 1.3653, 127: 1.6384}
# (time knob, depth) -> gain of each repeat over the one before, measured on the plug-in
GAINS = {(10, 40): 0.186, (10, 127): 0.982, (40, 80): 0.463, (40, 110): 0.783, (64, 40): 0.180, (96, 80): 0.436,
         (110, 127): 0.891, (120, 110): 0.683, (125, 40): 0.150, (126, 80): 0.356, (127, 127): 0.693}


def check(ok, what):
    global failures
    print("  [%s] %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        failures += 1


def main():
    tmp = tempfile.mkdtemp(prefix="tinyk_delay_")
    try:
        eng = cal.Engine(cal.build_library(tmp))
        timbre = dict(VOCODER_CARRIER, wave1=1.0, osc1_ctrl1=1.0, osc1_ctrl2=0.0, osc_mix=0.0, noise_level=0.0, detune=0.5, resonance=0.0, cutoff=1.0,
                      filter_type=0.0, keytrack=0.5, env_int=0.5, drive=0.0, attack1=0.0, sustain1=1.0, attack2=0.0,
                      decay2=0.12, sustain2=0.0, release2=0.0, portamento=0.0, level=0.25, amp_level=65.0 / 128.0)   # a burst long enough to clear the 4.5 ms onset fade

        def train(kind, time, depth, pan=0.5, count=3):
            fx = dict({f: 0.0 for f in cal.FX_FIELDS}, delay_time=time / 127.0, delay_feedback=depth / 127.0, delay_mix=depth / 127.0)
            t = dict(timbre, pan=pan)
            patch = {"voice_mode": 0.0, "t1": t, "t2": dict(t), "fx": fx, "delay_type": kind / 2.0}
            total = 0.4 + (count + 0.5) * max(TIMES.get(time, 0.3), 0.05)
            got = echo_train(eng.render_patch(0, patch, 60, 0.01, total, stereo=True), count + 1)
            return [r for r in got if abs(r[1]) + abs(r[2]) > 0.02][:count]

        print("Delay time (knob -> first repeat)")
        worst = (0.0, 0)
        for knob, want in TIMES.items():
            got = train(0, knob, 100, count=1)[0][0]
            if abs(got - want) > worst[0]:
                worst = (abs(got - want), knob)
        check(worst[0] < 0.0002, "12 knob positions within %.2f ms of the plug-in (worst at %d)" % (1000 * worst[0], worst[1]))

        print("Depth (gain of each repeat, at several delay times)")
        worst = (0.0, None)
        for (knob, depth), want in GAINS.items():
            t = train(0, knob, depth, count=2)
            first, ratio = t[0][1], t[1][1] / t[0][1]
            err = max(abs(first - want), abs(ratio - want))
            if err > worst[0]:
                worst = (err, (knob, depth))
        check(worst[0] < 0.012, "11 settings: first repeat and repeat-to-repeat gain within %.3f (worst at time, depth %s)" % worst)

        print("Types (a burst panned hard left, then centred)")
        g = 0.570   # depth 90 at time 40
        near = lambda t, want: len(t) == len(want) and all(abs(a[1] - w[0]) < 0.01 and abs(a[2] - w[1]) < 0.01 for a, w in zip(t, want))
        check(near(train(0, 40, 90, 0.0), [(g, 0), (g * g, 0), (g ** 3, 0)]), "Stereo: a left source repeats on the left only")
        check(near(train(1, 40, 90, 0.0), [(0, g), (g * g, 0), (0, g ** 3)]), "Cross: right, left, right")
        check(near(train(2, 40, 90, 0.0), [(g / 2, 0), (0, g * g / 2), (g ** 3 / 2, 0)]), "L/R: left, right, left at half level (the mono sum of one side)")
        check(near(train(2, 40, 90, 0.5), [(g, 0), (0, g * g), (g ** 3, 0)]), "L/R: a centred source starts on the left at full level")
        check(train(0, 40, 0) == [], "depth 0 is off")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%s" % ("ALL PASS" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
