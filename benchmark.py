#!/usr/bin/env python3
"""Collect single runs and/or averages of 10 identical runs of build/benchmark."""

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKLOADS = ("uniform", "mixed", "fragmented", "resize")
STRATEGIES = ("first", "best", "next")
METRICS = ("elapsed_ns", "throughput_calls_per_second", "checkpoints")


def run_once(build, workload, seed, steps, variant, strategy, library, size_scale=1):
    """Start a fresh process, select its allocator, and collect its JSON result."""
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    env.pop("ALLOC_STRATEGY", None)
    if library:
        env["LD_PRELOAD"] = str(build / library)
        env["ALLOC_STRATEGY"] = strategy

    command = [
        str(build / "benchmark"),
        workload,
        str(seed),
        str(steps),
        strategy,
        "--size-scale",
        str(size_scale),
    ]
    process = subprocess.run(command, env=env, capture_output=True, text=True)

    if process.returncode:
        raise ValueError(
            f"benchmark exited with {process.returncode}: {process.stderr.strip()}"
        )

    row = json.loads(process.stdout)
    expected = {
        "schema_version": 1,
        "workload": workload,
        "seed": seed,
        "steps": steps,
        "size_scale": size_scale,
        "variant": variant,
        "allocator": strategy,
    }
    if not isinstance(row, dict) or any(row.get(k) != v for k, v in expected.items()):
        raise ValueError("benchmark reported an unexpected workload or allocator")

    if type(row.get("checksum")) is not int:
        raise ValueError("benchmark did not report an integer checksum")

    return row


def mean_metrics(values):
    """Average numeric measurements recursively; unavailable measurements stay null."""
    first = values[0]
    if isinstance(first, dict):
        if any(not isinstance(v, dict) or v.keys() != first.keys() for v in values):
            raise ValueError("measurement fields differ between runs")
        return {key: mean_metrics([v[key] for v in values]) for key in first}
    if any(v is None for v in values):
        return None
    if not all(type(v) in (int, float) for v in values):
        raise ValueError("expected numeric measurements")
    return statistics.mean(values)


def average_results(rows):
    """Check identical trace data before averaging timing and memory measurements."""
    reference = rows[0]
    for index, row in enumerate(rows[1:], start=2):
        if row["checksum"] != reference["checksum"]:
            raise ValueError(f"checksum mismatch in repetition {index}")
        # Keep identifiers, counts, and the 64-bit checksum exact, not floating point.
        if row.keys() != reference.keys() or any(
            row[key] != value for key, value in reference.items() if key not in METRICS
        ):
            raise ValueError(f"workload data differs in repetition {index}")
    result = reference.copy()

    for key in METRICS:
        result[key] = mean_metrics([row[key] for row in rows])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("single", "average", "both"), default="both")
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--steps", type=int, default=100_000)
    parser.add_argument(
        "--size-scale",
        type=int,
        default=1,
        help="positive integer multiplier for all allocation sizes (default: 1)",
    )
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument(
        "--output", type=Path, default=ROOT / "benchmark_results/benchmarks.json"
    )
    args = parser.parse_args()

    if not sys.platform.startswith("linux"):
        parser.error(
            "run this script on Linux, or inside the project's Docker container"
        )

    if not 1 <= args.seed <= 4294967295 or args.steps < 1:
        parser.error("seed must be 1..4294967295 and steps must be positive")
    if args.size_scale < 1:
        parser.error("size scale must be positive")

    build = args.build_dir.resolve()
    for name in ("benchmark", "liballocator.so", "liballocator_with_stats.so"):
        if not (build / name).is_file():
            parser.error(f"missing {build / name}; run make first")

    configurations = [("glibc", "system", None)]
    for variant, library in (
        ("plain", "liballocator.so"),
        ("stats", "liballocator_with_stats.so"),
    ):
        configurations.extend((variant, strategy, library) for strategy in STRATEGIES)

    modes = ("single", "average") if args.mode == "both" else (args.mode,)
    results = {mode: [] for mode in modes}
    checksums = {}

    try:
        for mode in modes:
            repeats = 1 if mode == "single" else 10
            for workload in WORKLOADS:
                for variant, strategy, library in configurations:
                    label = f"{mode}: {workload}, {variant}, {strategy}"
                    print(f"{label} ({repeats} runs)", file=sys.stderr, flush=True)
                    rows = []
                    for repetition in range(1, repeats + 1):
                        row = run_once(
                            build,
                            workload,
                            args.seed,
                            args.steps,
                            variant,
                            strategy,
                            library,
                            args.size_scale,
                        )
                        # Identical work must also agree across libraries, strategies, and modes.
                        expected = checksums.setdefault(workload, row["checksum"])
                        if row["checksum"] != expected:
                            raise ValueError(
                                f"{label}, repetition {repetition}: checksum mismatch "
                                f"({row['checksum']} != {expected})"
                            )
                        rows.append(row)
                    result = rows[0] if mode == "single" else average_results(rows)
                    results[mode].append({"repetitions": repeats, "result": result})

        # Write the output only after the whole collection succeeds.
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2, allow_nan=False) + "\n")
    except (OSError, ValueError) as error:
        parser.exit(1, f"benchmark.py: {error}\n")

    print(f"Saved results to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
