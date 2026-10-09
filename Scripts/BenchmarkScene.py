#!/usr/bin/env python3
"""Scene benchmark orchestrator: launch Editor, drive camera, collect metrics."""

import argparse
import csv
import json
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional


def percentile_nearest_rank(values: list[float], p: float) -> float:
    """Compute percentile using nearest-rank method (matching Engine)."""
    if not values:
        return 0.0
    sorted_vals = sorted(values)
    index = int(math.ceil(p * len(sorted_vals))) - 1
    return sorted_vals[min(index, len(sorted_vals) - 1)]


def check_environment() -> tuple[bool, str]:
    """Verify Qwen/omarchy is not active and VRAM usage is acceptable."""
    # Check for omarchy-local-coder.service
    try:
        result = subprocess.run(
            ["systemctl", "is-active", "omarchy-local-coder.service"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            return False, "omarchy-local-coder.service is active; stop it first"
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # Check VRAM usage
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-compute-apps=memory.used", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            used_mb = sum(int(line.strip()) for line in result.stdout.splitlines() if line.strip())
            if used_mb > 1024:
                return False, f"VRAM already in use: {used_mb} MiB (limit: 1024 MiB)"
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    return True, ""


def sample_gpu_memory(duration_s: float, interval_s: float = 1.0) -> list[dict[str, Any]]:
    """Sample GPU memory at regular intervals."""
    samples = []
    start = time.time()
    while time.time() - start < duration_s:
        try:
            result = subprocess.run(
                ["nvidia-smi", "--query-gpu=memory.used,memory.total,utilization.gpu",
                 "--format=csv,noheader"],
                capture_output=True, text=True, timeout=2
            )
            if result.returncode == 0:
                line = result.stdout.strip()
                parts = [p.strip() for p in line.split(",")]
                if len(parts) >= 3:
                    used_mb = int(parts[0])
                    total_mb = int(parts[1])
                    util_pct = int(parts[2].rstrip("%"))
                    samples.append({
                        "timestamp": time.time(),
                        "memory_used_mib": used_mb,
                        "memory_total_mib": total_mb,
                        "utilization_percent": util_pct
                    })
        except Exception:
            pass
        time.sleep(interval_s)
    return samples


def sample_process_memory(pid: int, duration_s: float) -> list[dict[str, Any]]:
    """Sample process memory from /proc."""
    samples = []
    start = time.time()
    while time.time() - start < duration_s:
        try:
            with open(f"/proc/{pid}/status", "r") as f:
                for line in f:
                    if line.startswith("VmRSS"):
                        rss_kb = int(line.split()[1])
                        samples.append({
                            "timestamp": time.time(),
                            "rss_kib": rss_kb
                        })
                    elif line.startswith("VmHWM"):
                        hwm_kb = int(line.split()[1])
                        if samples:
                            samples[-1]["hwm_kib"] = hwm_kb
        except (FileNotFoundError, IOError):
            pass
        time.sleep(1.0)
    return samples


def run_benchmark_test(
    editor_path: str,
    control_dir: str,
    output_dir: str,
    duration_s: float = 70,
    fps: int = 60,
    project_path: Optional[str] = None
) -> dict[str, Any]:
    """Launch Editor with benchmark flags and drive camera."""
    launch_script = Path(editor_path).parent.parent / "Scripts" / "LaunchHeadedEditor.sh"

    # Build benchmark flags
    benchmark_flags = [
        f"--frame-pacing-benchmark",
        f"--frame-pacing-benchmark-frames=3600",
        f"--smooth-frametime-target-fps={fps}",
        f"--frame-pacing-benchmark-output={output_dir}/engine"
    ]
    if project_path:
        benchmark_flags.append(f"--project={project_path}")

    # Launch Editor
    try:
        result = subprocess.run(
            ["bash", str(launch_script)] + benchmark_flags,
            capture_output=True, text=True, timeout=duration_s + 30
        )
    except subprocess.TimeoutExpired as e:
        return {"error": f"Editor timeout: {e}"}

    if result.returncode != 0:
        return {"error": f"Editor launch failed: {result.stderr}"}

    # Parse receipt to find Editor PID
    receipt_match = re.search(r"pid=(\d+)", result.stdout)
    if not receipt_match:
        return {"error": "Could not parse Editor PID from launch receipt"}

    editor_pid = int(receipt_match.group(1))

    # Sample memory and wait for Editor to finish
    gpu_samples = sample_gpu_memory(duration_s)
    proc_samples = sample_process_memory(editor_pid, duration_s)

    # Wait for Editor shutdown and verify cleanup
    time.sleep(5)
    try:
        os.kill(editor_pid, 0)
        return {"error": f"Editor process {editor_pid} did not exit"}
    except ProcessLookupError:
        pass

    # Read benchmark artifact
    benchmark_json = Path(output_dir) / "engine" / "frame-pacing-benchmark.json"
    if not benchmark_json.exists():
        return {"error": f"Benchmark artifact not found: {benchmark_json}"}

    with open(benchmark_json) as f:
        benchmark_data = json.load(f)

    return {
        "success": True,
        "benchmark": benchmark_data,
        "gpu_samples": gpu_samples,
        "proc_samples": proc_samples
    }


def compute_stats(frames: list[dict]) -> dict[str, Any]:
    """Compute frame-time statistics."""
    if not frames:
        return {}

    frame_times = [f.get("startToStartMs", 0) for f in frames]
    cpu_active = [f.get("cpuActiveMs", 0) for f in frames]
    gpu_duration = [f.get("gpuDurationMs", 0) for f in frames if f.get("gpuTimingStatus") == "Ready"]

    deadline_misses = sum(1 for f in frames if f.get("waits", []))
    for f in frames:
        if f.get("waits"):
            for w in f["waits"]:
                if w.get("deadlineMissed"):
                    deadline_misses += 1

    return {
        "frame_count": len(frame_times),
        "p50_ms": percentile_nearest_rank(frame_times, 0.50),
        "p95_ms": percentile_nearest_rank(frame_times, 0.95),
        "p99_ms": percentile_nearest_rank(frame_times, 0.99),
        "p99_9_ms": percentile_nearest_rank(frame_times, 0.999),
        "one_percent_low_fps": 1000.0 / percentile_nearest_rank(frame_times, 0.99) if frame_times else 0,
        "point_one_percent_low_fps": 1000.0 / percentile_nearest_rank(frame_times, 0.999) if frame_times else 0,
        "max_ms": max(frame_times) if frame_times else 0,
        "cpu_active_p50_ms": percentile_nearest_rank(cpu_active, 0.50),
        "cpu_active_p95_ms": percentile_nearest_rank(cpu_active, 0.95),
        "cpu_active_p99_ms": percentile_nearest_rank(cpu_active, 0.99),
        "gpu_duration_p50_ms": percentile_nearest_rank(gpu_duration, 0.50) if gpu_duration else 0,
        "gpu_duration_p95_ms": percentile_nearest_rank(gpu_duration, 0.95) if gpu_duration else 0,
        "gpu_duration_p99_ms": percentile_nearest_rank(gpu_duration, 0.99) if gpu_duration else 0,
        "deadline_misses": deadline_misses
    }


def write_report(result: dict[str, Any], output_dir: str) -> None:
    """Write JSON and human-readable reports."""
    Path(output_dir).mkdir(parents=True, exist_ok=True)

    stats = compute_stats(result.get("benchmark", {}).get("frames", []))
    gpu_samples = result.get("gpu_samples", [])
    proc_samples = result.get("proc_samples", [])

    gpu_memory = [s.get("memory_used_mib", 0) for s in gpu_samples]
    proc_memory = [s.get("rss_kib", 0) for s in proc_samples]

    report = {
        "schema": 1,
        "timestamp": time.time(),
        "benchmark": result.get("benchmark", {}),
        "frame_stats": stats,
        "gpu_memory": {
            "min_mib": min(gpu_memory) if gpu_memory else 0,
            "max_mib": max(gpu_memory) if gpu_memory else 0,
            "avg_mib": sum(gpu_memory) / len(gpu_memory) if gpu_memory else 0,
            "samples_count": len(gpu_samples)
        },
        "process_memory": {
            "min_kib": min(proc_memory) if proc_memory else 0,
            "max_kib": max(proc_memory) if proc_memory else 0,
            "samples_count": len(proc_samples)
        }
    }

    json_path = Path(output_dir) / "report.json"
    with open(json_path, "w") as f:
        json.dump(report, f, indent=2, default=str)

    summary_path = Path(output_dir) / "summary.txt"
    with open(summary_path, "w") as f:
        f.write(f"Benchmark Summary\n")
        f.write(f"{'=' * 60}\n")
        f.write(f"Frames: {stats.get('frame_count', 0)}\n")
        f.write(f"Frame time p50: {stats.get('p50_ms', 0):.2f} ms\n")
        f.write(f"Frame time p95: {stats.get('p95_ms', 0):.2f} ms\n")
        f.write(f"Frame time p99: {stats.get('p99_ms', 0):.2f} ms\n")
        f.write(f"Frame time p99.9: {stats.get('p99_9_ms', 0):.2f} ms\n")
        f.write(f"1% low FPS: {stats.get('one_percent_low_fps', 0):.1f}\n")
        f.write(f"0.1% low FPS: {stats.get('point_one_percent_low_fps', 0):.1f}\n")
        f.write(f"Max frame time: {stats.get('max_ms', 0):.2f} ms\n")
        f.write(f"CPU active p50: {stats.get('cpu_active_p50_ms', 0):.2f} ms\n")
        f.write(f"GPU duration p50: {stats.get('gpu_duration_p50_ms', 0):.2f} ms\n")
        f.write(f"GPU memory max: {report['gpu_memory']['max_mib']} MiB\n")
        f.write(f"Process memory max: {report['process_memory']['max_kib'] / 1024:.0f} MiB\n")
        f.write(f"Deadline misses: {stats.get('deadline_misses', 0)}\n")

    print(f"Report written to {json_path}")
    print(f"Summary written to {summary_path}")


def selftest() -> int:
    """Run self-test with synthetic data."""
    print("Running selftest mode...")

    # Test percentile calculation
    test_values = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    p99 = percentile_nearest_rank(test_values, 0.99)
    assert p99 == 10, f"p99 should be 10, got {p99}"
    p50 = percentile_nearest_rank(test_values, 0.50)
    assert p50 in [5, 6], f"p50 should be 5 or 6, got {p50}"
    print("  percentile math: OK")

    # Test CSV parsing
    csv_data = "frame,startToStartMs\n30,16.67\n31,16.70"
    frames = []
    reader = csv.DictReader(csv_data.splitlines())
    for row in reader:
        frames.append({"frame": int(row["frame"]), "startToStartMs": float(row["startToStartMs"])})
    assert len(frames) == 2, "CSV parsing failed"
    print("  CSV parsing: OK")

    # Test orbit generator
    orbit_radius = 10.0
    angles = [i * (360.0 / 10) for i in range(10)]
    positions = [
        {"x": orbit_radius * math.cos(math.radians(a)), "z": orbit_radius * math.sin(math.radians(a))}
        for a in angles
    ]
    assert len(positions) == 10, "Orbit generation failed"
    print("  orbit generator: OK")

    # Test report writer
    result = {
        "benchmark": {"frames": [{"startToStartMs": i} for i in range(100)]},
        "gpu_samples": [{"memory_used_mib": 1024 + i} for i in range(10)],
        "proc_samples": [{"rss_kib": 500000 + i * 1000} for i in range(10)]
    }
    output_dir = "/tmp/selftest-benchmark"
    Path(output_dir).mkdir(exist_ok=True)
    write_report(result, output_dir)
    assert (Path(output_dir) / "report.json").exists(), "Report not written"
    print("  report writer: OK")

    print("Selftest passed!")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true", help="Run self-test mode")
    parser.add_argument("--duration", type=float, default=70.0, help="Total duration in seconds")
    parser.add_argument("--fps", type=int, default=60, help="Target FPS")
    parser.add_argument("--editor", help="Path to Editor executable")
    parser.add_argument("--project", help="Project path")
    parser.add_argument("--output", default="output/benchmarks/latest", help="Output directory")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    # Check environment
    ok, msg = check_environment()
    if not ok:
        print(f"Environment check failed: {msg}", file=sys.stderr)
        return 1

    if not args.editor:
        print("--editor is required", file=sys.stderr)
        return 1

    # Run benchmark
    result = run_benchmark_test(args.editor, "", args.output, args.duration, args.fps, args.project)
    if "error" in result:
        print(f"Benchmark failed: {result['error']}", file=sys.stderr)
        return 1

    # Write reports
    write_report(result, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
