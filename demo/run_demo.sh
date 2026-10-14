#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${FRAMEFORGE_BIN:-$ROOT/build/frameforge}"
GENERATED="${FRAMEFORGE_DEMO_MEDIA:-$ROOT/demo/generated}"
OUTPUT="${FRAMEFORGE_DEMO_OUTPUT:-$ROOT/demo/output}"
FFMPEG="${FFMPEG:-ffmpeg}"

if [[ ! -x "$BIN" ]]; then
  echo "Build FrameForge first: cmake -S '$ROOT' -B '$ROOT/build' -DCMAKE_BUILD_TYPE=Release && cmake --build '$ROOT/build' -j" >&2
  exit 1
fi
command -v "$FFMPEG" >/dev/null || { echo "ffmpeg CLI is required for demo generation" >&2; exit 1; }
mkdir -p "$GENERATED" "$OUTPUT"

# Short low-resolution clip with a deliberate, known scene cut at frame 12.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'color=c=black:s=160x90:r=12:d=1' \
  -f lavfi -i 'color=c=white:s=160x90:r=12:d=1' \
  -filter_complex '[0:v][1:v]concat=n=2:v=1:a=0' -an -c:v libx264 -preset ultrafast \
  -crf 40 -pix_fmt yuv420p -movflags +faststart -y "$GENERATED/short-low-quality.mp4"
# Other clips use the same moving test pattern at distinct resolutions, CRF quality, and durations.
"$FFMPEG" -hide_banner -loglevel error -f lavfi \
  -i 'testsrc2=size=640x360:rate=24:duration=6' -an -c:v libx264 -preset ultrafast \
  -crf 28 -pix_fmt yuv420p -movflags +faststart -y "$GENERATED/medium-6s.mp4"
"$FFMPEG" -hide_banner -loglevel error -f lavfi \
  -i 'testsrc2=size=1280x720:rate=30:duration=6' -an -c:v libx264 -preset ultrafast \
  -crf 18 -pix_fmt yuv420p -movflags +faststart -y "$GENERATED/hd-high-quality-6s.mp4"
"$FFMPEG" -hide_banner -loglevel error -f lavfi \
  -i 'testsrc2=size=640x360:rate=24:duration=30' -an -c:v libx264 -preset ultrafast \
  -crf 30 -pix_fmt yuv420p -movflags +faststart -y "$GENERATED/long-30s.mp4"

"$BIN" analyze "$GENERATED/short-low-quality.mp4" --output "$OUTPUT/short-low-quality.json"
"$BIN" analyze "$GENERATED/medium-6s.mp4" --output "$OUTPUT/medium-6s.json"
"$BIN" analyze "$GENERATED/hd-high-quality-6s.mp4" --output "$OUTPUT/hd-high-quality-6s.json"
"$BIN" analyze "$GENERATED/long-30s.mp4" --output "$OUTPUT/long-30s.json"
"$BIN" extract "$GENERATED/hd-high-quality-6s.mp4" --out "$OUTPUT/selected-hd-frames" \
  --ranges 0:2,90:92 --threads 0

python3 - "$OUTPUT" <<'PY'
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
rows = []
for report in sorted(out.glob('*.json')):
    data = json.loads(report.read_text())
    stream = data['stream']
    summary = data['analysis']['summary']
    row = (f"| {report.stem} | {stream['width']}×{stream['height']} "
           f"| {stream['nominal_fps']:g} | {summary['decoded_frame_count']} "
           f"| {summary['observed_duration_seconds']:.3f} "
           f"| {stream['bit_rate_bits_per_second']} | {summary['scene_count']} |")
    rows.append(row)
    print(f"{report.stem:22s} {stream['width']}x{stream['height']} "
          f"{stream['nominal_fps']:g} fps  {summary['decoded_frame_count']} frames  "
          f"{summary['scene_count']} detected segment(s)")
summary = ["# FrameForge demonstration results", "",
           "Synthetic FFmpeg test-pattern clips; these are functional/performance-path "
           "checks, not semantic-analysis benchmarks.", "",
           "| Profile | Resolution | FPS | Decoded frames | Duration (s) | Bitrate (bit/s) | Scene segments |",
           "|---|---:|---:|---:|---:|---:|---:|"] + rows + ["",
           "HD frame extraction exports indexes 0–2 and 90–92 in `selected-hd-frames/`.",
           "The sidecar manifest contains each selected frame index/timestamp and confirms "
           "the 180-frame source count.", ""]
(out / 'summary.md').write_text('\n'.join(summary))
PY

echo "Full-resolution PNG extraction and manifest: $OUTPUT/selected-hd-frames"
echo "Reports and generated clips are under: $OUTPUT and $GENERATED"
