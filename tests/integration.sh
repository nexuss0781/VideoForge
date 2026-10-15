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

# A separate 10-bit source verifies that no intermediate RGB conversion truncates samples.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=160x90:rate=12:duration=1' \
  -vf format=yuv420p10le -an -c:v ffv1 -pix_fmt yuv420p10le -y "$TMP/high10.mkv"

# NV12 is not an FFV1 pixel format here, so unpack must use the exact-format fallback.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=160x90:rate=12:duration=1' \
  -pix_fmt nv12 -an -c:v rawvideo -f nut -y "$TMP/nv12.nut"

# Audio must not disappear when the decoded-plane candidate is smaller.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=160x90:rate=12:duration=1' \
  -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' \
  -map 0:v:0 -map 1:a:0 -c:v libx264 -preset ultrafast -crf 30 \
  -c:a aac -shortest -y "$TMP/with-audio.mp4"

# Uncompressed video and PCM audio make the frame-plane candidate small enough
# to win while exercising compressed audio packet storage and exact remuxing.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'color=c=black:s=160x90:r=12:d=1' \
  -an -c:v rawvideo -pix_fmt yuv420p -f nut -y "$TMP/audio-video-only.nut"
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'color=c=black:s=160x90:r=12:d=1' \
  -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' \
  -map 0:v:0 -map 1:a:0 -c:v rawvideo -pix_fmt yuv420p \
  -c:a pcm_s16le -shortest -f nut -y "$TMP/audio-pcm.nut"
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'color=c=black:s=160x90:r=12:d=1' \
  -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' \
  -map 0:v:0 -map 1:a:0 -c:v rawvideo -pix_fmt yuv420p \
  -c:a aac -shortest -f nut -y "$TMP/audio-aac.nut"

# Explicit colorimetry changes how decoded YUV samples are displayed.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'testsrc2=size=640x360:rate=24:duration=1' \
  -vf 'format=yuv420p,setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709' \
  -color_range tv -c:v libx264 -preset ultrafast -qp 0 -pix_fmt yuv420p \
  -y "$TMP/color-metadata.mkv"

# A live Matroska fixture avoids a generated DURATION tag so the measured native
# candidate can be selected while exercising supported display metadata.
"$FFMPEG" -hide_banner -loglevel error \
  -f lavfi -i 'color=c=black:s=640x360:r=25:d=1' \
  -vf 'format=yuv420p,setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709:field_mode=tff,setsar=4/3' \
  -an -c:v ffv1 -level 3 -pix_fmt yuv420p -color_range tv \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 \
  -chroma_sample_location left -live 1 -y "$TMP/display-metadata.mkv"

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

"$BIN" pack "$TMP/short-low.mp4" --out "$TMP/short.vfs" --threads 2 \
  > "$TMP/pack-stats.json"
"$BIN" unpack "$TMP/short.vfs" --out "$TMP/restored.nut" \
  > "$TMP/unpack-stats.json"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/short-low.mp4" \
  -f rawvideo -pix_fmt yuv420p -y "$TMP/source.yuv"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/restored.nut" \
  -f rawvideo -pix_fmt yuv420p -y "$TMP/restored.yuv"
cmp "$TMP/source.yuv" "$TMP/restored.yuv"

"$BIN" pack "$TMP/hd.mp4" --out "$TMP/hd.vfs" --threads 2 \
  > "$TMP/hd-pack-stats.json"
"$BIN" unpack "$TMP/hd.vfs" --out "$TMP/hd-restored.mp4" \
  > "$TMP/hd-unpack-stats.json"
cmp "$TMP/hd.mp4" "$TMP/hd-restored.mp4"
"$BIN" pack "$TMP/hd.mp4" --out "$TMP/hd-repeat.vfs" --threads 2 \
  > "$TMP/hd-repeat-stats.json"
cmp "$TMP/hd.vfs" "$TMP/hd-repeat.vfs"

"$BIN" pack "$TMP/high10.mkv" --out "$TMP/high10.vfs" --compression 1 \
  > "$TMP/high10-pack-stats.json"
"$BIN" unpack "$TMP/high10.vfs" --out "$TMP/high10-restored.nut" \
  > "$TMP/high10-unpack-stats.json"
