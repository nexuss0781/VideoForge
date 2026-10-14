#!/usr/bin/env bash
set -euo pipefail

BIN="$1"
FFMPEG="$2"
PYTHON="$3"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Two flat-color shots force a known scene boundary at frame 12.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'color=c=black:s=160x90:r=12:d=1' \
  -f lavfi -i 'color=c=white:s=160x90:r=12:d=1' \
  -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0' \
  -an -c:v libx264 -preset ultrafast -crf 40 -pix_fmt yuv420p -y "$TMP/short-low.mp4"

# A higher-resolution and higher-quality, longer test clip.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=640x360:rate=24:duration=3' \
  -an -c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p -y "$TMP/hd.mp4"

"$BIN" analyze "$TMP/short-low.mp4" --output "$TMP/short.json"
"$BIN" analyze "$TMP/hd.mp4" --output "$TMP/hd.json"
"$BIN" extract "$TMP/short-low.mp4" --out "$TMP/frames" --indices 0,12 --ranges 12:13,23:23
"$BIN" extract "$TMP/hd.mp4" --out "$TMP/hd-frames" --ranges 24:25,71:71

"$PYTHON" - "$TMP" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
short = json.loads((root / 'short.json').read_text())
hd = json.loads((root / 'hd.json').read_text())
assert short['stream']['width'] == 160 and short['stream']['height'] == 90
assert short['analysis']['summary']['decoded_frame_count'] == 24
assert short['analysis']['summary']['scene_count'] >= 2
assert short['analysis']['summary']['segments'][0]['end_frame'] == 11
assert hd['stream']['width'] == 640 and hd['stream']['height'] == 360
assert hd['analysis']['summary']['decoded_frame_count'] == 72
assert hd['analysis']['frames'][0]['index'] == 0
assert hd['analysis']['frames'][-1]['index'] == 71
manifest = json.loads((root / 'frames' / 'manifest.json').read_text())
assert manifest['decoded_frame_count'] == 24
assert manifest['exported_count'] == 4
assert [item['index'] for item in manifest['frames']] == [0, 12, 13, 23]
for item in manifest['frames']:
    data = (root / 'frames' / item['file']).read_bytes()
    assert data.startswith(b'\x89PNG\r\n\x1a\n')
hd_manifest = json.loads((root / 'hd-frames' / 'manifest.json').read_text())
assert hd_manifest['decoded_frame_count'] == 72 and hd_manifest['exported_count'] == 3
print('integration assertions passed: low/HD decode counts, scene cut, and 7 PNGs')
PY

if "$BIN" extract "$TMP/short-low.mp4" --out "$TMP/oob" --indices 999 >/dev/null 2>&1; then
  echo 'out-of-range frame selection unexpectedly succeeded' >&2
  exit 1
fi

echo 'out-of-range selection correctly rejected'
