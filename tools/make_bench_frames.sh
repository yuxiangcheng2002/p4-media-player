#!/usr/bin/env bash
# Single JPEG frames (1920x1080 in bench/1080, 1280x720 in bench/720) embedded in p4_loop for the headless decode
# bench (no storage needed): flat test pattern, a detailed smooth-gradient
# image (ffmpeg's mandelbrot source, standing in for a photo), a ~185 KB
# "typical" frame and a temporal-noise worst case, all 4:2:2, plus the
# typical frame in 4:4:4 for comparison.
#
# Why Pillow and not ffmpeg for the JPEG step: ffmpeg's mjpeg encoder writes
# "4:2:2" with luma sampling 2x2 (16-line MCUs), so a 1080-line frame decodes
# to 1088 lines on the P4 and no longer fits the frame buffer (measured
# 2026-11-22). libjpeg via Pillow writes 4:2:2 as luma 2x1 (8-line MCUs),
# which decodes to exactly 1080 lines.
#
# Every frame is generated from ffmpeg's own test sources, so the set carries
# no third-party content. Frames are written to a temporary directory and
# replace the old set only when all of them were produced.
# Needs ffmpeg and Pillow (pip install pillow).
set -euo pipefail
for RES in 1080 720; do
W=$([ $RES = 1080 ] && echo 1920 || echo 1280)
OUT=firmware/p4_loop/main/bench/$RES
mkdir -p "$OUT"
S=$(mktemp -d)
J=$(mktemp -d)
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
ff -f lavfi -i "testsrc=size=${W}x${RES}:rate=30:decimals=2" -frames:v 1 "$S/flat.png"
ff -f lavfi -i "mandelbrot=size=${W}x${RES}:rate=30" -frames:v 1 "$S/photo.png"
ff -f lavfi -i "testsrc=size=${W}x${RES}:rate=30:decimals=2" -frames:v 1 -vf "noise=alls=8:allf=t" "$S/typical.png"
ff -f lavfi -i "testsrc=size=${W}x${RES}:rate=30:decimals=2" -frames:v 1 -vf "noise=alls=25:allf=t" "$S/noisy.png"
python3 - "$S" "$J" <<'PY'
import sys
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
# subsampling: 1 = 4:2:2 (luma 2x1), 0 = 4:4:4. quality 90 ~ ffmpeg -q:v 4 in size.
# Names carry no resolution so the firmware's embed symbols are the same for both builds.
for name in ("flat", "photo", "typical", "noisy"):
    Image.open(f"{src}/{name}.png").convert("RGB").save(f"{out}/{name}_422.jpg", quality=90, subsampling=1)
Image.open(f"{src}/typical.png").convert("RGB").save(f"{out}/typical_444.jpg", quality=90, subsampling=0)
PY
[ "$(ls "$J"/*.jpg | wc -l)" -eq 5 ]  # all five frames, or stop before touching $OUT
rm -f "$OUT"/*.jpg
mv "$J"/*.jpg "$OUT"/
rm -rf "$S" "$J"
ls -l "$OUT"
done