cmp "$TMP/high10.mkv" "$TMP/high10-restored.nut"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/high10.mkv" \
  -f rawvideo -pix_fmt yuv420p10le -y "$TMP/high10-source.raw"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/high10-restored.nut" \
  -f rawvideo -pix_fmt yuv420p10le -y "$TMP/high10-restored.raw"
cmp "$TMP/high10-source.raw" "$TMP/high10-restored.raw"
if command -v ffprobe >/dev/null 2>&1; then
  test "$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt \
    -of default=nw=1:nk=1 "$TMP/high10-restored.nut")" = "yuv420p10le"
fi

"$BIN" pack "$TMP/nv12.nut" --out "$TMP/nv12.vfs" --compression 1 \
  > "$TMP/nv12-pack-stats.json"
"$BIN" pack "$TMP/nv12.nut" --out "$TMP/nv12-repeat.vfs" --compression 1 \
  > "$TMP/nv12-repeat-pack-stats.json"
cmp "$TMP/nv12.vfs" "$TMP/nv12-repeat.vfs"
"$BIN" unpack "$TMP/nv12.vfs" --out "$TMP/nv12-restored.nut" \
  > "$TMP/nv12-unpack-stats.json"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/nv12.nut" \
  -f rawvideo -pix_fmt nv12 -y "$TMP/nv12-source.raw"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/nv12-restored.nut" \
  -f rawvideo -pix_fmt nv12 -y "$TMP/nv12-restored.raw"
cmp "$TMP/nv12-source.raw" "$TMP/nv12-restored.raw"

"$BIN" pack "$TMP/with-audio.mp4" --out "$TMP/with-audio.vfs" \
  > "$TMP/with-audio-pack-stats.json"
"$BIN" unpack "$TMP/with-audio.vfs" --out "$TMP/with-audio-restored.mp4" \
  > "$TMP/with-audio-unpack-stats.json"
cmp "$TMP/with-audio.mp4" "$TMP/with-audio-restored.mp4"

"$BIN" pack "$TMP/audio-video-only.nut" --out "$TMP/audio-video-only.vfs" \
  > "$TMP/audio-video-only-pack-stats.json"
"$BIN" pack "$TMP/audio-pcm.nut" --out "$TMP/audio-pcm.vfs" \
  > "$TMP/audio-pcm-pack-stats.json"
"$BIN" pack "$TMP/audio-pcm.nut" --out "$TMP/audio-pcm-repeat.vfs" \
  > "$TMP/audio-pcm-repeat-pack-stats.json"
cmp "$TMP/audio-pcm.vfs" "$TMP/audio-pcm-repeat.vfs"
"$BIN" unpack "$TMP/audio-pcm.vfs" --out "$TMP/audio-pcm-restored.nut" \
  > "$TMP/audio-pcm-unpack-stats.json"
"$BIN" pack "$TMP/audio-aac.nut" --out "$TMP/audio-aac.vfs" \
  > "$TMP/audio-aac-pack-stats.json"
"$BIN" unpack "$TMP/audio-aac.vfs" --out "$TMP/audio-aac-restored.nut" \
  > "$TMP/audio-aac-unpack-stats.json"

if command -v ffprobe >/dev/null 2>&1; then
  ffprobe -v error -select_streams a:0 -show_packets -show_data_hash sha256 \
    -show_entries packet=pts,dts,duration,flags,data_hash \
    -show_entries stream=codec_name,time_base,sample_rate,channels,channel_layout \
    -of json "$TMP/audio-pcm.nut" > "$TMP/audio-pcm-source-audio.json"
  ffprobe -v error -select_streams a:0 -show_packets -show_data_hash sha256 \
    -show_entries packet=pts,dts,duration,flags,data_hash \
    -show_entries stream=codec_name,time_base,sample_rate,channels,channel_layout \
    -of json "$TMP/audio-pcm-restored.nut" > "$TMP/audio-pcm-restored-audio.json"
  ffprobe -v error -select_streams v:0 -show_frames \
    -show_entries stream=time_base -show_entries frame=best_effort_timestamp \
    -of json "$TMP/audio-pcm.nut" > "$TMP/audio-pcm-source-video.json"
  ffprobe -v error -select_streams v:0 -show_frames \
    -show_entries stream=time_base -show_entries frame=best_effort_timestamp \
    -of json "$TMP/audio-pcm-restored.nut" > "$TMP/audio-pcm-restored-video.json"
  ffprobe -v error -select_streams a:0 -show_packets -show_data_hash sha256 \
    -show_entries packet=pts,dts,duration,flags,data_hash \
    -show_entries stream=codec_name,time_base,sample_rate,channels \
    -of json "$TMP/audio-aac.nut" > "$TMP/audio-aac-source.json"
  ffprobe -v error -select_streams a:0 -show_packets -show_data_hash sha256 \
    -show_entries packet=pts,dts,duration,flags,data_hash \
    -show_entries stream=codec_name,time_base,sample_rate,channels \
    -of json "$TMP/audio-aac-restored.nut" > "$TMP/audio-aac-restored.json"
  "$PYTHON" - "$TMP" <<'PY'
