#!/usr/bin/env python3
"""Digitise the microKORG's 64 DWGS waveforms from oscilloscope plots and write src/dsp/dwgs_waves.h.

Source: "Korg microKorg DWGS Waveform Reference List" (Paquito Salazar), a PDF with one scope capture per
waveform (pages 3-66): 10 ms of the instrument's output at 220 Hz (A3), +-275 mV full scale, the same axes
on every page. The PDF is not part of the repository:

    python tools/trace_dwgs.py path/to/dwgs.pdf [--preview DIR]

needs `pdftoppm` (poppler), NumPy, SciPy and Pillow. --preview writes each page with the fitted wave drawn
over the scan.

Method: the black trace is read column by column (darkness-weighted centre; the title text and the grey
cursor read-out are masked), giving ~1446 samples over 2.2 periods of the note. A Fourier series is then
fitted by least squares over everything visible, so the two cycles average the tracing noise (typically
1-3 mV rms of a 550 mV range). 96 harmonics of the note are kept: 21 kHz at 220 Hz, about what the plot's
658 pixels per period resolve.

Most waves repeat every period of the note. A few do not (LOOPS): their table spans several periods of the
note, and they hold partials between the note's harmonics.
  - SynSine3, Digi8, Endless repeat every 2 periods (a component an octave below the note).
  - Digi2, Bell3 repeat every 1.5 periods (partials on multiples of 2/3 of the note); stored as a 3-period
    table with every other harmonic empty.
  - Bell4 does not repeat within the 10 ms shown, so its loop length cannot be read from the plot. Its
    partials (a subspace estimate, ESPRIT) are snapped to a 5-period grid, the shortest that matches the
    free estimate; it is an approximation of the snapshot, not a recovered table.
Endless also carries slightly detuned partial pairs that a 2-period table can only approximate.

Levels: the plots share one mV scale, so the waves keep their relative level; the loudest peak is 1.0.
The traces are the instrument's analogue output, so they include its output stage (a slight droop on flat
tops from AC coupling, at most a few degrees of phase at 220 Hz). The start phase is the scope's trigger
point. DC is dropped.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "dsp", "dwgs_waves.h")

NAMES = (["SynSine%d" % i for i in range(1, 8)] + ["SynBass%d" % i for i in range(1, 8)]
         + ["SynWave%d" % i for i in range(1, 10)] + ["5thWave%d" % i for i in range(1, 4)]
         + ["Digi%d" % i for i in range(1, 9)] + ["Endless"] + ["E.Piano%d" % i for i in range(1, 5)]
         + ["Organ%d" % i for i in range(1, 8)] + ["Clav1", "Clav2"] + ["Guitar%d" % i for i in range(1, 4)]
         + ["Bass%d" % i for i in range(1, 6)] + ["Bell%d" % i for i in range(1, 5)]
         + ["Voice%d" % i for i in range(1, 5)])
assert len(NAMES) == 64

# Page geometry at 72 dpi (1565 x 794): the plot frame and its scales, identical on all 64 pages
X0, X1 = 97, 1543            # plot interior columns (frame lines at 96 / 1544)
Y0, Y1 = 22, 757             # plot interior rows (frame lines at 21 / 758)
ZERO_ROW = 389.5
PX_PER_MV = (758.5 - 20.5) / 550.0
PERIOD = 658.6               # pixels per period of the note: 219.86 Hz, the median of the per-page fits
TEXT_COLS, TEXT_ROWS = 520, 74   # the title ("12 SynBass5") sits above / left of these
HARMONICS = 96               # per period of the note

# wave number -> (table length in periods of the note, harmonic stride within the table)
LOOPS = {3: (2, 1), 34: (2, 1), 35: (2, 1), 28: (3, 2), 59: (3, 2)}
SPARSE = {60: 5}             # wave number -> table length; partials from ESPRIT, see the module docstring


def trace(path):
    """One page -> the trace in mV per plot column (NaN where no trace was found)."""
    im = np.array(Image.open(path).convert("L")).astype(float)[Y0:Y1 + 1, X0:X1 + 1]
    dark = im < 200                                     # grid lines are 214 and lighter
    dark[330 - Y0:396 - Y0, 1522 - X0:] = False         # the grey cursor read-out at the right edge
    lab, n = ndimage.label(dark, structure=np.ones((3, 3)))
    keep = np.zeros(n + 1, bool)                        # label 0 is the background
    for i, (r, c) in enumerate(ndimage.find_objects(lab), 1):
        keep[i] = not (r.stop + Y0 <= TEXT_ROWS and c.stop + X0 <= TEXT_COLS)   # a glyph of the title
    w = np.where(keep[lab], 255.0 - im, 0.0)
    rows = np.arange(im.shape[0], dtype=float)
    s = w.sum(axis=0)
    with np.errstate(invalid="ignore", divide="ignore"):
        yc = (w * rows[:, None]).sum(axis=0) / s + Y0
    return np.where(s > 0, (ZERO_ROW - yc) / PX_PER_MV, np.nan)


def fourier_fit(y, periods, stride):
    """Least-squares Fourier series over the visible columns. Returns (cos, sin) per table harmonic 1..H
    (H = HARMONICS * periods, zero off the stride) and the rms residual in mV."""
    x = np.arange(y.size, dtype=float) + 0.5
    H = HARMONICS * periods
    k = np.arange(stride, H + 1, stride)
    ph = 2 * np.pi * np.outer(x, k) / (PERIOD * periods)
    A = np.hstack([np.ones((x.size, 1)), np.cos(ph), np.sin(ph)])
    lam = 1e-3 * np.r_[0.0, (k / H) ** 2, (k / H) ** 2]      # mild ridge: keeps the top harmonics tame
    ok = np.isfinite(y)
    for _ in range(3):                                         # drop outlier columns (stray marks) and refit
        Ao = A[ok]
        c = np.linalg.solve(Ao.T @ Ao + np.diag(lam * ok.sum()), Ao.T @ y[ok])
        r = np.nan_to_num(A @ c - y)
        rms = np.sqrt(np.mean(r[ok] ** 2))
        ok = np.isfinite(y) & (np.abs(r) < 4 * rms + 3)
    co, si = np.zeros(H + 1), np.zeros(H + 1)
    co[k], si[k] = c[1:1 + k.size], c[1 + k.size:]
    return co[1:], si[1:], rms


def esprit_fit(y, periods, order=60, floor_mv=1.5):
    """A wave that does not repeat in view: estimate its partials and snap them to a table of `periods`."""
    x = np.arange(y.size, dtype=float)
    ok = np.isfinite(y)
    yy = np.interp(x, x[ok], y[ok])
    yy -= yy.mean()
    H = np.lib.stride_tricks.sliding_window_view(yy, y.size // 3)
    V = np.linalg.svd(H, full_matrices=False)[2][:order].T
    z = np.linalg.eigvals(np.linalg.lstsq(V[:-1], V[1:], rcond=None)[0])
    f = np.angle(z) / (2 * np.pi) * PERIOD                   # in harmonics of the note
    f = np.sort(f[(f > 0.05) & (np.abs(np.abs(z) - 1) < 0.02)])

    def refit(freqs):
        ph = 2 * np.pi * np.outer(x + 0.5, freqs) / PERIOD
        A = np.hstack([np.cos(ph), np.sin(ph)])
        c = np.linalg.lstsq(A, yy, rcond=None)[0]
        return c[:freqs.size], c[freqs.size:], np.sqrt(np.mean((A @ c - yy) ** 2))

    co, si, _ = refit(f)
    f = f[np.hypot(co, si) > floor_mv]
    k = np.unique(np.round(f * periods).astype(int))
    k = k[(k > 0) & (k <= HARMONICS * periods)]
    co, si, rms = refit(k / periods)
    oc, os_ = np.zeros(HARMONICS * periods + 1), np.zeros(HARMONICS * periods + 1)
    oc[k], os_[k] = co, si
    return oc[1:], os_[1:], rms


def synth(co, si, n):
    ph = 2 * np.pi * np.outer(np.arange(n) / n, np.arange(1, co.size + 1))
    return np.cos(ph) @ co + np.sin(ph) @ si


def render_pages(pdf, out_dir):
    subprocess.run(["pdftoppm", "-r", "72", "-gray", "-png", "-f", "3", "-l", "66", pdf,
                    os.path.join(out_dir, "p")], check=True)
    pages = sorted(glob.glob(os.path.join(out_dir, "p-*.png")))
    if len(pages) != 64:
        sys.exit("expected 64 waveform pages (3-66), got %d" % len(pages))
    return pages


def write_header(waves, scale):
    q = 32767.0 / max(max(np.abs(co).max(), np.abs(si).max()) for _, co, si, _ in waves)
    out = ["/* Generated by tools/trace_dwgs.py: the microKORG's 64 DWGS waveforms as Fourier series, digitised\n",
           " * from scope plots of the instrument's output (see the tool for the method and its limits).\n",
           " * A table spans `periods` periods of the note and holds harmonics 1..count of that span, as\n",
           " * (cos, sin) int16 pairs; value * DWGS_COEF_SCALE is the amplitude with the loudest wave's peak at 1. */\n",
           "#ifndef TINYK_DWGS_WAVES_H\n#define TINYK_DWGS_WAVES_H\n\n",
           "#define DWGS_WAVE_COUNT 64\n",
           "#define DWGS_HARMONICS %d      /* per period of the note */\n" % HARMONICS,
           "#define DWGS_MAX_PERIODS %d\n" % max(p for p, _, _, _ in waves),
           "#define DWGS_TOTAL_PERIODS %d\n" % sum(p for p, _, _, _ in waves),
           "#define DWGS_COEF_SCALE %.9ef\n\n" % (scale / q)]
    out.append("static const char *const DWGS_NAMES[DWGS_WAVE_COUNT] = {\n")
    for i in range(0, 64, 8):
        out.append("    " + ", ".join('"%s"' % n for n in NAMES[i:i + 8]) + ",\n")
    out.append("};\n\n")
    for i, (periods, co, si, _) in enumerate(waves):
        vals = np.round(np.column_stack([co, si]).ravel() * q).astype(int)
        out.append("static const short DWGS_COEF_%02d[%d] = { /* %s */\n" % (i + 1, vals.size, NAMES[i]))
        for j in range(0, vals.size, 24):
            out.append("    " + ",".join(str(v) for v in vals[j:j + 24]) + ",\n")
        out.append("};\n")
    out.append("\nstatic const struct { const short *coef; short periods, count; } DWGS_WAVES[DWGS_WAVE_COUNT] = {\n")
    for i, (periods, co, _, _) in enumerate(waves):
        out.append("    { DWGS_COEF_%02d, %d, %d },\n" % (i + 1, periods, co.size))
    out.append("};\n\n#endif\n")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pdf")
    ap.add_argument("--preview", metavar="DIR", help="write overlay PNGs of the fit on each page")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        pages = render_pages(args.pdf, tmp)
        waves = []
        for n, page in enumerate(pages, 1):
            y = trace(page)
            if n in SPARSE:
                periods = SPARSE[n]
                co, si, rms = esprit_fit(y, periods)
            else:
                periods, stride = LOOPS.get(n, (1, 1))
                co, si, rms = fourier_fit(y, periods, stride)
            waves.append((periods, co, si, rms))
            peak = np.abs(synth(co, si, 2048 * periods)).max()
            print("%2d %-9s periods %d  traced %4d/%d columns  residual %5.2f mV  peak %5.1f mV"
                  % (n, NAMES[n - 1], periods, np.isfinite(y).sum(), y.size, rms, peak))
            if args.preview:
                os.makedirs(args.preview, exist_ok=True)
                im = Image.open(page).convert("RGB")
                x = np.arange(y.size) + 0.5
                ph = 2 * np.pi * np.outer(x / (PERIOD * periods), np.arange(1, co.size + 1))
                fit = np.cos(ph) @ co + np.sin(ph) @ si
                ImageDraw.Draw(im).line([(X0 + i, ZERO_ROW - v * PX_PER_MV) for i, v in enumerate(fit)],
                                        fill=(255, 0, 0), width=1)
                im.save(os.path.join(args.preview, "%02d_%s.png" % (n, NAMES[n - 1])))
    top = max(np.abs(synth(co, si, 2048 * p)).max() for p, co, si, _ in waves)
    write_header(waves, 1.0 / top)
    print("loudest peak %.1f mV -> 1.0; wrote %s" % (top, os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
