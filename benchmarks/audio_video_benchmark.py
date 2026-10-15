#!/usr/bin/env python3
"""Reproducible Release benchmark for VFS5 on audio-bearing video sources.

Generates deterministic raw-video NUT sources across multiple resolutions and
content patterns with PCM or AAC audio, plus a metadata-unsafe Matroska fixture.
Measures end-to-end pack/unpack wall time and both candidate/selected sizes,
then validates packet hashes, audio/video timestamps, and exact source fallback.
"""

from __future__ import annotations

import argparse
import csv
import filecmp
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from fractions import Fraction
from pathlib import Path
from typing import Any, Sequence


FPS = 24
CONTENT_SOURCES = {
    "static": lambda width, height: f"color=c=0x315a83:s={width}x{height}:r={FPS}",
    "moving_detail": lambda width, height: f"testsrc2=size={width}x{height}:rate={FPS}",
    "temporal_noise": lambda width, height: (
        f"nullsrc=size={width}x{height}:rate={FPS},format=yuv420p,"
        "noise=alls=100:allf=t+u:all_seed=54711"
    ),
}
AUDIO_CODECS = ("pcm_s16le", "aac")
SOURCE_ARCHIVE_MAGIC = b"VFSRAW\r\n"
CSV_FIELDS = [
    "case_id", "content", "width", "height", "frames", "audio_codec",
    "source_bytes", "archive_bytes", "archive_over_source",
    "native_candidate_bytes", "source_file_candidate_bytes",
    "representation_mode", "representation_reason", "native_candidate_larger",
    "pack_seconds_median", "pack_frames_per_second_median",
    "pack_source_mib_per_second_median", "unpack_seconds_median",
    "unpack_frames_per_second_median", "unpack_source_mib_per_second_median",
    "audio_packet_count", "audio_packet_hashes_exact", "audio_timestamps_exact",
    "video_timestamps_exact", "av_sync_exact", "av_start_offset_seconds",
    "av_end_offset_seconds", "source_sha256", "archive_sha256",
    "source_fallback_exact",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frameforge", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--ffprobe", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--frames", type=int, default=48)
    parser.add_argument("--sizes", default="160x90,320x180,640x360")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--compression", type=int, default=1)
    return parser.parse_args()


def parse_sizes(specification: str) -> list[tuple[int, int]]:
    sizes: list[tuple[int, int]] = []
    for item in specification.split(","):
        match = re.fullmatch(r"\s*(\d+)x(\d+)\s*", item)
        if match is None:
            raise ValueError(f"invalid size {item!r}; expected WIDTHxHEIGHT")
        width, height = int(match.group(1)), int(match.group(2))
        if width < 2 or height < 2 or width % 2 or height % 2:
            raise ValueError(f"size {item!r} must have positive even dimensions")
        sizes.append((width, height))
    if not sizes or len(set(sizes)) != len(sizes):
        raise ValueError("provide at least one unique even video size")
    return sizes


def run_checked(command: Sequence[str], description: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, check=False, text=True, capture_output=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic output"
        raise RuntimeError(f"{description} failed with exit code {result.returncode}:\n{detail}")
    return result


def timed_command(
    command: Sequence[str], description: str
) -> tuple[subprocess.CompletedProcess[str], float]:
    start = time.perf_counter()
    result = run_checked(command, description)
    return result, max(time.perf_counter() - start, 1.0e-9)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def generate_source(
    ffmpeg: Path, source: Path, *, content: str, width: int, height: int,
    frames: int, audio_codec: str,
) -> None:
    duration = frames / FPS
    audio = f"sine=frequency=440:sample_rate=48000:duration={duration:.9f}"
    command = [
        str(ffmpeg), "-hide_banner", "-loglevel", "error", "-filter_threads", "1",
        "-f", "lavfi", "-i", CONTENT_SOURCES[content](width, height),
        "-f", "lavfi", "-i", audio,
        "-map", "0:v:0", "-map", "1:a:0", "-frames:v", str(frames),
        "-t", f"{duration:.9f}", "-c:v", "rawvideo", "-pix_fmt", "yuv420p",
        "-threads", "1", "-c:a", audio_codec,
    ]
    if audio_codec == "aac":
        command += ["-b:a", "128k"]
    command += ["-f", "nut", "-y", str(source)]
    run_checked(command, f"generating {content} {width}x{height} with {audio_codec}")
    if not source.is_file() or source.stat().st_size == 0:
        raise RuntimeError(f"FFmpeg generated an empty source: {source}")


def generate_unsafe_source(ffmpeg: Path, source: Path, *, width: int, height: int,
                           frames: int) -> None:
    duration = frames / FPS
    command = [
        str(ffmpeg), "-hide_banner", "-loglevel", "error", "-filter_threads", "1",
        "-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate={FPS}",
        "-f", "lavfi", "-i",
        f"sine=frequency=880:sample_rate=48000:duration={duration:.9f}",
        "-map", "0:v:0", "-map", "1:a:0", "-frames:v", str(frames),
        "-t", f"{duration:.9f}", "-c:v", "ffv1", "-level", "3",
        "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le", "-threads", "1",
        "-color_range", "tv", "-color_primaries", "bt709", "-color_trc", "bt709",
        "-colorspace", "bt709", "-f", "matroska", "-y", str(source),
    ]
    run_checked(command, "generating metadata-unsafe audio-bearing fixture")
    if not source.is_file() or source.stat().st_size == 0:
        raise RuntimeError("FFmpeg generated an empty metadata-unsafe fixture")


def require_unsafe_color_metadata(ffprobe: Path, source: Path) -> None:
    probe = probe_json(
        ffprobe, source, "v:0", "streams",
        "stream=color_range,color_primaries,color_transfer,color_space",
        "checking unsafe fixture color metadata",
    )
    stream = one_stream(probe, "video")
    if not any(stream.get(key) not in (None, "unknown", "unspecified") for key in
               ("color_range", "color_primaries", "color_transfer", "color_space")):
        raise RuntimeError("unsafe fixture did not retain its explicit color metadata")


def probe_json(ffprobe: Path, source: Path, stream_selector: str,
               show: str, entries: str, description: str) -> dict[str, Any]:
    result = run_checked(
        [str(ffprobe), "-v", "error", "-select_streams", stream_selector,
         "-show_data_hash", "sha256", f"-show_{show}", "-show_entries", entries,
         "-of", "json", str(source)],
        description,
    )
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{description} returned invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{description} did not return a JSON object")
    return value


def ffprobe_audio(ffprobe: Path, source: Path) -> dict[str, Any]:
    return probe_json(
        ffprobe, source, "a:0", "packets",
        "stream=codec_name,time_base,sample_rate,channels,channel_layout:"
        "packet=pts,dts,duration,flags,data_hash",
        f"probing audio packets in {source.name}",
    ) | probe_json(
        ffprobe, source, "a:0", "streams",
        "stream=codec_name,time_base,sample_rate,channels,channel_layout",
        f"probing audio stream in {source.name}",
    )


def ffprobe_video(ffprobe: Path, source: Path) -> dict[str, Any]:
    return probe_json(
        ffprobe, source, "v:0", "frames",
        "stream=codec_name,time_base,width,height,pix_fmt:"
        "frame=best_effort_timestamp,pkt_duration",
        f"probing video frames in {source.name}",
    ) | probe_json(
        ffprobe, source, "v:0", "streams",
        "stream=codec_name,time_base,width,height,pix_fmt",
        f"probing video stream in {source.name}",
    )


def one_stream(probe: dict[str, Any], label: str) -> dict[str, Any]:
    streams = probe.get("streams")
    if not isinstance(streams, list) or len(streams) != 1 or not isinstance(streams[0], dict):
        raise RuntimeError(f"expected exactly one {label} stream")
    return streams[0]


def exact_int(value: Any, label: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as error:
        raise RuntimeError(f"missing or invalid {label}: {value!r}") from error


def audio_signature(probe: dict[str, Any]) -> tuple[dict[str, Any], list[tuple[Any, ...]]]:
    stream = one_stream(probe, "audio")
    time_base = Fraction(stream["time_base"])
    params = {key: stream.get(key) for key in
              ("codec_name", "sample_rate", "channels", "channel_layout")}
    packets = probe.get("packets")
    if not isinstance(packets, list) or not packets:
        raise RuntimeError("audio-bearing benchmark input contains no probed packets")
    signature: list[tuple[Any, ...]] = []
    for packet in packets:
        if not isinstance(packet, dict) or not packet.get("data_hash"):
            raise RuntimeError("ffprobe did not provide an audio packet SHA-256 hash")
        timeline: list[Fraction | None] = []
        for key in ("pts", "dts", "duration"):
            raw = packet.get(key)
            timeline.append(None if raw is None else Fraction(exact_int(raw, f"audio {key}")) * time_base)
        signature.append((packet["data_hash"], packet.get("flags"), *timeline))
    return params, signature


def video_signature(probe: dict[str, Any]) -> tuple[dict[str, Any], list[Fraction], Fraction]:
    stream = one_stream(probe, "video")
    time_base = Fraction(stream["time_base"])
    params = {key: stream.get(key) for key in ("width", "height", "pix_fmt")}
    frames = probe.get("frames")
    if not isinstance(frames, list) or not frames:
        raise RuntimeError("audio-bearing benchmark input contains no video frames")
    timestamps: list[Fraction] = []
    durations: list[Fraction | None] = []
    for frame in frames:
        if not isinstance(frame, dict):
            raise RuntimeError("ffprobe returned an invalid video frame record")
        timestamp = frame.get("best_effort_timestamp")
        if timestamp is None:
            raise RuntimeError("ffprobe omitted a best-effort timestamp for a video frame")
        timestamps.append(Fraction(exact_int(timestamp, "video timestamp")) * time_base)
        raw_duration = frame.get("pkt_duration")
        durations.append(None if raw_duration is None else
                         Fraction(exact_int(raw_duration, "video frame duration")) * time_base)
    positive_intervals = [right - left for left, right in zip(timestamps, timestamps[1:])
                          if right > left]
    if positive_intervals:
        frame_interval = statistics.median(positive_intervals)
    elif durations[-1] is not None and durations[-1] > 0:
        frame_interval = durations[-1]
    else:
        raise RuntimeError("cannot infer a positive video frame interval")
    final_duration = durations[-1] if durations[-1] is not None and durations[-1] > 0 \
        else frame_interval
    return params, timestamps, final_duration


def av_sync_offsets(audio_packets: list[tuple[Any, ...]],
                    video_timestamps: list[Fraction],
                    final_video_duration: Fraction) -> tuple[Fraction, Fraction, Fraction]:
    first_audio_pts = next((packet[2] for packet in audio_packets if packet[2] is not None), None)
    if first_audio_pts is None:
        first_audio_pts = next((packet[3] for packet in audio_packets if packet[3] is not None), None)
    if first_audio_pts is None:
        raise RuntimeError("audio packets have neither PTS nor DTS for synchronization check")
    last_audio_end: Fraction | None = None
    for packet in reversed(audio_packets):
        pts, dts, duration = packet[2], packet[3], packet[4]
        start = pts if pts is not None else dts
        if start is not None and duration is not None:
            last_audio_end = start + duration
            break
    if last_audio_end is None:
        raise RuntimeError("cannot determine the audio packet end time")
    video_start = video_timestamps[0]
    video_end = video_timestamps[-1] + final_video_duration
    first_audio_duration = next((packet[4] for packet in audio_packets
                                 if packet[4] is not None and packet[4] > 0), None)
    last_audio_duration = next((packet[4] for packet in reversed(audio_packets)
                                if packet[4] is not None and packet[4] > 0), None)
    frame_interval = (video_timestamps[1] - video_timestamps[0]
                      if len(video_timestamps) > 1 else final_video_duration)
    tolerance = max(frame_interval, first_audio_duration or Fraction(0),
                    last_audio_duration or Fraction(0))
    start_offset, end_offset = first_audio_pts - video_start, last_audio_end - video_end
    if abs(start_offset) > tolerance or abs(end_offset) > tolerance:
        raise RuntimeError(
            "generated source is not A/V synchronized within one frame/packet: "
            f"start offset {float(start_offset):.9f}s, end offset {float(end_offset):.9f}s, "
            f"tolerance {float(tolerance):.9f}s"
        )
    return start_offset, end_offset, tolerance


def inspect_media(ffprobe: Path, source: Path) -> dict[str, Any]:
    audio = ffprobe_audio(ffprobe, source)
    video = ffprobe_video(ffprobe, source)
    audio_params, audio_packets = audio_signature(audio)
    video_params, video_timestamps, video_duration = video_signature(video)
    start_offset, end_offset, tolerance = av_sync_offsets(
        audio_packets, video_timestamps, video_duration
    )
    return {
        "audio_params": audio_params,
        "audio_packets": audio_packets,
        "video_params": video_params,
        "video_timestamps": video_timestamps,
        "av_start_offset": start_offset,
        "av_end_offset": end_offset,
        "av_tolerance": tolerance,
    }


def verify_round_trip(ffprobe: Path, source: Path, restored: Path,
                      expected_codec: str, frames: int) -> dict[str, Any]:
    original = inspect_media(ffprobe, source)
    round_trip = inspect_media(ffprobe, restored)
    if original["audio_params"] != round_trip["audio_params"]:
        raise RuntimeError("audio codec parameters changed during VFS5/fallback round trip")
    if original["audio_params"].get("codec_name") != expected_codec:
        raise RuntimeError(
            f"expected {expected_codec} audio but ffprobe reports "
            f"{original['audio_params'].get('codec_name')!r}"
        )
    original_audio = original["audio_packets"]
    restored_audio = round_trip["audio_packets"]
    if len(original_audio) != len(restored_audio) or original_audio != restored_audio:
        raise RuntimeError("audio packet hashes, flags, or exact packet timestamps changed")
    if original["video_params"] != round_trip["video_params"]:
        raise RuntimeError("video dimensions, pixel format, or stream parameters changed")
    if len(original["video_timestamps"]) != frames or \
            original["video_timestamps"] != round_trip["video_timestamps"]:
        raise RuntimeError("video frame timestamps changed or decoded frame count is wrong")
    original_offsets = (original["av_start_offset"], original["av_end_offset"])
    restored_offsets = (round_trip["av_start_offset"], round_trip["av_end_offset"])
    if original_offsets != restored_offsets:
        raise RuntimeError("audio/video relative timestamp synchronization changed")
    return {
        "audio_packet_count": len(original_audio),
        "audio_packet_hashes_exact": True,
        "audio_timestamps_exact": True,
        "video_timestamps_exact": True,
        "av_sync_exact": True,
        "av_start_offset_seconds": float(original["av_start_offset"]),
        "av_end_offset_seconds": float(original["av_end_offset"]),
    }


def run_case(
    *, ffmpeg: Path, ffprobe: Path, frameforge: Path, work_dir: Path,
    case_id: str, content: str, width: int, height: int, frames: int,
    audio_codec: str, repeats: int, compression: int, unsafe: bool = False,
) -> dict[str, Any]:
    case_dir = work_dir / case_id
    case_dir.mkdir(parents=True)
    extension = ".mkv" if unsafe else ".nut"
    source = case_dir / f"source{extension}"
    if unsafe:
        generate_unsafe_source(ffmpeg, source, width=width, height=height, frames=frames)
        require_unsafe_color_metadata(ffprobe, source)
        expected_codec = "pcm_s16le"
    else:
        generate_source(ffmpeg, source, content=content, width=width, height=height,
                        frames=frames, audio_codec=audio_codec)
        expected_codec = audio_codec

    source_size = source.stat().st_size
    source_hash = sha256_file(source)
    pack_times: list[float] = []
    unpack_times: list[float] = []
    archives: set[tuple[int, str, str, str, int, int]] = set()
    archive_hashes: set[str] = set()
    round_trip_metrics: dict[str, Any] | None = None
    source_fallback_exact = False

    for repetition in range(repeats):
        archive = case_dir / f"archive-{repetition}.vfs"
        restored = case_dir / f"restored-{repetition}{extension}"
        pack_result, pack_elapsed = timed_command(
            [str(frameforge), "pack", str(source), "--out", str(archive),
             "--threads", "1", "--compression", str(compression)],
            f"packing {case_id} repetition {repetition + 1}",
        )
        try:
            stats = json.loads(pack_result.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"pack returned invalid JSON for {case_id}: {error}") from error
        mode = stats.get("representation_mode")
        reason = stats.get("representation_reason")
        native_bytes = stats.get("native_archive_bytes")
        source_candidate_bytes = stats.get("source_file_archive_bytes")
        output_bytes = stats.get("output_bytes")
        if mode not in {"native-planes", "source-file"} or not isinstance(reason, str):
            raise RuntimeError(f"pack omitted the representation decision for {case_id}")
        if not all(isinstance(value, int) and value >= 0 for value in
                   (native_bytes, source_candidate_bytes, output_bytes)):
            raise RuntimeError(f"pack omitted candidate sizes for {case_id}")
        if source_candidate_bytes <= 0 or output_bytes != archive.stat().st_size:
            raise RuntimeError(f"pack candidate/output sizes are inconsistent for {case_id}")
        if mode == "native-planes":
            if native_bytes <= 0 or output_bytes != native_bytes or native_bytes > source_candidate_bytes:
                raise RuntimeError(f"native VFS5 candidate selection is inconsistent for {case_id}")
            with archive.open("rb") as packed:
                prefix = packed.read(10)
            if prefix[8:10] != b"\x05\x00":
                raise RuntimeError(f"native audio-bearing archive is not VFS5 for {case_id}")
        else:
            if output_bytes != source_candidate_bytes:
                raise RuntimeError(f"source-file candidate was not selected for {case_id}")
            with archive.open("rb") as packed:
                if packed.read(len(SOURCE_ARCHIVE_MAGIC)) != SOURCE_ARCHIVE_MAGIC:
                    raise RuntimeError(f"source-file fallback archive has an invalid signature: {case_id}")
            if reason == "smaller-source-file" and not native_bytes > source_candidate_bytes:
                raise RuntimeError(f"size-based source fallback did not have a larger native candidate: {case_id}")

        unpack_result, unpack_elapsed = timed_command(
            [str(frameforge), "unpack", str(archive), "--out", str(restored)],
            f"unpacking {case_id} repetition {repetition + 1}",
        )
        try:
            unpack_stats = json.loads(unpack_result.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"unpack returned invalid JSON for {case_id}: {error}") from error
        if mode == "source-file":
            if not filecmp.cmp(source, restored, shallow=False):
                raise RuntimeError(f"source-file fallback did not restore exact bytes for {case_id}")
            if unpack_stats.get("frame_count") != 0:
                raise RuntimeError(f"source-file restore unexpectedly decoded frames for {case_id}")
            source_fallback_exact = True
        elif unpack_stats.get("frame_count") != frames:
            raise RuntimeError(f"VFS5 unpack frame count differs for {case_id}")

        round_trip_metrics = verify_round_trip(
            ffprobe, source, restored, expected_codec, frames
        )
        pack_times.append(pack_elapsed)
        unpack_times.append(unpack_elapsed)
        archive_hash = sha256_file(archive)
        archive_hashes.add(archive_hash)
        archives.add((output_bytes, mode, reason, archive_hash, native_bytes,
                      source_candidate_bytes))
        archive.unlink()
        restored.unlink()

    if len(archives) != 1 or len(archive_hashes) != 1:
        raise RuntimeError(f"archive bytes or candidate selection varied across repetitions: {case_id}")
    if round_trip_metrics is None:
        raise RuntimeError(f"no round-trip measurements were collected for {case_id}")
    output_bytes, mode, reason, archive_hash, native_bytes, source_candidate_bytes = next(iter(archives))
    pack_median = statistics.median(pack_times)
    unpack_median = statistics.median(unpack_times)
    return {
        "case_id": case_id,
        "content": content,
        "width": width,
        "height": height,
        "frames": frames,
        "audio_codec": expected_codec,
        "source_bytes": source_size,
        "archive_bytes": output_bytes,
        "archive_over_source": output_bytes / source_size,
        "native_candidate_bytes": native_bytes,
        "source_file_candidate_bytes": source_candidate_bytes,
        "representation_mode": mode,
        "representation_reason": reason,
        "native_candidate_larger": native_bytes > source_candidate_bytes,
        "pack_seconds_median": pack_median,
        "pack_frames_per_second_median": frames / pack_median,
        "pack_source_mib_per_second_median": source_size / pack_median / (1024.0 * 1024.0),
        "unpack_seconds_median": unpack_median,
        "unpack_frames_per_second_median": frames / unpack_median,
        "unpack_source_mib_per_second_median": source_size / unpack_median / (1024.0 * 1024.0),
        **round_trip_metrics,
        "source_sha256": source_hash,
        "archive_sha256": archive_hash,
        "source_fallback_exact": source_fallback_exact,
    }


def write_results(output_dir: Path, rows: list[dict[str, Any]], metadata: dict[str, Any]) -> None:
    csv_path = output_dir / "audio-video-results.csv"
    temporary_csv = csv_path.with_suffix(".csv.tmp")
    with temporary_csv.open("w", newline="", encoding="utf-8") as destination:
        writer = csv.DictWriter(destination, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    temporary_csv.replace(csv_path)
    json_path = output_dir / "audio-video-run.json"
    temporary_json = json_path.with_suffix(".json.tmp")
    temporary_json.write_text(json.dumps({**metadata, "results": rows}, indent=2,
                                         sort_keys=True) + "\n", encoding="utf-8")
    temporary_json.replace(json_path)


def main() -> int:
    args = parse_args()
    try:
        if args.build_type.strip().casefold() != "release":
            raise ValueError(
                "this benchmark requires a Release build; configure with "
                "-DCMAKE_BUILD_TYPE=Release or select Release in a multi-config generator"
            )
        if args.frames < 2:
            raise ValueError("--frames must be at least 2")
        if args.repeats < 1:
            raise ValueError("--repeats must be at least 1")
        if not 0 <= args.compression <= 9:
            raise ValueError("--compression must be in the range 0..9")
        sizes = parse_sizes(args.sizes)
        frameforge = args.frameforge.resolve(strict=True)
        ffmpeg = args.ffmpeg.resolve(strict=True)
        ffprobe = args.ffprobe.resolve(strict=True)
        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        for executable in (frameforge, ffmpeg, ffprobe):
            if not os.access(executable, os.X_OK):
                raise ValueError(f"not an executable file: {executable}")

        ffmpeg_version = run_checked([str(ffmpeg), "-version"], "reading FFmpeg version")
        ffprobe_version = run_checked([str(ffprobe), "-version"], "reading ffprobe version")
        rows: list[dict[str, Any]] = []
        total = len(CONTENT_SOURCES) * len(sizes) * len(AUDIO_CODECS) + 2
        with tempfile.TemporaryDirectory(prefix="frameforge-audio-benchmark-",
                                          dir=output_dir) as temporary:
            work_dir = Path(temporary)
            completed = 0
            for content in CONTENT_SOURCES:
                for width, height in sizes:
                    for audio_codec in AUDIO_CODECS:
                        case_id = f"{content}-{width}x{height}-{audio_codec}"
                        row = run_case(
                            ffmpeg=ffmpeg, ffprobe=ffprobe, frameforge=frameforge,
                            work_dir=work_dir, case_id=case_id, content=content,
                            width=width, height=height, frames=args.frames,
                            audio_codec=audio_codec, repeats=args.repeats,
                            compression=args.compression,
                        )
                        rows.append(row)
                        completed += 1
                        print(
                            f"[{completed}/{total}] {case_id:42s} "
                            f"source={row['source_bytes']} archive={row['archive_bytes']} "
                            f"native={row['native_candidate_bytes']} "
                            f"mode={row['representation_mode']}:{row['representation_reason']} "
                            f"pack={row['pack_source_mib_per_second_median']:.2f} MiB/s "
                            f"unpack={row['unpack_source_mib_per_second_median']:.2f} MiB/s "
                            "audio+sync=PASS",
                            flush=True,
                        )
            unsafe_width, unsafe_height = sizes[min(1, len(sizes) - 1)]
            unsafe_row = run_case(
                ffmpeg=ffmpeg, ffprobe=ffprobe, frameforge=frameforge,
                work_dir=work_dir, case_id=f"unsafe-color-metadata-{unsafe_width}x{unsafe_height}",
                content="metadata-unsafe", width=unsafe_width, height=unsafe_height,
                frames=args.frames, audio_codec="pcm_s16le", repeats=args.repeats,
                compression=args.compression, unsafe=True,
            )
            rows.append(unsafe_row)
            completed += 1
            print(
                f"[{completed}/{total}] {unsafe_row['case_id']:42s} "
                f"source={unsafe_row['source_bytes']} archive={unsafe_row['archive_bytes']} "
                f"native={unsafe_row['native_candidate_bytes']} "
                f"mode={unsafe_row['representation_mode']}:{unsafe_row['representation_reason']} "
                "source-exact=PASS audio+sync=PASS",
                flush=True,
            )

            larger_row = run_case(
                ffmpeg=ffmpeg, ffprobe=ffprobe, frameforge=frameforge,
                work_dir=work_dir, case_id="larger-native-candidate-2x2-pcm_s16le",
                content="static", width=2, height=2, frames=args.frames,
                audio_codec="pcm_s16le", repeats=args.repeats,
                compression=args.compression,
            )
            rows.append(larger_row)
            completed += 1
            print(
                f"[{completed}/{total}] {larger_row['case_id']:42s} "
                f"source={larger_row['source_bytes']} archive={larger_row['archive_bytes']} "
                f"native={larger_row['native_candidate_bytes']} "
                f"mode={larger_row['representation_mode']}:{larger_row['representation_reason']} "
                "larger-candidate/exact-source=PASS audio+sync=PASS",
                flush=True,
            )

        larger_fallbacks = [row for row in rows
                            if row["representation_reason"] == "smaller-source-file"
                            and row["native_candidate_larger"]]
        unsafe_fallbacks = [row for row in rows
                            if row["representation_mode"] == "source-file"
                            and (row["representation_reason"].startswith("source-content-safety")
                                 or row["representation_reason"] ==
                                    "audio-remux-unsupported-fast-path")
                            and row["source_fallback_exact"]]
        if not larger_fallbacks:
            raise RuntimeError(
                "benchmark did not produce a case where the measured VFS5 candidate "
                "was larger than the source-file candidate and exact-source fallback was selected"
            )
        if not unsafe_fallbacks:
            raise RuntimeError(
                "benchmark did not prove exact-source fallback for unsafe video metadata"
            )
        if not (larger_row["representation_mode"] == "source-file"
                and larger_row["representation_reason"] == "smaller-source-file"
                and larger_row["native_candidate_larger"]
                and larger_row["source_fallback_exact"]):
            raise RuntimeError("the dedicated 2x2 fixture did not prove size-based exact-source fallback")

        metadata: dict[str, Any] = {
            "schema_version": 1,
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            "build_type": args.build_type,
            "frameforge_executable": str(frameforge),
            "ffmpeg_executable": str(ffmpeg),
            "ffprobe_executable": str(ffprobe),
            "ffmpeg_version": ffmpeg_version.stdout.splitlines()[0],
            "ffprobe_version": ffprobe_version.stdout.splitlines()[0],
            "python_version": sys.version.split()[0],
            "host_platform": platform.platform(),
            "configuration": {
                "frames_per_source": args.frames,
                "frame_rate": FPS,
                "sizes": [{"width": width, "height": height} for width, height in sizes],
                "content_types": list(CONTENT_SOURCES),
                "audio_codecs": list(AUDIO_CODECS),
                "repeats": args.repeats,
                "compression_level": args.compression,
                "video_source_codec": "rawvideo/yuv420p in NUT",
                "audio_source": "440 Hz sine, 48 kHz, mono; AAC at 128 kbit/s",
                "pack_threads": 1,
                "timing_scope": "end-to-end process wall time, including startup and file I/O; median of repetitions",
                "throughput_basis": "source file bytes",
                "integrity_checks": [
                    "SHA-256 per audio packet, flags and exact rational packet PTS/DTS/duration",
                    "exact decoded video-frame timestamp sequence",
                    "A/V start and end offset equality, and source alignment within one frame/packet",
                    "byte-for-byte exact restore for every selected source-file fallback",
                    "deterministic selected archive and representation across repetitions",
                ],
            },
            "fallback_assertions": {
                "larger_native_candidate_cases": [row["case_id"] for row in larger_fallbacks],
                "unsafe_exact_source_cases": [row["case_id"] for row in unsafe_fallbacks],
            },
        }
        write_results(output_dir, rows, metadata)
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"audio/video benchmark failed: {error}", file=sys.stderr)
        return 1

    print(f"Results: {output_dir / 'audio-video-results.csv'}")
    print(f"Run metadata: {output_dir / 'audio-video-run.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