from fractions import Fraction
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
source_audio = json.loads((root / 'audio-pcm-source-audio.json').read_text())
restored_audio = json.loads((root / 'audio-pcm-restored-audio.json').read_text())
sa, ra = source_audio['streams'][0], restored_audio['streams'][0]
assert sa['codec_name'] == ra['codec_name'] == 'pcm_s16le'
assert sa['sample_rate'] == ra['sample_rate'] == '48000'
assert sa['channels'] == ra['channels'] == 1
ta, tb = Fraction(sa['time_base']), Fraction(ra['time_base'])
sp, rp = source_audio['packets'], restored_audio['packets']
assert len(sp) == len(rp) > 0
for source, restored in zip(sp, rp):
    assert source['data_hash'] == restored['data_hash']
    assert source['flags'] == restored['flags']
    for key in ('pts', 'dts', 'duration'):
        if key in source and key in restored:
            assert int(source[key]) * ta == int(restored[key]) * tb

source_video = json.loads((root / 'audio-pcm-source-video.json').read_text())
restored_video = json.loads((root / 'audio-pcm-restored-video.json').read_text())
tv0 = Fraction(source_video['streams'][0]['time_base'])
tv1 = Fraction(restored_video['streams'][0]['time_base'])
vf0, vf1 = source_video['frames'], restored_video['frames']
assert len(vf0) == len(vf1) == 12
assert [int(x['best_effort_timestamp']) * tv0 for x in vf0] == \
       [int(x['best_effort_timestamp']) * tv1 for x in vf1]
assert int(sp[0]['pts']) * ta == int(rp[0]['pts']) * tb
assert (int(sp[-1]['pts']) + int(sp[-1]['duration'])) * ta == \
       (int(rp[-1]['pts']) + int(rp[-1]['duration'])) * tb

aac_source = json.loads((root / 'audio-aac-source.json').read_text())
aac_restored = json.loads((root / 'audio-aac-restored.json').read_text())
as0, as1 = aac_source['streams'][0], aac_restored['streams'][0]
assert as0['codec_name'] == as1['codec_name'] == 'aac'
assert as0['sample_rate'] == as1['sample_rate'] == '48000'
at0, at1 = Fraction(as0['time_base']), Fraction(as1['time_base'])
ap0, ap1 = aac_source['packets'], aac_restored['packets']
assert len(ap0) == len(ap1) > 0
for source, restored in zip(ap0, ap1):
    assert source['data_hash'] == restored['data_hash']
    assert source['flags'] == restored['flags']
    for key in ('pts', 'dts', 'duration'):
        if key in source and key in restored:
            assert int(source[key]) * at0 == int(restored[key]) * at1
print('VFS5 preserved compressed audio packet hashes, flags, and exact A/V timing')
PY
fi

cp "$TMP/audio-pcm.vfs" "$TMP/audio-pcm-corrupt.vfs"
"$PYTHON" - "$TMP/audio-pcm-corrupt.vfs" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
data = bytearray(path.read_bytes())
data[-5] ^= 1
path.write_bytes(data)
PY
if "$BIN" unpack "$TMP/audio-pcm-corrupt.vfs" \
    --out "$TMP/audio-pcm-corrupt.nut" >/dev/null 2>&1; then
  echo 'unpack unexpectedly accepted a corrupt VFS5 audio extension' >&2
  exit 1
fi
test ! -e "$TMP/audio-pcm-corrupt.nut"
test ! -e "$TMP/audio-pcm-corrupt.tmp.nut"

"$BIN" pack "$TMP/color-metadata.mkv" --out "$TMP/color-metadata.vfs" \
  > "$TMP/color-metadata-pack-stats.json"
