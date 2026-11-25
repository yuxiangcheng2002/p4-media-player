#!/usr/bin/env bash
# Make loop-test clips (macOS/BSD stat; needs ffmpeg, Pillow, bc).
# For your own videos use tools/p4_transcode.py, which also caps frame sizes.
#
# Usage:
#   tools/make_clips.sh                 # synthetic 10 s test pattern
#   tools/make_clips.sh input.mp4       # convert your own video instead
#   OUT=dir tools/make_clips.sh [input] # output directory (default: clips/)
#
# Why a timestamp pattern: ffmpeg's testsrc draws a running time counter with
# two decimals, so on a filmed screen a seam shows as 9.9x -> 0.00 with no
# repeated or black frame in between. (Homebrew ffmpeg has no drawtext, so
# testsrc's built-in counter is used instead.)
#
# Outputs:
#   p4_1080p_422.avi        MJPEG, true 4:2:2 (luma 2x1): what the P4 player expects
#   p4_1080p_422_noisy.avi  same, worst-case frame size for the USB/SD and PSRAM budget
#   p4_720p_422.avi         same at 1280x720 (plays with mode = 720 in config.txt)
#   p4_1080p_ffmpeg2x2.avi  ffmpeg's own "4:2:2" (luma 2x2): decodes to 1088 lines,
#                           rejected by the default (YUV422) player, which needs
#                           true 4:2:2; exercises the copy fallback in fmt 1 builds
#   sbc_1080p_hevc.mp4      H.265 / H.264 references for a Linux single-board-computer
#   sbc_1080p_h264.mp4      player (not part of this repository); not used by the P4
#
# Why P4 clips go through Pillow: ffmpeg's mjpeg encoder writes "yuvj422p" with
# luma sampling 2x2 (16-line MCUs), which the P4 decodes to 1088 lines and
# which cannot go zero-copy into a 1080-line frame buffer (2026-11-22).
# libjpeg (Pillow, subsampling=1) writes 2x1. Frames are encoded one by one and
# muxed into AVI without re-encoding.
# All outputs are 30 fps constant frame rate, no audio, every-second keyframe.
set -euo pipefail

OUT="${OUT:-clips}"
mkdir -p "$OUT"

if [[ $# -ge 1 ]]; then
  SRC=(-i "$1")
else
  SRC=(-f lavfi -i "testsrc=size=1920x1080:rate=30:decimals=2:duration=10")
fi

# Letterbox any input into 1920x1080 without distortion, force CFR 30.
FIT="scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,fps=30"
FIT720="scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2,fps=30"

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# P4: ffmpeg renders PNG frames, Pillow encodes true 4:2:2 JPEG (quality 90 is
# about ffmpeg -q:v 4 in size), ffmpeg muxes them into AVI with -c:v copy.
p4_clip() {  # p4_clip <filter> <out.avi>
  local T; T=$(mktemp -d)
  ff "${SRC[@]}" -an -vf "$1" "$T/%05d.png"
  python3 - "$T" <<'PY'
import glob, os, sys
from PIL import Image
for f in sorted(glob.glob(os.path.join(sys.argv[1], "*.png"))):
    Image.open(f).convert("RGB").save(f[:-4] + ".jpg", quality=90, subsampling=1)
    os.remove(f)
PY
  ff -framerate 30 -i "$T/%05d.jpg" -c:v copy "$2"
  rm -rf "$T"
}
p4_clip "$FIT" "$OUT/p4_1080p_422.avi"
# The test pattern is flat (~90 KB/frame); temporal noise gives a worst case.
p4_clip "$FIT,noise=alls=25:allf=t" "$OUT/p4_1080p_422_noisy.avi"
p4_clip "$FIT720" "$OUT/p4_720p_422.avi"
ff "${SRC[@]}" -an -vf "$FIT,format=yuvj422p" -c:v mjpeg -q:v 4 "$OUT/p4_1080p_ffmpeg2x2.avi"

# SBC: closed GOP, keyframe every 30 frames so the loop restart lands on an IDR.
ff "${SRC[@]}" -an -vf "$FIT,format=yuv420p" -c:v libx265 -preset slow -crf 20 \
   -x265-params "keyint=30:min-keyint=30:scenecut=0:open-gop=0:log-level=error" \
   -tag:v hvc1 -movflags +faststart "$OUT/sbc_1080p_hevc.mp4"
ff "${SRC[@]}" -an -vf "$FIT,format=yuv420p" -c:v libx264 -profile:v high -level 4.1 \
   -preset slow -crf 18 -g 30 -keyint_min 30 -sc_threshold 0 \
   -movflags +faststart "$OUT/sbc_1080p_h264.mp4"

# Report what was made, including average bytes per frame for the USB budget.
for f in "$OUT"/p4_1080p_422.avi "$OUT"/p4_1080p_422_noisy.avi "$OUT"/p4_720p_422.avi \
         "$OUT"/p4_1080p_ffmpeg2x2.avi "$OUT"/sbc_1080p_hevc.mp4 "$OUT"/sbc_1080p_h264.mp4; do
  [ -f "$f" ] || continue
  frames=$(ffprobe -v error -count_packets -select_streams v:0 \
           -show_entries stream=nb_read_packets -of csv=p=0 "$f")
  bytes=$(stat -f %z "$f")
  info=$(ffprobe -v error -select_streams v:0 \
         -show_entries stream=codec_name,width,height,pix_fmt,r_frame_rate -of csv=p=0 "$f")
  printf '%-24s %-40s %6d frames  %7.1f KB/frame  %6.2f MB/s @30fps\n' \
    "$(basename "$f")" "$info" "$frames" \
    "$(echo "$bytes/$frames/1024" | bc -l)" "$(echo "$bytes/$frames*30/1048576" | bc -l)"
done
