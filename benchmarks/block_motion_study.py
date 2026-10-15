#!/usr/bin/env python3
"""Compare experimental exact block motion with VFS3 on the established corpus.

The encoder/decoder timings are codec-only: raw frames are preloaded, and the
measurements exclude FFmpeg source decode, container serialization, file I/O,
and process startup. Candidate archive sizes include a conservative eight-byte
VFS4 feature declaration and a four-byte vector-stream length on each motion
record. No VFS4 file is written by this experiment.
"""

from __future__ import annotations

import argparse
import csv
import json
import platform
import re
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


CONTENT_SOURCES = {
    "static": lambda width, height: f"color=c=0x315a83:s={width}x{height}:r=24",
    "moving_detail": lambda width, height: f"testsrc2=size={width}x{height}:rate=24",
    "temporal_noise": lambda width, height: (
        f"nullsrc=size={width}x{height}:rate=24,format=yuv420p,"
        "noise=alls=100:allf=t+u:all_seed=54711"
    ),
}

CSV_FIELDS = [
    "content", "width", "height", "frames", "pixel_format",
    "decoded_bytes", "vfs3_native_bytes", "motion_candidate_bytes",
    "size_delta_bytes", "size_delta_percent", "motion_frames_selected",
    "compressed_vector_bytes", "vector_metadata_bytes", "v4_header_bytes",
    "vfs3_pack_seconds", "motion_pack_seconds", "pack_time_ratio",
    "vfs3_unpack_seconds", "motion_unpack_seconds", "unpack_time_ratio",
    "vfs3_pack_mib_per_second", "motion_pack_mib_per_second",
    "vfs3_unpack_mib_per_second", "motion_unpack_mib_per_second",
    "baseline_exact", "motion_exact",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frameforge", required=True, type=Path)
    parser.add_argument("--motion-benchmark", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--frames", type=int, default=48)
    parser.add_argument("--sizes", default="320x180,640x360,1280x720")
    parser.add_argument("--repeats", type=int, default=3)
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
        raise RuntimeError(f"{description} failed with exit code {result.returncode}:\n{detail}")
    return result


def benchmark_case(
    *, ffmpeg: Path, frameforge: Path, motion_benchmark: Path,
    work_dir: Path, content: str, width: int, height: int,
    frames: int, repeats: int,
) -> dict[str, Any]:
    case_dir = work_dir / f"{content}-{width}x{height}"
    case_dir.mkdir(parents=True)
    source = case_dir / "source.mp4"
    raw = case_dir / "source.yuv"
    native_archive = case_dir / "native-check.vfs"

    run_checked(
        [
            str(ffmpeg), "-hide_banner", "-loglevel", "error", "-filter_threads", "1",
            "-f", "lavfi", "-i", CONTENT_SOURCES[content](width, height),
            "-frames:v", str(frames), "-an", "-c:v", "libx264",
            "-preset", "veryfast", "-crf", "23", "-g", str(frames),
            "-keyint_min", str(frames), "-sc_threshold", "0", "-threads", "1",
            "-pix_fmt", "yuv420p", "-y", str(source),
        ],
        f"generating {content} {width}x{height} source",
    )
    run_checked(
        [
            str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(source),
            "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p",
            "-f", "rawvideo", "-y", str(raw),
        ],
        f"decoding {content} {width}x{height} source to raw frames",
    )
    decoded_bytes = raw.stat().st_size

    pack_result = run_checked(
        [str(frameforge), "pack", str(source), "--out", str(native_archive),
         "--threads", "1", "--compression", "1"],
        f"measuring the current VFS3 native candidate for {content} {width}x{height}",
    )
    pack_stats = json.loads(pack_result.stdout)
    if pack_stats.get("frame_count") != frames:
        raise RuntimeError(f"VFS3 decoded the wrong frame count for {content} {width}x{height}")
    if pack_stats.get("pixel_format") != "yuv420p":
        raise RuntimeError(f"unexpected pixel format for {content} {width}x{height}")

    result = json.loads(run_checked(
        [str(motion_benchmark), "--raw", str(raw), "--width", str(width),
         "--height", str(height), "--frames", str(frames), "--repeats", str(repeats)],
        f"running block-motion codec study for {content} {width}x{height}",
    ).stdout)
    actual_vfs3_native_bytes = int(pack_stats["native_archive_bytes"])
    estimated_vfs3_bytes = int(result["baseline_vfs3_native_bytes"])
    if actual_vfs3_native_bytes != estimated_vfs3_bytes:
        raise RuntimeError(
            f"VFS3 byte model mismatch for {content} {width}x{height}: "
            f"measured {actual_vfs3_native_bytes}, codec estimate {estimated_vfs3_bytes}"
        )
    if not result["baseline_exact"] or not result["adaptive_exact"]:
        raise RuntimeError(f"byte-exact validation failed for {content} {width}x{height}")

    baseline_bytes = actual_vfs3_native_bytes
    motion_bytes = int(result["adaptive_motion_archive_bytes"])
    baseline_pack = float(result["baseline_pack_seconds"])
    motion_pack = float(result["adaptive_pack_seconds"])
    baseline_unpack = float(result["baseline_unpack_seconds"])
    motion_unpack = float(result["adaptive_unpack_seconds"])
    throughput_basis = decoded_bytes * frames
    return {
        "content": content,
        "width": width,
        "height": height,
        "frames": frames,
        "pixel_format": "yuv420p",
        "decoded_bytes": decoded_bytes,
        "vfs3_native_bytes": baseline_bytes,
        "motion_candidate_bytes": motion_bytes,
        "size_delta_bytes": motion_bytes - baseline_bytes,
        "size_delta_percent": (motion_bytes / baseline_bytes - 1.0) * 100.0,
        "motion_frames_selected": int(result["selected_motion_frames"]),
        "compressed_vector_bytes": int(result["compressed_vector_bytes"]),
        "vector_metadata_bytes": int(result["vector_metadata_bytes"]),
        "v4_header_bytes": int(result["v4_feature_header_bytes"]),
        "vfs3_pack_seconds": baseline_pack,
        "motion_pack_seconds": motion_pack,
        "pack_time_ratio": motion_pack / baseline_pack,
        "vfs3_unpack_seconds": baseline_unpack,
        "motion_unpack_seconds": motion_unpack,
        "unpack_time_ratio": motion_unpack / baseline_unpack,
        "vfs3_pack_mib_per_second": throughput_basis / baseline_pack / (1024 * 1024),
        "motion_pack_mib_per_second": throughput_basis / motion_pack / (1024 * 1024),
        "vfs3_unpack_mib_per_second": throughput_basis / baseline_unpack / (1024 * 1024),
        "motion_unpack_mib_per_second": throughput_basis / motion_unpack / (1024 * 1024),
        "baseline_exact": result["baseline_exact"],
        "motion_exact": result["adaptive_exact"],
    }


def main() -> int:
    args = parse_args()
    try:
        if args.frames < 2 or args.repeats < 1:
            raise ValueError("--frames must be at least 2 and --repeats must be positive")
        sizes = parse_sizes(args.sizes)
        frameforge = args.frameforge.resolve(strict=True)
        motion_benchmark = args.motion_benchmark.resolve(strict=True)
        ffmpeg = args.ffmpeg.resolve(strict=True)
        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        rows: list[dict[str, Any]] = []
        total = len(CONTENT_SOURCES) * len(sizes)
        with tempfile.TemporaryDirectory(prefix="frameforge-block-motion-", dir=output_dir) as temp:
            work_dir = Path(temp)
            completed = 0
            for content in CONTENT_SOURCES:
                for width, height in sizes:
                    row = benchmark_case(
                        ffmpeg=ffmpeg, frameforge=frameforge,
                        motion_benchmark=motion_benchmark, work_dir=work_dir,
                        content=content, width=width, height=height,
                        frames=args.frames, repeats=args.repeats,
                    )
                    rows.append(row)
                    completed += 1
                    print(
                        f"[{completed}/{total}] {content:14s} {width:4d}x{height:<4d} "
                        f"sizeΔ={row['size_delta_percent']:+.2f}% "
                        f"motion={row['motion_frames_selected']:2d}/{args.frames} "
                        f"pack×{row['pack_time_ratio']:.2f} "
                        f"unpack×{row['unpack_time_ratio']:.2f} exact=PASS",
                        flush=True,
                    )

        csv_path = output_dir / "block-motion-study.csv"
        with csv_path.open("w", newline="", encoding="utf-8") as destination:
            writer = csv.DictWriter(destination, fieldnames=CSV_FIELDS)
            writer.writeheader()
            writer.writerows(rows)

        ffmpeg_version = run_checked([str(ffmpeg), "-version"], "reading FFmpeg version")
        metadata = {
            "schema_version": 1,
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            "host_platform": platform.platform(),
            "python_version": sys.version.split()[0],
            "ffmpeg_version": ffmpeg_version.stdout.splitlines()[0],
            "configuration": {
                "frames_per_source": args.frames,
                "sizes": [{"width": w, "height": h} for w, h in sizes],
                "content_types": list(CONTENT_SOURCES),
                "repeats": args.repeats,
                "source_codec": "libx264",
                "source_preset": "veryfast",
                "source_crf": 23,
                "source_threads": 1,
                "zstandard_level": 1,
                "motion_block_size": 16,
                "motion_search_radius_samples": 4,
                "motion_sad_sampling": "up to 4x4 evenly spaced samples per block",
            },
            "timing_scope": (
                "codec-only; raw frames preloaded; excludes FFmpeg source decode, "
                "VFS container I/O, disk I/O, and process startup"
            ),
            "archive_size_model": (
                "actual VFS3 native archive bytes; motion candidate includes an "
                "estimated 8-byte VFS4 feature header and 4-byte length prefix "
                "per selected motion frame; no VFS4 archive is written"
            ),
            "results": rows,
        }
        (output_dir / "block-motion-study.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        print(f"Wrote {csv_path}")
        return 0
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"block-motion study failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