"$BIN" unpack "$TMP/color-metadata.vfs" --out "$TMP/color-metadata-restored.mkv" \
  > "$TMP/color-metadata-unpack-stats.json"
cmp "$TMP/color-metadata.mkv" "$TMP/color-metadata-restored.mkv"

"$BIN" pack "$TMP/display-metadata.mkv" --out "$TMP/display-metadata.vfs" \
  > "$TMP/display-metadata-pack-stats.json"
"$BIN" unpack "$TMP/display-metadata.vfs" --out "$TMP/display-metadata-restored.mkv" \
  > "$TMP/display-metadata-unpack-stats.json"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/display-metadata.mkv" \
  -f rawvideo -pix_fmt yuv420p -y "$TMP/display-metadata-source.raw"
"$FFMPEG" -hide_banner -loglevel error -i "$TMP/display-metadata-restored.mkv" \
  -f rawvideo -pix_fmt yuv420p -y "$TMP/display-metadata-restored.raw"
cmp "$TMP/display-metadata-source.raw" "$TMP/display-metadata-restored.raw"

if command -v ffprobe >/dev/null 2>&1; then
  ffprobe -v error -select_streams v:0 \
    -show_entries 'stream=time_base,sample_aspect_ratio,color_range,color_primaries,color_transfer,color_space,chroma_location,field_order:frame=best_effort_timestamp,interlaced_frame,top_field_first,repeat_pict' \
    -of json "$TMP/display-metadata.mkv" > "$TMP/display-metadata-source.json"
  ffprobe -v error -select_streams v:0 \
    -show_entries 'stream=time_base,sample_aspect_ratio,color_range,color_primaries,color_transfer,color_space,chroma_location,field_order:frame=best_effort_timestamp,interlaced_frame,top_field_first,repeat_pict' \
    -of json "$TMP/display-metadata-restored.mkv" > "$TMP/display-metadata-restored.json"
  "$PYTHON" - "$TMP" <<'PY'
from fractions import Fraction
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
source = json.loads((root / 'display-metadata-source.json').read_text())
restored = json.loads((root / 'display-metadata-restored.json').read_text())
source_stream = source['streams'][0]
restored_stream = restored['streams'][0]
display_fields = ('sample_aspect_ratio', 'color_range', 'color_primaries',
                  'color_transfer', 'color_space', 'chroma_location', 'field_order')
assert all(source_stream.get(key) == restored_stream.get(key) for key in display_fields)
assert source_stream['sample_aspect_ratio'] == '4:3'
assert source_stream['color_range'] == 'tv'
assert source_stream['color_primaries'] == 'bt709'
assert source_stream['color_transfer'] == 'bt709'
assert source_stream['color_space'] == 'bt709'
assert source_stream['chroma_location'] == 'left'
assert source_stream['field_order'] == 'tb'
source_tb = Fraction(source_stream['time_base'])
restored_tb = Fraction(restored_stream['time_base'])
source_frames = source['frames']
restored_frames = restored['frames']
assert len(source_frames) == len(restored_frames) == 25
frame_fields = ('interlaced_frame', 'top_field_first', 'repeat_pict')
assert [[frame.get(key) for key in frame_fields] for frame in source_frames] == \
       [[frame.get(key) for key in frame_fields] for frame in restored_frames]
source_times = [int(frame['best_effort_timestamp']) * source_tb for frame in source_frames]
restored_times = [int(frame['best_effort_timestamp']) * restored_tb for frame in restored_frames]
assert source_times == restored_times
print('VFS4 display metadata and exact frame timing survived Matroska unpack')
PY
fi

if "$BIN" unpack "$TMP/display-metadata.vfs" \
    --out "$TMP/display-metadata-lossy.nut" >/dev/null 2>&1; then
  echo 'unpack unexpectedly accepted NUT output that cannot preserve VFS4 color metadata' >&2
  exit 1
fi
test ! -e "$TMP/display-metadata-lossy.nut"
if "$BIN" unpack "$TMP/nv12.vfs" \
    --out "$TMP/nv12-nonexact.mkv" >/dev/null 2>&1; then
  echo 'unpack unexpectedly accepted Matroska output with non-millisecond VFS timestamps' >&2
  exit 1
