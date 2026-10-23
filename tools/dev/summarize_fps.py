#!/usr/bin/env python3
"""Summarize ReXGlue guest-swap intervals, rather than averaging FPS values."""
import argparse
import csv
import math
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


LOAD_COLUMNS = ("draw_calls", "vertices_processed", "texture_cache_misses",
                "pipeline_cache_misses", "command_buffer_stalls")


def print_load_comparison(samples, rows):
    """Compare GPU load counters in the slowest 5% of frames against the median half."""
    columns = [c for c in LOAD_COLUMNS if c in rows[0]]
    if not columns or len(samples) < 20:
        return
    def mean(indices, column):
        values = []
        for i in indices:
            try:
                values.append(float(rows[i][column]))
            except (ValueError, TypeError):
                pass
        return sum(values) / len(values) if values else 0.0
    slow_limit = percentile(samples, .95)
    low, high = percentile(samples, .25), percentile(samples, .75)
    slow = [i for i, v in enumerate(samples) if v >= slow_limit]
    typical = [i for i, v in enumerate(samples) if low <= v <= high]
    if not any(mean(range(len(rows)), c) for c in columns):
        print("Load counters are all zero: the CSV comes from a build without the Vulkan counters.")
        return
    print(f"Load per frame (mean): slowest 5% ({len(slow)}) vs typical p25-p75 frames ({len(typical)})")
    for column in columns:
        print(f"  {column:24} {mean(slow, column):12.1f} vs {mean(typical, column):12.1f}")


def print_gpu_thread(rows):
    """GPU command processor thread work per frame."""
    if "gpu_busy_ns" not in rows[0]:
        return
    work, draws = [], []
    protects, faults = [], []
    for row in rows:
        try:
            work.append((int(row["gpu_busy_ns"]) - int(row["gpu_wait_ns"])) / 1e6)
            draws.append(int(row["draw_calls"]))
            protects.append(int(row.get("page_protect_calls", 0)))
            faults.append(int(row.get("page_faults", 0)))
        except (ValueError, TypeError, KeyError):
            continue
    if not work:
        return
    mean_work = sum(work) / len(work)
    print(f"GPU thread: mean work {mean_work:.2f} ms/frame (excluding waits for the game); "
          f"alone it would allow {1000 / max(mean_work, 1e-3):.0f} fps")
    if any(protects):
        print(f"  memory watches: {sum(protects) / len(protects):.0f} VirtualProtect and "
              f"{sum(faults) / len(faults):.0f} page faults per frame")
    for low, high in ((0, 500), (500, 1000), (1000, 1500), (1500, 10**9)):
        chosen = [w for w, d in zip(work, draws) if low <= d < high]
        if chosen:
            value = sum(chosen) / len(chosen)
            label = f"{low}-{high}" if high < 10**8 else f"{low}+"
            draw_total = sum(d for d in draws if low <= d < high)
            print(f"  {label:>9} draws: {len(chosen):5} frames, work {value:6.2f} ms "
                  f"(max {1000 / max(value, 1e-3):4.0f} fps), "
                  f"{1000 * sum(chosen) / max(draw_total, 1):5.2f} us/draw")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--skip-seconds", type=float, default=5.0,
                        help="skip this many seconds from the first recorded swap (default: 5)")
    parser.add_argument("--ignore-markers", action="store_true",
                        help="use the whole capture even if it has a measure_start marker")
    args = parser.parse_args()
    if not math.isfinite(args.skip_seconds) or args.skip_seconds < 0:
        parser.error("--skip-seconds must be finite and nonnegative")
    elapsed_us = 0.0
    samples = []
    rows_kept = []
    invalid = 0
    with args.csv.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if "frame_time_us" not in (reader.fieldnames or []):
            parser.error("CSV has no frame_time_us column; use the diagnostic build")
        rows = list(reader)
    # marker: 1 = measure_start, 2 = measure_stop (input replay/recording)
    markers = [row.get("marker", "0") for row in rows]
    window = None
    if not args.ignore_markers and "1" in markers:
        start = markers.index("1")
        stop = markers.index("2", start) if "2" in markers[start:] else len(rows)
        window = (start, stop)
        rows = rows[start:stop]
        args.skip_seconds = 0.0
    for row in rows:
        try:
            value = float(row["frame_time_us"])
        except (ValueError, TypeError):
            invalid += 1
            continue
        if not math.isfinite(value) or value <= 0:
            invalid += 1
            continue
        elapsed_us += value
        if elapsed_us > args.skip_seconds * 1_000_000:
            samples.append(value)
            rows_kept.append(row)
    if not samples:
        parser.error("no positive samples remain; record longer or reduce --skip-seconds")
    duration = sum(samples) / 1_000_000
    print(f"File: {args.csv}")
    print("Diagnostic build; the counters add measurement overhead.")
    if window:
        closed = "measure_stop" if window[1] < len(markers) else "end of file (no measure_stop)"
        print(f"Marked window: frames {window[0]}-{window[1]} (measure_start to {closed}); "
              f"invalid/zero rows: {invalid}")
    else:
        print(f"Skipped from the first swap: {args.skip_seconds:g} s; invalid/zero rows: {invalid}")
    print(f"Sample: {len(samples)} frames / {duration:.2f} s")
    print(f"Effective mean FPS: {len(samples) / duration:.2f}")
    print(f"Frame time: median {percentile(samples, .50) / 1000:.2f} ms; "
          f"p95 {percentile(samples, .95) / 1000:.2f} ms; p99 {percentile(samples, .99) / 1000:.2f} ms")
    print_load_comparison(samples, rows_kept)
    print_gpu_thread(rows_kept)
    if not window:
        print("The sample may include menus, videos or loading; it does not prove gameplay performance.")


if __name__ == "__main__":
    main()
