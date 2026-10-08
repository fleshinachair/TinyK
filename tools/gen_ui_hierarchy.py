"""Rewrites MK_UI_HIERARCHY in src/dsp/dsp.c from src/module.json's ui_hierarchy (the engine serves the same
JSON as the manifest; test_bank_loader.py checks they are identical). Run after editing the manifest."""
import json, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BS = chr(92)

def main():
    manifest = json.load(open(os.path.join(ROOT, "src", "module.json"), encoding="utf-8"))
    j = json.dumps(manifest["capabilities"]["ui_hierarchy"], separators=(",", ":"), ensure_ascii=False)
    esc = j.replace(BS, BS + BS).replace('"', BS + '"')
    out, i = [], 0
    while i < len(esc):
        e = min(i + 130, len(esc))
        k = e
        while k > i and esc[k - 1] == BS:
            k -= 1
        if (e - k) % 2:  # do not split a backslash from the character it escapes
            e -= 1
        out.append(esc[i:e])
        i = e
    lit = "static const char MK_UI_HIERARCHY[] =\n" + "\n".join('    "%s"' % c for c in out) + ";"
    path = os.path.join(ROOT, "src", "dsp", "dsp.c")
    s = open(path, encoding="utf-8").read()
    a = s.index("static const char MK_UI_HIERARCHY[] =")
    b = s.index(";", s.index('}}}"', a)) + 1
    open(path, "w", encoding="utf-8", newline="").write(s[:a] + lit + s[b:])

if __name__ == "__main__":
    main()