fi
test ! -e "$TMP/nv12-nonexact.mkv"

if command -v ffprobe >/dev/null 2>&1; then
  ffprobe -v error -select_streams v:0 -show_entries stream=time_base \
    -show_entries frame=best_effort_timestamp -of json "$TMP/nv12.nut" \
    > "$TMP/source-timestamps.json"
  ffprobe -v error -select_streams v:0 -show_entries stream=time_base \
    -show_entries frame=best_effort_timestamp -of json "$TMP/nv12-restored.nut" \
    > "$TMP/restored-timestamps.json"
  "$PYTHON" - "$TMP" <<'PY'
from fractions import Fraction
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
source = json.loads((root / 'source-timestamps.json').read_text())
restored = json.loads((root / 'restored-timestamps.json').read_text())
source_tb = Fraction(source['streams'][0]['time_base'])
restored_tb = Fraction(restored['streams'][0]['time_base'])
source_pts = [int(frame['best_effort_timestamp']) for frame in source['frames']]
restored_pts = [int(frame['best_effort_timestamp']) for frame in restored['frames']]
assert len(source_pts) == len(restored_pts) == 12
assert [pts * source_tb for pts in source_pts] == [pts * restored_tb for pts in restored_pts]
print('whole-video archive timestamps passed: exact rational presentation times')
PY
fi

"$FFMPEG" -hide_banner -loglevel error -i "$TMP/restored.nut" \
  -map 0:v:0 -f null -
"$PYTHON" - "$TMP" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
packed = json.loads((root / 'pack-stats.json').read_text())
unpacked = json.loads((root / 'unpack-stats.json').read_text())
def assert_measured_selection(stats):
    native = stats['native_archive_bytes']
    source = stats['source_file_archive_bytes']
    assert source > 0
    if 'fast-path' in stats['representation_reason']:
        assert stats['representation_mode'] == 'source-file'
        assert native == 0
        assert stats['frame_count_exact'] is False
        assert stats['output_bytes'] == source
        return
    assert native > 0 and stats['frame_count_exact'] is True
    if stats['representation_mode'] == 'source-file':
        assert stats['output_bytes'] == source
    else:
        assert stats['representation_mode'] == 'native-planes'
        assert native <= source
        assert stats['output_bytes'] == native

assert_measured_selection(packed)
assert packed['frame_count'] == 24 and packed['width'] == 160 and packed['height'] == 90
assert packed['pixel_format'] == 'yuv420p'
assert packed['input_bytes'] > 0 and packed['output_bytes'] > 0
assert packed['elapsed_seconds'] > 0 and packed['frames_per_second'] > 0
if packed['representation_mode'] == 'source-file':
    assert unpacked['representation_mode'] == 'source-file'
    assert unpacked['frame_count'] == 0 and unpacked['pixel_format'] == ''
else:
    assert unpacked['representation_mode'] == 'native-planes'
    assert unpacked['frame_count'] == 24 and unpacked['pixel_format'] == 'yuv420p'
assert unpacked['output_bytes'] > 0
assert unpacked['elapsed_seconds'] > 0
if unpacked['representation_mode'] == 'native-planes':
    assert unpacked['frames_per_second'] > 0
else:
    assert unpacked['frame_count'] == 0 and unpacked['frames_per_second'] == 0
