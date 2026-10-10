#!/usr/bin/env python3
"""The two program decoders against each other on a real bank: tools/extracts_presets.py (Python) and
src/dsp/syx_bank.h (C, as the engine loads it).

    python tools/test_decoders.py BANK.syx [BANK.syx ...]

Every timbre and FX float of all 128 programs must agree. Needs no reference bank of its own, so it runs on
whatever dump is at hand (tools/test_bank_loader.py needs banks/TinyK_Default.syx)."""
import ctypes
import os
import shutil
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate_dsp as cal
from extracts_presets import FX_FIELDS, TIMBRE_FIELDS, load_programs, parse_program


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tmp = tempfile.mkdtemp(prefix="tinyk_dec_")
    lib = cal.Engine(cal.build_library(tmp)).lib
    fptr = ctypes.POINTER(ctypes.c_float)
    lib.tinyk_bank_scan.argtypes = [ctypes.c_char_p]
    lib.tinyk_bank_preset.argtypes = [ctypes.c_int, ctypes.c_int, fptr, fptr, fptr, ctypes.c_char_p, ctypes.c_int]
    bad = 0
    for path in sys.argv[1:]:
        d = tempfile.mkdtemp(prefix="bank_", dir=tmp)
        shutil.copy(path, os.path.join(d, "bank.syx"))
        if lib.tinyk_bank_scan(d.encode()) != 1:
            sys.exit("%s: the C loader did not accept it" % path)
        progs = load_programs(path)
        worst = {}
        for i, prog in enumerate(progs):
            ref = parse_program(i, prog)
            if not ref["data_valid"]:
                continue
            t1, t2 = np.zeros(len(TIMBRE_FIELDS), np.float32), np.zeros(len(TIMBRE_FIELDS), np.float32)
            fx, label = np.zeros(len(FX_FIELDS), np.float32), ctypes.create_string_buffer(64)
            lib.tinyk_bank_preset(1, i, t1.ctypes.data_as(fptr), t2.ctypes.data_as(fptr), fx.ctypes.data_as(fptr), label, 64)
            for name, got, want in (("t1", t1, ref["t1"]), ("t2", t2, ref["t2"])):
                for k, f in enumerate(TIMBRE_FIELDS):
                    worst[f] = max(worst.get(f, 0.0), abs(float(got[k]) - want[f]))
            for k, f in enumerate(FX_FIELDS):
                worst[f] = max(worst.get(f, 0.0), abs(float(fx[k]) - ref["fx"][f]))
        off = {f: v for f, v in worst.items() if v > 1e-6}
        bad += len(off)
        print("  [%s] %s: %d timbre + %d FX fields over %d programs%s" % (
            "PASS" if not off else "FAIL", os.path.basename(path), len(TIMBRE_FIELDS), len(FX_FIELDS), len(progs),
            "" if not off else ", differing: %s" % ", ".join("%s (%.4f)" % kv for kv in sorted(off.items()))))
    print("\nALL PASS" if not bad else "\nFAILED")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
