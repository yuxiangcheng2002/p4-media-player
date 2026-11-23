#!/usr/bin/env python3
"""Transcode any video into what the P4 player plays: 1920x1080 (or 1280x720)
MJPEG in AVI, true 4:2:2 JPEG (luma 2x1), no audio, source frame rate kept.

Why not plain ffmpeg: its mjpeg encoder writes "4:2:2" with luma 2x2 sampling,
which the P4 decodes to 1088 lines (2026-11-22). Frames are therefore encoded
with libjpeg via Pillow (subsampling=1 = luma 2x1).

Single pass, no temporary images (a 4K source would need tens of GB): ffmpeg decodes
and scales to raw RGB on a pipe, Pillow encodes each frame once per requested
quality, and one ffmpeg muxer per quality writes the AVI with -c:v copy.

Frame-size cap (--max-kb): storage, not the decoder, limits the P4 (decode is
27 ms per 1080p frame whatever its size). Real footage has bursts (grain,
fast detail) where q90 frames reach ~1.3 MB, more than a USB drive delivers at
30 fps. With a cap, each frame is encoded at the target quality and, only if it
is larger than the cap, re-encoded at the highest lower quality that fits.
Measured: 512 KB for the USB-C Host port (raw reads 20-22 MB/s; worst
one-second window of a 4K test clip 15.6 MB/s), ~280 KB for the add-on's
microSD slot (~12 MB/s).

The player shows each file at the frame rate recorded in the AVI; use sources
at 30 fps or below (1080p is driven at ~35 Hz).

Usage:
  tools/p4_transcode.py input.mp4 outdir --quality 90 --max-kb 512 [--height 720]
Prints per-quality average/max frame size, the read rate needed, and how
many frames the cap touched.
"""
import argparse
import io
import json
import os
import subprocess
import sys
import time

from PIL import Image


def probe(path):
    out = subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
        "stream=width,height,r_frame_rate,nb_frames", "-of", "json", path])
    s = json.loads(out)["streams"][0]
    return s


def encode(img, q):
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=q, subsampling=1)
    return buf.getvalue()


def encode_capped(img, q, cap):
    """Largest quality <= q whose frame fits in cap bytes; returns (data, q_used)."""
    data = encode(img, q)
    if not cap or len(data) <= cap:
        return data, q
    lo, hi, best = 10, q - 1, None
    while lo <= hi:  # binary search, ~5 encodes
        mid = (lo + hi) // 2
        d = encode(img, mid)
        if len(d) <= cap:
            best, lo = (d, mid), mid + 1
        else:
            hi = mid - 1
    if best:
        return best
    d = encode(img, 10)  # nothing fits: lowest quality, reported by the caller
    return d, 10


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("outdir")
    ap.add_argument("--quality", type=int, nargs="+", default=[70, 80, 90])
    ap.add_argument("--height", type=int, default=1080, choices=[1080, 720])
    ap.add_argument("--max-kb", type=int, default=0, help="per-frame size cap in KB (0 = none)")
    a = ap.parse_args()

    info = probe(a.input)
    rate = info["r_frame_rate"]  # keep the source rate exactly (e.g. 30000/1001)
    num, den = (int(x) for x in rate.split("/"))
    if den == 0 or num / den > 30.5:
        sys.exit(f"source is {rate} fps; the player shows at most 30 fps at 1080p. "
                 "Convert first, e.g. ffmpeg -i in.mp4 -r 30 -c:v libx264 -crf 12 in30.mp4")
    h = a.height
    w = 1920 if h == 1080 else 1280
    os.makedirs(a.outdir, exist_ok=True)
    base = os.path.splitext(os.path.basename(a.input))[0]

    # Letterbox into the frame without distortion; lanczos for 4K -> 1080p.
    vf = (f"scale={w}:{h}:force_original_aspect_ratio=decrease:flags=lanczos,"
          f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2")
    dec = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", a.input, "-an", "-sn", "-dn",
         "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE, bufsize=w * h * 3 * 4)

    muxers, stats, outs = {}, {}, {}
    for q in a.quality:
        tag = f"q{q}" + (f"_max{a.max_kb}k" if a.max_kb else "")
        out = os.path.join(a.outdir, f"{base}_p4_{h}p_422_{tag}.avi")
        outs[q] = out
        muxers[q] = subprocess.Popen(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "image2pipe",
             "-framerate", rate, "-c:v", "mjpeg", "-i", "-", "-c:v", "copy", out],
            stdin=subprocess.PIPE)
        stats[q] = [0, 0, 0, 0, 100, 0]  # frames, bytes, max bytes, capped, lowest q, over cap

    frame_bytes = w * h * 3
    t0 = time.time()
    n = 0
    while True:
        raw = dec.stdout.read(frame_bytes)
        if len(raw) < frame_bytes:
            break
        img = Image.frombytes("RGB", (w, h), raw)
        for q in a.quality:
            data, q_used = encode_capped(img, q, a.max_kb * 1024)
            try:
                muxers[q].stdin.write(data)
            except BrokenPipeError:
                dec.kill()
                for out in outs.values():
                    if os.path.exists(out):
                        os.remove(out)
                sys.exit("ffmpeg muxer stopped (see its messages above); no output kept")
            st = stats[q]
            st[0] += 1
            st[1] += len(data)
            st[2] = max(st[2], len(data))
            if q_used != q:
                st[3] += 1
                st[4] = min(st[4], q_used)
            if a.max_kb and len(data) > a.max_kb * 1024:
                st[5] += 1  # over the cap even at q10
        n += 1
        if n % 300 == 0:
            print(f"  {n} frames, {n / (time.time() - t0):.1f} fps", file=sys.stderr)
    failed = dec.wait() != 0
    for q, m in muxers.items():
        m.stdin.close()
        if m.wait() != 0:
            failed = True
    if failed or n == 0:
        for out in outs.values():
            if os.path.exists(out):
                os.remove(out)  # never leave a truncated file that looks finished
        sys.exit("ffmpeg failed (see its messages above); no output kept")

    fps = num / den
    print(f"source {info['width']}x{info['height']} @ {fps:.3f} fps -> {w}x{h}, {n} frames, "
          f"{time.time() - t0:.0f} s")
    for q in a.quality:
        frames, total, mx, capped, qlow, over = stats[q]
        avg = total / max(frames, 1)
        print(f"q{q}: {os.path.basename(outs[q])}  {os.path.getsize(outs[q]) / 1e6:.0f} MB  "
              f"avg {avg / 1024:.0f} KB/frame  max {mx / 1024:.0f} KB  "
              f"needs {avg * fps / 1e6:.2f} MB/s avg, {mx * fps / 1e6:.2f} MB/s at the largest frame"
              + (f"  capped {capped} frames (lowest q{qlow})" if a.max_kb else "")
              + (f"  WARNING: {over} frames still over the cap at q10" if over else ""))


if __name__ == "__main__":
    main()