hd = json.loads((root / 'hd-pack-stats.json').read_text())
hd_unpacked = json.loads((root / 'hd-unpack-stats.json').read_text())
assert_measured_selection(hd)
assert hd['representation_mode'] == hd_unpacked['representation_mode'] == 'source-file'
assert hd['source_file_archive_bytes'] == hd['input_bytes'] + 28
assert hd_unpacked['frame_count'] == 0 and hd_unpacked['pixel_format'] == ''
high10 = json.loads((root / 'high10-pack-stats.json').read_text())
assert high10['pixel_format'] == 'yuv420p10le' and high10['frame_count'] == 12
assert_measured_selection(high10)
high10_unpacked = json.loads((root / 'high10-unpack-stats.json').read_text())
nv12 = json.loads((root / 'nv12-pack-stats.json').read_text())
nv12_unpacked = json.loads((root / 'nv12-unpack-stats.json').read_text())
assert_measured_selection(nv12)
assert high10['representation_mode'] == high10_unpacked['representation_mode'] == 'source-file'
assert high10_unpacked['frame_count'] == 0 and high10_unpacked['pixel_format'] == ''
assert nv12['representation_mode'] == nv12_unpacked['representation_mode'] == 'native-planes'
assert nv12['pixel_format'] == nv12_unpacked['pixel_format'] == 'nv12'
with_audio = json.loads((root / 'with-audio-pack-stats.json').read_text())
assert with_audio['representation_mode'] == 'source-file'
assert with_audio['representation_reason'] == 'audio-remux-unsupported-fast-path'
assert with_audio['output_bytes'] == with_audio['source_file_archive_bytes']
audio_video_only = json.loads((root / 'audio-video-only-pack-stats.json').read_text())
audio_pcm = json.loads((root / 'audio-pcm-pack-stats.json').read_text())
audio_pcm_unpacked = json.loads((root / 'audio-pcm-unpack-stats.json').read_text())
audio_aac = json.loads((root / 'audio-aac-pack-stats.json').read_text())
audio_aac_unpacked = json.loads((root / 'audio-aac-unpack-stats.json').read_text())
assert_measured_selection(audio_video_only)
assert_measured_selection(audio_pcm)
assert_measured_selection(audio_aac)
assert audio_video_only['representation_mode'] == 'native-planes'
assert audio_pcm['representation_mode'] == audio_pcm_unpacked['representation_mode'] == 'native-planes'
assert audio_aac['representation_mode'] == audio_aac_unpacked['representation_mode'] == 'native-planes'
assert audio_pcm['frame_count'] == audio_pcm_unpacked['frame_count'] == 12
assert audio_aac['frame_count'] == audio_aac_unpacked['frame_count'] == 12
assert audio_pcm['native_archive_bytes'] > audio_video_only['native_archive_bytes']
assert audio_pcm['native_archive_bytes'] < audio_pcm['source_file_archive_bytes']
assert audio_pcm['output_bytes'] == audio_pcm['native_archive_bytes']
assert (root / 'audio-pcm.vfs').read_bytes()[8:10] == b'\x05\x00'
assert audio_aac['native_archive_bytes'] < audio_aac['source_file_archive_bytes']
assert (root / 'audio-aac.vfs').read_bytes()[8:10] == b'\x05\x00'
color_metadata = json.loads((root / 'color-metadata-pack-stats.json').read_text())
assert color_metadata['representation_mode'] == 'source-file'
assert color_metadata['representation_reason'] == 'source-content-safety'
assert color_metadata['native_archive_bytes'] > 0
assert color_metadata['native_archive_bytes'] < color_metadata['source_file_archive_bytes']
assert color_metadata['output_bytes'] == color_metadata['source_file_archive_bytes']
display_metadata = json.loads((root / 'display-metadata-pack-stats.json').read_text())
assert display_metadata['representation_mode'] == 'native-planes'
assert display_metadata['frame_count'] == 25
assert display_metadata['native_archive_bytes'] < display_metadata['source_file_archive_bytes']
assert display_metadata['output_bytes'] == display_metadata['native_archive_bytes']
assert (root / 'display-metadata.vfs').read_bytes()[8:10] == b'\x04\x00'
print('archive mode checks passed: measured size selection, exact source bytes, and exact native planes')
PY

if "$BIN" unpack "$TMP/nv12.vfs" --out "$TMP/nv12-restored.mp4" >/dev/null 2>&1; then
  echo 'unpack unexpectedly accepted a non-NUT output extension' >&2
  exit 1
fi

cp "$TMP/hd.vfs" "$TMP/corrupt-source.vfs"
"$PYTHON" - "$TMP/corrupt-source.vfs" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
data = bytearray(path.read_bytes())
data[24] ^= 1
path.write_bytes(data)
PY
if "$BIN" unpack "$TMP/corrupt-source.vfs" --out "$TMP/corrupt-restored.mp4" >/dev/null 2>&1; then
  echo 'unpack unexpectedly accepted a source-file payload checksum mismatch' >&2
  exit 1
fi
test ! -e "$TMP/corrupt-restored.mp4"
test ! -e "$TMP/corrupt-restored.tmp.mp4"

if "$BIN" pack "$TMP/short-low.mp4" --out "$TMP/short.vfs" >/dev/null 2>&1; then
  echo 'pack unexpectedly overwrote an existing archive' >&2
  exit 1
fi
