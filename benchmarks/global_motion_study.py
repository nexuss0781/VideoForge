#!/usr/bin/env python3
"""Measure gated global-translation refinement on native YUV420 frame sequences.

Static, moving-detail, and temporal-noise sources follow the established
FrameForge benchmark's fixed FFmpeg settings. Three additional raw YUV sources
are exact crops of one deterministic high-entropy scene: horizontal pan, vertical
pan, and diagonal translation. Codec timings exclude source generation,
container I/O, and process startup; archive sizes use the VFS3 header/record
model and are cross-checked against actual FrameForge native archives.
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


CONTENT_SOURCES = ("static", "temporal_noise", "moving_detail",
                   "horizontal_pan", "vertical_pan", "diagonal_translation")
CSV_FIELDS = [
    "content", "width", "height", "frames", "decoded_bytes",
    "vfs3_native_bytes", "gated_candidate_bytes", "size_delta_bytes",
    "size_delta_percent", "gate_pass_frames", "selected_motion_frames",
    "vfs3_encode_seconds", "gated_encode_seconds", "encode_time_ratio",
    "vfs3_decode_seconds", "gated_decode_seconds", "decode_time_ratio",
    "vfs3_encode_mib_per_second", "gated_encode_mib_per_second",
    "vfs3_decode_mib_per_second", "gated_decode_mib_per_second",
    "baseline_exact", "gated_exact",
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
        width, height = int(match.group(1)), int(match.group(2))
        if width <= 64 or height <= 64 or width % 2 or height % 2:
            raise ValueError(f"size {item!r} must be even and greater than 64 pixels")
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


def source_graph(content: str, width: int, height: int, frames: int) -> str:
    if content == "static":
        return f"color=c=0x315a83:s={width}x{height}:r=24"
    if content == "moving_detail":
        return f"testsrc2=size={width}x{height}:rate=24"
    if content == "temporal_noise":
        return (f"nullsrc=size={width}x{height}:rate=24,format=yuv420p,"
                "noise=alls=100:allf=t+u:all_seed=54711")

    max_shift = 2 * min(max(frames - 1, 0), 16)
    background_width = width + (max_shift if content in
                                 ("horizontal_pan", "diagonal_translation") else 0)
    background_height = height + (max_shift if content in
                                  ("vertical_pan", "diagonal_translation") else 0)
    if content == "horizontal_pan":
        x_expr, y_expr = f"min(n*2,{max_shift})", "0"
    elif content == "vertical_pan":
        x_expr, y_expr = "0", f"min(n*2,{max_shift})"
    elif content == "diagonal_translation":
        x_expr = y_expr = f"min(n*2,{max_shift})"
    else:
        raise ValueError(f"unknown content type: {content}")

    # A frame-index-independent geq texture is cropped by an even, controlled
    # offset per frame, yielding exact YUV translations without an interframe
    # video encoder's quantization or motion compensation.
    texture = (
        f"nullsrc=size={background_width}x{background_height}:rate=24,"
        "format=yuv420p,noise=alls=100:allf=u:all_seed=54711,"
        f"crop={width}:{height}:x='{x_expr}':y='{y_expr}'"
    )
    return texture


def generate_source(ffmpeg: Path, content: str, width: int, height: int,
                    frames: int, case_dir: Path) -> tuple[Path, Path]:
    raw = case_dir / "source.yuv"
    if content in ("static", "moving_detail", "temporal_noise"):
        source = case_dir / "source.mp4"
        run_checked(
            [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-filter_threads", "1",
             "-f", "lavfi", "-i", source_graph(content, width, height, frames),
             "-frames:v", str(frames), "-an", "-c:v", "libx264", "-preset", "veryfast",
             "-crf", "23", "-g", str(frames), "-keyint_min", str(frames),
             "-sc_threshold", "0", "-threads", "1", "-pix_fmt", "yuv420p",
             "-y", str(source)],
            f"generating {content} {width}x{height} source",
        )
    else:
        source = case_dir / "source.y4m"
        run_checked(
            [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-filter_threads", "1",
             "-f", "lavfi", "-i", source_graph(content, width, height, frames),
             "-frames:v", str(frames), "-an", "-c:v", "rawvideo", "-threads", "1",
             "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", "-y", str(source)],
            f"generating exact {content} {width}x{height} YUV source",
        )

    run_checked(
        [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(source),
         "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "yuv420p",
         "-f", "rawvideo", "-y", str(raw)],
        f"decoding {content} {width}x{height} to native YUV420 frames",
    )
    expected_size = width * height * 3 // 2 * frames
    if raw.stat().st_size != expected_size:
        raise RuntimeError(
            f"{content} raw size mismatch: got {raw.stat().st_size}, expected {expected_size}"
        )
    return source, raw


def benchmark_case(*, ffmpeg: Path, frameforge: Path, motion_benchmark: Path,
                   work_dir: Path, content: str, width: int, height: int,
                   frames: int, repeats: int) -> dict[str, Any]:
    case_dir = work_dir / f"{content}-{width}x{height}"
    case_dir.mkdir(parents=True)
    source, raw = generate_source(ffmpeg, content, width, height, frames, case_dir)
    archive = case_dir / "native.vfs"
    pack_stats = json.loads(run_checked(
        [str(frameforge), "pack", str(source), "--out", str(archive),
         "--threads", "1", "--compression", "1"],
        f"packing native {content} {width}x{height} source",
    ).stdout)
    if pack_stats.get("frame_count") != frames or pack_stats.get("pixel_format") != "yuv420p":
        raise RuntimeError(f"FrameForge returned unexpected metadata for {content} {width}x{height}")

    result = json.loads(run_checked(
        [str(motion_benchmark), "--raw", str(raw), "--width", str(width),
         "--height", str(height), "--frames", str(frames), "--repeats", str(repeats)],
        f"running gated global-motion study for {content} {width}x{height}",
    ).stdout)
    actual_native_bytes = int(pack_stats["native_archive_bytes"])
    modeled_native_bytes = int(result["baseline_vfs3_native_bytes"])
    if actual_native_bytes != modeled_native_bytes:
        raise RuntimeError(
            f"VFS3 size model mismatch for {content} {width}x{height}: "
            f"actual {actual_native_bytes}, modeled {modeled_native_bytes}"
        )
    if not result["baseline_exact"] or not result["gated_exact"]:
        raise RuntimeError(f"byte-exact validation failed for {content} {width}x{height}")

    baseline_bytes = actual_native_bytes
    candidate_bytes = int(result["gated_candidate_archive_bytes"])
    baseline_encode = float(result["baseline_encode_seconds"])
    gated_encode = float(result["gated_encode_seconds"])
    baseline_decode = float(result["baseline_decode_seconds"])
    gated_decode = float(result["gated_decode_seconds"])
    decoded_bytes = int(raw.stat().st_size)
    timing_basis = decoded_bytes
    return {
        "content": content,
        "width": width,
        "height": height,
        "frames": frames,
        "decoded_bytes": decoded_bytes,
        "vfs3_native_bytes": baseline_bytes,
        "gated_candidate_bytes": candidate_bytes,
        "size_delta_bytes": candidate_bytes - baseline_bytes,
        "size_delta_percent": (candidate_bytes / baseline_bytes - 1.0) * 100.0,
        "gate_pass_frames": int(result["gate_pass_frames"]),
        "selected_motion_frames": int(result["selected_motion_frames"]),
        "vfs3_encode_seconds": baseline_encode,
        "gated_encode_seconds": gated_encode,
        "encode_time_ratio": gated_encode / baseline_encode,
        "vfs3_decode_seconds": baseline_decode,
        "gated_decode_seconds": gated_decode,
        "decode_time_ratio": gated_decode / baseline_decode,
        "vfs3_encode_mib_per_second": timing_basis / baseline_encode / (1024 * 1024),
        "gated_encode_mib_per_second": timing_basis / gated_encode / (1024 * 1024),
        "vfs3_decode_mib_per_second": timing_basis / baseline_decode / (1024 * 1024),
        "gated_decode_mib_per_second": timing_basis / gated_decode / (1024 * 1024),
        "baseline_exact": result["baseline_exact"],
        "gated_exact": result["gated_exact"],
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
        with tempfile.TemporaryDirectory(prefix="frameforge-global-motion-", dir=output_dir) as temp:
            work_dir = Path(temp)
            for completed, (content, (width, height)) in enumerate(
                    ((name, size) for name in CONTENT_SOURCES for size in sizes), start=1):
                row = benchmark_case(
                    ffmpeg=ffmpeg, frameforge=frameforge,
                    motion_benchmark=motion_benchmark, work_dir=work_dir,
                    content=content, width=width, height=height,
                    frames=args.frames, repeats=args.repeats,
                )
                rows.append(row)
                print(
                    f"[{completed}/{total}] {content:20s} {width:4d}x{height:<4d} "
                    f"sizeΔ={row['size_delta_percent']:+.2f}% "
                    f"gate={row['gate_pass_frames']:2d} "
                    f"selected={row['selected_motion_frames']:2d} "
                    f"encode×{row['encode_time_ratio']:.2f} "
                    f"decode×{row['decode_time_ratio']:.2f} exact=PASS",
                    flush=True,
                )

        csv_path = output_dir / "global-motion-study.csv"
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
                "compression_level": 1,
                "luma_search_sample_step": 16,
                "luma_gate_validation_sample_step": 4,
                "coarse_luma_box_size_pixels": 8,
                "coarse_search_radius_pixels": 32,
                "coarse_search_step_pixels": 8,
                "fine_search_radius_pixels": 7,
                "global_motion_gate_minimum_sampled_sad_improvement": 0.25,
                "block_size": 16,
                "local_search_radius_pixels_per_plane": 4,
                "pan_translation_per_frame_pixels": 2,
                "maximum_pan_offset_pixels": 32,
            },
            "timing_scope": (
                "codec-only with raw frames preloaded; includes baseline candidate encoding, "
                "global-luma estimation, gated block refinement and candidate selection; "
                "excludes source generation, VFS I/O and process startup"
            ),
            "archive_size_model": (
                "actual VFS3 native archive bytes cross-checked against the frame codec; "
                "candidate includes an estimated 8-byte feature declaration, 2-byte global "
                "translation and 4-byte vector length per selected motion record; no new "
                "archive format is written"
            ),
            "results": rows,
        }
        baseline_total = sum(row["vfs3_native_bytes"] for row in rows)
        candidate_total = sum(row["gated_candidate_bytes"] for row in rows)
        pan_rows = [row for row in rows if row["content"] in
                    ("horizontal_pan", "vertical_pan", "diagonal_translation")]
        pan_baseline_total = sum(row["vfs3_native_bytes"] for row in pan_rows)
        pan_candidate_total = sum(row["gated_candidate_bytes"] for row in pan_rows)
        metadata["aggregates"] = {
            "case_count": len(rows),
            "archive_size_reduction_percent_all_bytes":
                (1.0 - candidate_total / baseline_total) * 100.0,
            "archive_size_reduction_percent_pan_bytes":
                (1.0 - pan_candidate_total / pan_baseline_total) * 100.0,
            "pan_baseline_archive_bytes": pan_baseline_total,
            "pan_candidate_archive_bytes": pan_candidate_total,
            "encode_time_ratio_weighted_by_baseline_time":
                sum(row["gated_encode_seconds"] for row in rows) /
                sum(row["vfs3_encode_seconds"] for row in rows),
            "decode_time_ratio_weighted_by_baseline_time":
                sum(row["gated_decode_seconds"] for row in rows) /
                sum(row["vfs3_decode_seconds"] for row in rows),
            "pan_encode_time_ratio_weighted_by_baseline_time":
                sum(row["gated_encode_seconds"] for row in pan_rows) /
                sum(row["vfs3_encode_seconds"] for row in pan_rows),
            "pan_decode_time_ratio_weighted_by_baseline_time":
                sum(row["gated_decode_seconds"] for row in pan_rows) /
                sum(row["vfs3_decode_seconds"] for row in pan_rows),
        }
        json_path = output_dir / "global-motion-study.json"
        json_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
        mean_size_delta = statistics.mean(row["size_delta_percent"] for row in rows)
        mean_encode_ratio = statistics.mean(row["encode_time_ratio"] for row in rows)
        mean_decode_ratio = statistics.mean(row["decode_time_ratio"] for row in rows)
        summary = [
            "# Gated Global-Translation Experiment",
            "",
            f"Generated: {metadata['generated_utc']}",
            f"Cases: {len(rows)} ({len(CONTENT_SOURCES)} content types × {len(sizes)} sizes)",
            "Exact sample reconstruction: PASS for every case.",
            "",
            ("Byte-weighted archive-size reduction across all cases: "
             f"{metadata['aggregates']['archive_size_reduction_percent_all_bytes']:.3f}% ("
             f"{baseline_total:,} → {candidate_total:,} bytes)."),
            ("Byte-weighted archive-size reduction across the nine pan/translation cases: "
             f"{metadata['aggregates']['archive_size_reduction_percent_pan_bytes']:.3f}% ("
             f"{pan_baseline_total:,} → {pan_candidate_total:,} bytes)."),
            f"Unweighted mean per-case archive-size change: {mean_size_delta:+.3f}%.",
            f"Mean encode-time ratio: {mean_encode_ratio:.3f}× (1.0 is baseline speed).",
            f"Mean decode-time ratio: {mean_decode_ratio:.3f}× (1.0 is baseline speed).",
            ("Elapsed-time-weighted encode/decode ratios: "
             f"{metadata['aggregates']['encode_time_ratio_weighted_by_baseline_time']:.3f}× / "
             f"{metadata['aggregates']['decode_time_ratio_weighted_by_baseline_time']:.3f}×."),
            "",
            "| Content | Size | Archive Δ | Gate passes | Selected | Encode × | Decode × |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
        for row in rows:
            summary.append(
                f"| {row['content']} | {row['width']}×{row['height']} | "
                f"{row['size_delta_percent']:+.2f}% | {row['gate_pass_frames']} | "
                f"{row['selected_motion_frames']} | {row['encode_time_ratio']:.2f}× | "
                f"{row['decode_time_ratio']:.2f}× |"
            )
        summary.extend([
            "",
            "The candidate is an experiment-only byte model, not a production VFS format. "
            "A candidate motion record is selected only when its payload and first-use "
            "feature cost are smaller than the baseline record.",
            "",
        ])
        summary_path = output_dir / "global-motion-study.md"
        summary_path.write_text("\n".join(summary), encoding="utf-8")
        print(f"Wrote {csv_path}")
        print(f"Wrote {json_path}")
        print(f"Wrote {summary_path}")
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, ZeroDivisionError) as error:
        print(f"global-motion study failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
