#!/usr/bin/env python3
"""Diagnostic clip for the P4 player's picture path (2026-11-25).

Two questions, one clip, written straight in YCbCr so the values on the drive
are the values the P4's DSI bridge converts (Pillow saves a YCbCr image without
any colour conversion):

1. Top half, a 4x4 grid of flat patches: luma Y in {235, 245, 250, 255} by
   red-difference offset Cr-128 in {0, +6, +12, +20} (Cb neutral). Converted
   BT.601 full range, R = Y + 1.402 (Cr-128) exceeds 255 in the lower-right
   patches. If the bridge clamps, those patches look near-white or pink; if it
   wraps, R comes out small and the patch turns cyan. Each patch is labelled
   "Y/dCr" and the computed R in the label.
2. Bottom half: a white bar moving 24 px per frame across a dark field, with
   the frame number. A torn frame shows the bar broken at one height; dropped
   or repeated frames show as jumps in the counter.

Usage: tools/make_diag_clip.py outdir [--height 720|1080] [--seconds 10]
"""
import argparse
import io
import os
import subprocess

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def frame(w, h, n, fnt_big, fnt_small):
    Y = np.full((h, w), 16, np.uint8)
    Cb = np.full((h, w), 128, np.uint8)
    Cr = np.full((h, w), 128, np.uint8)
    labels = []
    # Top half: patch grid.
    top = h // 2
    ys, dcrs = [235, 245, 250, 255], [0, 6, 12, 20]
    pw, ph = w // 4, top // 4
    for r, dcr in enumerate(dcrs):
        for c, y in enumerate(ys):
            x0, y0 = c * pw + 4, r * ph + 4
            Y[y0:y0 + ph - 8, x0:x0 + pw - 8] = y
            Cr[y0:y0 + ph - 8, x0:x0 + pw - 8] = 128 + dcr
            labels.append((x0 + 8, y0 + 6, f"Y{y} dCr+{dcr} R={y + 1.402 * dcr:.0f}"))
    # Bottom half: moving bar.
    bw = max(24, w // 48)
    span = w + bw
    x = (n * 24) % span - bw
    xa, xb = max(0, x), min(w, x + bw)
    if xb > xa:
        Y[top + 8:h - 8, xa:xb] = 235
    # Text goes into the luma plane only (Cb/Cr stay neutral: grey text).
    ly = Image.fromarray(Y)  # 2-D uint8 -> mode L
    d = ImageDraw.Draw(ly)
    for tx, ty, s in labels:
        d.text((tx, ty), s, fill=40, font=fnt_small)
    d.text((16, top + 16), f"frame {n:04d}", fill=200, font=fnt_big)
    Y = np.asarray(ly)
    img = Image.merge("YCbCr", [Image.fromarray(p) for p in (Y, Cb, Cr)])
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=95, subsampling=1)  # 4:2:2, luma 2x1 like p4_transcode
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--height", type=int, default=720, choices=[720, 1080])
    ap.add_argument("--seconds", type=int, default=10)
    a = ap.parse_args()
    h = a.height
    w = 1920 if h == 1080 else 1280
    os.makedirs(a.outdir, exist_ok=True)
    out = os.path.join(a.outdir, f"diag_{h}p_422.avi")
    mux = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "image2pipe",
                            "-framerate", "30", "-c:v", "mjpeg", "-i", "-", "-c:v", "copy", out],
                           stdin=subprocess.PIPE)
    fb, fs = font(h // 12), font(h // 40)
    for n in range(a.seconds * 30):
        mux.stdin.write(frame(w, h, n, fb, fs))
    mux.stdin.close()
    if mux.wait() != 0:
        raise SystemExit("ffmpeg failed")
    print(out, os.path.getsize(out) // 1024, "KB")


if __name__ == "__main__":
    main()
