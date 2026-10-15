#!/usr/bin/env python3
"""Reproducible end-to-end benchmark for FrameForge's lossless video archive.

Run through the CMake frameforge_benchmark target in a Release configuration.
The generated sources are fixed FFmpeg lavfi patterns encoded to H.264 with
single-threaded, explicitly configured settings. Measurements include process
startup and file I/O; each case reports the median of repeated pack/unpack runs.
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
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


CONTENT_SOURCES = {
    "static": lambda width, height: (
        f"color=c=0x315a83:s={width}x{height}:r=24"
    ),
    "moving_detail": lambda width, height: (
        f"testsrc2=size={width}x{height}:rate=24"
    ),
    "temporal_noise": lambda width, height: (
        f"nullsrc=size={width}x{height}:rate=24,format=yuv420p,"
        "noise=alls=100:allf=t+u:all_seed=54711"
    ),
}

CSV_FIELDS = [
    "content",
    "width",
    "height",
    "frames",
    "pixel_format",
    "source_bytes",
    "archive_bytes",
    "archive_over_source",
    "archive_savings_percent",
    "decoded_bytes",
    "pack_seconds_median",
    "pack_fps_median",
    "pack_mib_per_second_median",
    "unpack_seconds_median",
    "unpack_fps_median",
    "unpack_mib_per_second_median",
    "source_sha256",
    "archive_sha256",
    "exact_match",
    "representation_mode",
    "representation_reason",
    "native_archive_bytes",
    "source_file_archive_bytes",
    "source_to_decoded_plane_ratio",
    "frame_count_exact",
]

SOURCE_DENSITY_FAST_PATH_THRESHOLD = 0.006


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frameforge", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--frames", type=int, default=48)
    parser.add_argument("--sizes", default="320x180,640x360,1280x720")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--compression", type=int, default=1)
    return parser.parse_args()


def parse_sizes(specification: str) -> list[tuple[int, int]]:
    sizes: list[tuple[int, int]] = []
    for item in specification.split(","):
        match = re.fullmatch(r"\s*(\d+)x(\d+)\s*", item)
        if match is None:
            raise ValueError(f"invalid size {item!r}; expected WIDTHxHEIGHT")
        width, height = (int(match.group(1)), int(match.group(2)))
        if width < 2 or height < 2 or width % 2 or height % 2:
            raise ValueError(f"size {item!r} must have positive even dimensions")
        sizes.append((width, height))
    if not sizes:
        raise ValueError("at least one benchmark size is required")
    return sizes


def run_checked(command: Sequence[str], description: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, check=False, text=True, capture_output=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic output"
        raise RuntimeError(
            f"{description} failed with exit code {result.returncode}:\n{detail}"
        )
    return result


def timed_command(
    command: Sequence[str], description: str
) -> tuple[subprocess.CompletedProcess[str], float]:
    start = time.perf_counter()
    result = run_checked(command, description)
    elapsed = max(time.perf_counter() - start, 1.0e-9)
    return result, elapsed


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def decode_raw(ffmpeg: Path, source: Path, target: Path, pixel_format: str) -> None:
    run_checked(
        [
            str(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(source),
            "-map",
            "0:v:0",
            "-fps_mode",
            "passthrough",
            "-pix_fmt",
            pixel_format,
            "-f",
            "rawvideo",
            "-y",
            str(target),
        ],
        f"decoding {source.name} to raw {pixel_format}",
    )


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def write_results(output_dir: Path, rows: list[dict[str, Any]], metadata: dict[str, Any]) -> None:
    csv_path = output_dir / "results.csv"
    csv_temp = csv_path.with_suffix(".csv.tmp")
    with csv_temp.open("w", newline="", encoding="utf-8") as destination:
        writer = csv.DictWriter(destination, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    csv_temp.replace(csv_path)
    atomic_write_json(output_dir / "run.json", {**metadata, "results": rows})


def benchmark_case(
    *,
    ffmpeg: Path,
    frameforge: Path,
    work_dir: Path,
    content: str,
    width: int,
    height: int,
    frames: int,
    repeats: int,
    compression: int,
) -> dict[str, Any]:
    case_dir = work_dir / f"{content}-{width}x{height}"
    case_dir.mkdir(parents=True)
    source = case_dir / "source.mp4"
    source_graph = CONTENT_SOURCES[content](width, height)
    run_checked(
        [
            str(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-filter_threads",
            "1",
            "-f",
            "lavfi",
            "-i",
            source_graph,
            "-frames:v",
            str(frames),
            "-an",
            "-c:v",
            "libx264",
            "-preset",
            "veryfast",
            "-crf",
            "23",
            "-g",
            str(frames),
            "-keyint_min",
            str(frames),
            "-sc_threshold",
            "0",
            "-threads",
            "1",
            "-pix_fmt",
            "yuv420p",
            "-y",
            str(source),
        ],
        f"generating {content} {width}x{height} source",
    )
    if not source.is_file() or source.stat().st_size == 0:
        raise RuntimeError(f"FFmpeg generated an empty source for {content} {width}x{height}")

    source_sha256 = sha256_file(source)
    source_bytes = source.stat().st_size
    pack_seconds: list[float] = []
    unpack_seconds: list[float] = []
    archive_sizes: set[int] = set()
    archive_hashes: set[str] = set()
    representation_modes: set[str] = set()
    representation_reasons: set[str] = set()
    native_archive_sizes: set[int] = set()
    source_archive_sizes: set[int] = set()
    frame_count_exact_values: set[bool] = set()
    decoded_bytes: int | None = None
    pixel_format: str | None = None
    exact_match = True

    for repetition in range(repeats):
        archive = case_dir / f"archive-{repetition}.vfs"
        restored = case_dir / f"restored-{repetition}.nut"
        pack_result, pack_elapsed = timed_command(
            [
                str(frameforge),
                "pack",
                str(source),
                "--out",
                str(archive),
                "--threads",
                "1",
                "--compression",
                str(compression),
            ],
            f"packing {content} {width}x{height} repetition {repetition + 1}",
        )
        pack_stats = json.loads(pack_result.stdout)
        if pack_stats.get("frame_count") != frames:
            raise RuntimeError(
                f"pack decoded {pack_stats.get('frame_count')} frames; expected {frames}"
            )
        representation_mode = pack_stats.get("representation_mode")
        representation_reason = pack_stats.get("representation_reason")
        if representation_mode not in {"native-planes", "source-file"}:
            raise RuntimeError(f"unknown auto representation mode: {representation_mode!r}")
        if not isinstance(representation_reason, str) or not representation_reason:
            raise RuntimeError("pack output did not report a representation reason")
        frame_count_exact = pack_stats.get("frame_count_exact", True)
        is_fast_path = "fast-path" in representation_reason
        if is_fast_path and (representation_mode != "source-file" or frame_count_exact):
            raise RuntimeError("source-preservation fast path reported inconsistent mode or frame count")
        if not is_fast_path and not frame_count_exact:
            raise RuntimeError("full measured pack path did not report an exact decoded frame count")
        native_candidate_bytes = pack_stats.get("native_archive_bytes")
        source_candidate_bytes = pack_stats.get("source_file_archive_bytes")
        output_bytes = pack_stats.get("output_bytes")
        if not all(isinstance(value, int) for value in
                   (native_candidate_bytes, source_candidate_bytes, output_bytes)):
            raise RuntimeError("pack output omitted measured candidate sizes")
        if source_candidate_bytes <= 0 or output_bytes > source_candidate_bytes:
            raise RuntimeError("auto archive is larger than the exact source-file representation")
        if is_fast_path:
            if native_candidate_bytes != 0 or output_bytes != source_candidate_bytes:
                raise RuntimeError("fast path unexpectedly built a native candidate")
        elif native_candidate_bytes <= 0:
            raise RuntimeError("full measured path did not report a native candidate")
        current_pixel_format = pack_stats.get("pixel_format")
        if not isinstance(current_pixel_format, str) or not current_pixel_format:
            raise RuntimeError("pack output did not report a pixel format")
        if pixel_format is None:
            pixel_format = current_pixel_format
        elif current_pixel_format != pixel_format:
            raise RuntimeError("pixel format varied between repeated pack runs")

        unpack_result, unpack_elapsed = timed_command(
            [str(frameforge), "unpack", str(archive), "--out", str(restored)],
            f"unpacking {content} {width}x{height} repetition {repetition + 1}",
        )
        unpack_stats = json.loads(unpack_result.stdout)
        if representation_mode == "source-file":
            if unpack_stats.get("frame_count") != 0:
                raise RuntimeError("source-file unpack unexpectedly reported decoded frames")
            if not filecmp.cmp(source, restored, shallow=False):
                raise RuntimeError("source-file archive did not restore the source bytes exactly")
        else:
            if unpack_stats.get("frame_count") != frames:
                raise RuntimeError(
                    f"unpack restored {unpack_stats.get('frame_count')} frames; expected {frames}"
                )
            if unpack_stats.get("pixel_format") != pixel_format:
                raise RuntimeError("unpack changed the archived pixel format")

        source_raw = case_dir / "source.raw"
        restored_raw = case_dir / f"restored-{repetition}.raw"
        if repetition == 0:
            decode_raw(ffmpeg, source, source_raw, pixel_format)
            decoded_bytes = source_raw.stat().st_size
        decode_raw(ffmpeg, restored, restored_raw, pixel_format)
        if not filecmp.cmp(source_raw, restored_raw, shallow=False):
            exact_match = False
            raise RuntimeError(
                f"decoded bytes differ for {content} {width}x{height} repetition {repetition + 1}"
            )

        pack_seconds.append(pack_elapsed)
        unpack_seconds.append(unpack_elapsed)
        archive_sizes.add(archive.stat().st_size)
        archive_hashes.add(sha256_file(archive))
        representation_modes.add(representation_mode)
        representation_reasons.add(representation_reason)
        native_archive_sizes.add(native_candidate_bytes)
        source_archive_sizes.add(source_candidate_bytes)
        frame_count_exact_values.add(bool(frame_count_exact))
        restored_raw.unlink()
        restored.unlink()
        archive.unlink()

    if (len(archive_sizes) != 1 or len(archive_hashes) != 1 or
            len(representation_modes) != 1 or len(representation_reasons) != 1 or
            len(native_archive_sizes) != 1 or len(source_archive_sizes) != 1 or
            len(frame_count_exact_values) != 1):
        raise RuntimeError(
            f"archive output or representation is not deterministic across repetitions for {content} {width}x{height}"
        )
    if decoded_bytes is None or pixel_format is None:
        raise RuntimeError("benchmark case produced no validated measurements")
    source_to_decoded_ratio = source_bytes / decoded_bytes
    if (next(iter(representation_reasons)) == "source-density-fast-path" and
            source_to_decoded_ratio < SOURCE_DENSITY_FAST_PATH_THRESHOLD):
        raise RuntimeError("source-density fast path selected a below-threshold case")
    if (content == "static" and "fast-path" in next(iter(representation_reasons)) and
            source_to_decoded_ratio < SOURCE_DENSITY_FAST_PATH_THRESHOLD):
        raise RuntimeError("low-density static content was incorrectly source-fast-routed")

    pack_median = statistics.median(pack_seconds)
    unpack_median = statistics.median(unpack_seconds)
    archive_bytes = next(iter(archive_sizes))
    archive_over_source = archive_bytes / source_bytes
    return {
        "content": content,
        "width": width,
        "height": height,
        "frames": frames,
        "pixel_format": pixel_format,
        "source_bytes": source_bytes,
        "archive_bytes": archive_bytes,
        "archive_over_source": archive_over_source,
        "archive_savings_percent": (1.0 - archive_over_source) * 100.0,
        "decoded_bytes": decoded_bytes,
        "pack_seconds_median": pack_median,
        "pack_fps_median": frames / pack_median,
        "pack_mib_per_second_median": decoded_bytes / pack_median / (1024.0 * 1024.0),
        "unpack_seconds_median": unpack_median,
        "unpack_fps_median": frames / unpack_median,
        "unpack_mib_per_second_median": decoded_bytes / unpack_median / (1024.0 * 1024.0),
        "source_sha256": source_sha256,
        "archive_sha256": next(iter(archive_hashes)),
        "exact_match": exact_match,
        "representation_mode": next(iter(representation_modes)),
        "representation_reason": next(iter(representation_reasons)),
        "native_archive_bytes": next(iter(native_archive_sizes)),
        "source_file_archive_bytes": next(iter(source_archive_sizes)),
        "source_to_decoded_plane_ratio": source_to_decoded_ratio,
        "frame_count_exact": next(iter(frame_count_exact_values)),
    }


def main() -> int:
    args = parse_args()
    try:
        if args.build_type.strip().casefold() != "release":
            raise ValueError(
                "this benchmark is intended for Release builds; configure with "
                "-DCMAKE_BUILD_TYPE=Release (or select Release with a multi-config generator)"
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
        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        if not os.access(frameforge, os.X_OK):
            raise ValueError(f"FrameForge executable is not executable: {frameforge}")
        if not os.access(ffmpeg, os.X_OK):
            raise ValueError(f"FFmpeg executable is not executable: {ffmpeg}")

        ffmpeg_version = run_checked([str(ffmpeg), "-version"], "reading FFmpeg version")
        rows: list[dict[str, Any]] = []
        total = len(CONTENT_SOURCES) * len(sizes)
        with tempfile.TemporaryDirectory(prefix="frameforge-benchmark-", dir=output_dir) as temporary:
            work_dir = Path(temporary)
            completed = 0
            for content in CONTENT_SOURCES:
                for width, height in sizes:
                    row = benchmark_case(
                        ffmpeg=ffmpeg,
                        frameforge=frameforge,
                        work_dir=work_dir,
                        content=content,
                        width=width,
                        height=height,
                        frames=args.frames,
                        repeats=args.repeats,
                        compression=args.compression,
                    )
                    rows.append(row)
                    completed += 1
                    print(
                        f"[{completed}/{total}] {content:14s} {width:4d}x{height:<4d} "
                        f"archive/source={row['archive_over_source']:.3f} "
                        f"mode={row['representation_mode']} "
                        f"reason={row['representation_reason']} "
                        f"pack={row['pack_mib_per_second_median']:.2f} MiB/s "
                        f"unpack={row['unpack_mib_per_second_median']:.2f} MiB/s "
                        "exact=PASS",
                        flush=True,
                    )

        metadata: dict[str, Any] = {
            "schema_version": 1,
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            "build_type": args.build_type,
            "frameforge_executable": str(frameforge),
            "ffmpeg_executable": str(ffmpeg),
            "ffmpeg_version": ffmpeg_version.stdout.splitlines()[0],
            "python_version": sys.version.split()[0],
            "host_platform": platform.platform(),
            "configuration": {
                "frames_per_source": args.frames,
                "sizes": [{"width": width, "height": height} for width, height in sizes],
                "content_types": list(CONTENT_SOURCES),
                "repeats": args.repeats,
                "compression_level": args.compression,
                "source_codec": "libx264",
                "source_preset": "veryfast",
                "source_crf": 23,
                "source_threads": 1,
                "pack_threads": 1,
                "timing_scope": "end-to-end process wall time, including process startup and file I/O",
                "throughput_basis": "decoded active plane bytes",
            },
        }
        write_results(output_dir, rows, metadata)
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        return 1

    print(f"Results: {output_dir / 'results.csv'}")
    print(f"Run metadata: {output_dir / 'run.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
