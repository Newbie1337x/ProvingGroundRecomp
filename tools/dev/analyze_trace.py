#!/usr/bin/env python3
"""Break down a Tracy capture of the game into GPU-thread work, GPU waits on the
guest, and guest thread activity, per frame inside the measurement window."""
import argparse
import csv
import io
import subprocess
from collections import defaultdict
from pathlib import Path

DEFAULT_EXPORT = Path(__file__).resolve().parents[1] / "windows-0.13.1" / "tracy-csvexport.exe"

# Zones are identified by source file and line, because every GPU zone is named "gpu".
PRIMARY = ("graphics/command_processor.cpp", "ExecutePrimaryBuffer")
WAIT_REG_MEM = ("graphics/command_processor.cpp", "ExecutePacketType3_WAIT_REG_MEM")
ISSUE_DRAW = ("graphics/vulkan/command_processor.cpp", "IssueDraw")
SWAP = ("graphics/vulkan/command_processor.cpp", "IssueSwap")
# Kernel calls in which a guest thread blocks instead of running game code.
BLOCKING = ("__imp__NtWaitForSingleObjectEx", "__imp__NtWaitForMultipleObjectsEx",
            "__imp__KeWaitForSingleObject", "__imp__KeWaitForMultipleObjects",
            "__imp__KeDelayExecutionThread", "__imp__NtYieldExecution",
            "__imp__NtSignalAndWaitForSingleObjectEx")


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def export(exe, trace, *options):
    """Stream csvexport rows; per-event exports can have millions of lines."""
    process = subprocess.Popen([str(exe), *options, str(trace)], stdout=subprocess.PIPE)
    stream = io.TextIOWrapper(process.stdout, encoding="utf-8", errors="replace", newline="")
    yield from csv.DictReader(stream)
    if process.wait():
        raise SystemExit(f"tracy-csvexport failed with options {options}")


def find_line(sdk_src, relative, function):
    """Source line of the SCOPE_profile_cpu_f inside `function`, as Tracy records it."""
    lines = (sdk_src / "src" / relative).read_text(encoding="utf-8").splitlines()
    for number, text in enumerate(lines, 1):
        if function + "(" in text and "::" in text:
            for offset in range(number, min(number + 12, len(lines))):
                if "SCOPE_profile_cpu_f" in lines[offset]:
                    return offset + 1
    raise SystemExit(f"cannot find the profiling zone of {function} in {relative}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--csvexport", type=Path, default=DEFAULT_EXPORT)
    parser.add_argument("--sdk-src", type=Path, required=True,
                        help="ReXGlue source checkout the game was built from")
    args = parser.parse_args()

    ids = {key: (key[0], find_line(args.sdk_src, *key)) for key in
           (PRIMARY, WAIT_REG_MEM, ISSUE_DRAW, SWAP)}

    messages = {row["MessageName"]: int(row["total_ns"]) for row in export(args.csvexport, args.trace, "-m")}
    start = messages.get("measure_start", 0)
    stop = messages.get("measure_stop", float("inf"))

    # One IssueSwap per presented frame delimits the frames.
    events = []
    for row in export(args.csvexport, args.trace, "-u", "-f", "gpu"):
        key = (row["src_file"].split("/src/")[-1], int(row["src_line"]))
        for name, zone in ids.items():
            if key == zone:
                events.append((int(row["ns_since_start"]), int(row["exec_time_ns"]), name, row["thread"]))
    frames = sorted(t for t, _, name, _ in events if name is SWAP and start <= t <= stop)
    if len(frames) < 3:
        raise SystemExit("fewer than three frames in the measurement window")
    window_ns = frames[-1] - frames[0]

    def frame_of(t):
        if t < frames[0] or t >= frames[-1]:
            return None
        lo, hi = 0, len(frames) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if frames[mid] <= t:
                lo = mid
            else:
                hi = mid
        return lo

    per_frame = defaultdict(lambda: defaultdict(float))
    gpu_thread = None
    for t, duration, name, thread in events:
        if name is PRIMARY:
            gpu_thread = thread
        index = frame_of(t)
        if index is None:
            continue
        per_frame[index][name[1]] += duration
        if name is ISSUE_DRAW:
            per_frame[index]["draws"] += 1

    blocked = defaultdict(float)
    # Critical sections are left out: millions of events, and contention is rare.
    for pattern in ("WaitFor", "Delay", "Yield"):
        for row in export(args.csvexport, args.trace, "-u", "-f", pattern):
            if row["name"] not in BLOCKING or row["thread"] == gpu_thread:
                continue
            t0 = int(row["ns_since_start"])
            t1 = t0 + int(row["exec_time_ns"])
            overlap = min(t1, frames[-1]) - max(t0, frames[0])
            if overlap > 0:
                blocked[row["thread"]] += overlap

    frame_ms = [(frames[i + 1] - frames[i]) / 1e6 for i in range(len(frames) - 1)]
    work_ms, wait_ms, draw_ms, draws = [], [], [], []
    for i in range(len(frames) - 1):
        f = per_frame[i]
        wait = f["ExecutePacketType3_WAIT_REG_MEM"]
        work_ms.append((f["ExecutePrimaryBuffer"] - wait) / 1e6)
        wait_ms.append(wait / 1e6)
        draw_ms.append(f["IssueDraw"] / 1e6)
        draws.append(f["draws"])

    def mean(values):
        return sum(values) / len(values)

    print(f"Trace: {args.trace}")
    print(f"Window: {window_ns / 1e9:.2f} s, {len(frame_ms)} frames "
          f"({'measure_start/stop' if 'measure_start' in messages else 'whole capture'})")
    print(f"Frame: mean {mean(frame_ms):.2f} ms; p95 {percentile(frame_ms, .95):.2f} ms")
    print("GPU thread (translates Xenos commands to Vulkan), mean per frame:")
    print(f"  work                    {mean(work_ms):6.2f} ms  ({100 * sum(work_ms) / sum(frame_ms):4.1f}% of the time)")
    print(f"    of which IssueDraw    {mean(draw_ms):6.2f} ms  ({mean(draws):.0f} draws, "
          f"{1000 * sum(draw_ms) / max(sum(draws), 1):.1f} us/draw)")
    print(f"  waiting for the game    {mean(wait_ms):6.2f} ms  (WAIT_REG_MEM)")
    print(f"  no commands to read     {mean(frame_ms) - mean(work_ms) - mean(wait_ms):6.2f} ms  "
          "(the game has not submitted the frame yet)")
    print(f"  => the GPU thread alone would allow up to {1000 / max(mean(work_ms), 1e-3):.0f} fps")
    print("By draw count (frames in the window):")
    for low, high in ((0, 500), (500, 1000), (1000, 1500), (1500, 10**9)):
        chosen = [i for i, d in enumerate(draws) if low <= d < high]
        if chosen:
            work = mean([work_ms[i] for i in chosen])
            print(f"  {low:>5}-{'+' if high > 10**8 else high:<5} draws: {len(chosen):5} frames, "
                  f"GPU work {work:6.2f} ms (max {1000 / max(work, 1e-3):4.0f} fps), "
                  f"wait {mean([wait_ms[i] for i in chosen]):6.2f} ms")
    print("Busiest game threads (time outside kernel waits):")
    for thread, ns in sorted(blocked.items(), key=lambda item: item[1])[:5]:
        print(f"  thread {thread:>4}: active {100 * (1 - ns / window_ns):5.1f}% of the window")
    print("A game thread near 100% active while the GPU thread waits means the game code is the")
    print("limit; a GPU thread near 100% busy means the renderer backend is.")


if __name__ == "__main__":
    main()
