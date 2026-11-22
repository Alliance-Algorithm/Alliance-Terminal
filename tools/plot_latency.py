#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot per-sample RMCS -> Alliance-Terminal one-way latency CSV."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError as error:
    raise SystemExit(
        "matplotlib is required; install it with:\n"
        "  python3 -m pip install --user matplotlib"
    ) from error


def percentile(sorted_values: list[float], ratio: float) -> float:
    if not sorted_values:
        return 0.0
    position = (len(sorted_values) - 1) * ratio
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return sorted_values[lower]
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def load_rows(path: Path, include_warmup: bool) -> list[dict[str, int]]:
    rows: list[dict[str, int]] = []
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        required = {"latency_ns", "t_recv_ns", "seq"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise SystemExit(f"{path}: missing columns {sorted(required)}")

        for line_number, row in enumerate(reader, start=2):
            try:
                warmup = int(row.get("warmup", "0") or "0")
                if not include_warmup and warmup:
                    continue
                rows.append(
                    {
                        "seq": int(row["seq"]),
                        "t_recv_ns": int(row["t_recv_ns"]),
                        "latency_ns": int(row["latency_ns"]),
                        "warmup": warmup,
                    }
                )
            except (KeyError, TypeError, ValueError) as error:
                raise SystemExit(f"{path}:{line_number}: invalid row: {error}") from error

    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path, help="CSV written by ipc-probe --record")
    parser.add_argument(
        "--outdir",
        type=Path,
        default=None,
        help="output directory (default: CSV directory)",
    )
    parser.add_argument(
        "--include-warmup",
        action="store_true",
        help="include warmup samples in plots and statistics",
    )
    parser.add_argument("--title", default="RMCS -> Alliance-Terminal one-way latency")
    args = parser.parse_args()

    rows = load_rows(args.csv, args.include_warmup)
    if not rows:
        raise SystemExit(f"{args.csv}: no samples to plot")

    first_recv = rows[0]["t_recv_ns"]
    times_s = [(row["t_recv_ns"] - first_recv) / 1e9 for row in rows]
    latencies_ms = [row["latency_ns"] / 1e6 for row in rows]
    ordered = sorted(latencies_ms)
    negative = sum(1 for value in latencies_ms if value < 0)

    p50 = percentile(ordered, 0.50)
    p95 = percentile(ordered, 0.95)
    p99 = percentile(ordered, 0.99)
    p999 = percentile(ordered, 0.999)
    minimum = ordered[0]
    maximum = ordered[-1]

    outdir = args.outdir or args.csv.parent
    outdir.mkdir(parents=True, exist_ok=True)
    prefix = args.csv.stem

    summary_lines = [
        f"csv={args.csv}",
        f"samples={len(rows)}",
        f"warmup_included={args.include_warmup}",
        f"negative={negative}",
        f"min_ms={minimum:.6f}",
        f"p50_ms={p50:.6f}",
        f"p95_ms={p95:.6f}",
        f"p99_ms={p99:.6f}",
        f"p99.9_ms={p999:.6f}",
        f"max_ms={maximum:.6f}",
    ]
    summary_path = outdir / f"{prefix}_summary.txt"
    summary_path.write_text("\n".join(summary_lines) + "\n")
    print("\n".join(summary_lines))

    fig, axis = plt.subplots(figsize=(14, 4))
    axis.plot(times_s, latencies_ms, linewidth=0.5, color="#1f77b4")
    axis.axhline(p99, color="#d62728", linestyle="--", linewidth=0.8, label=f"p99={p99:.3f} ms")
    axis.set_xlabel("elapsed time (s)")
    axis.set_ylabel("latency (ms)")
    axis.set_title(f"{args.title}: per-sample")
    axis.grid(True, alpha=0.3)
    axis.legend(loc="upper right")
    fig.tight_layout()
    timeseries_path = outdir / f"{prefix}_timeseries.png"
    fig.savefig(timeseries_path, dpi=160)
    plt.close(fig)

    fig, axis = plt.subplots(figsize=(8, 4))
    axis.hist(latencies_ms, bins=200, color="#1f77b4", alpha=0.85)
    axis.axvline(p50, color="#2ca02c", linestyle="--", linewidth=1, label=f"p50={p50:.3f} ms")
    axis.axvline(p99, color="#d62728", linestyle="--", linewidth=1, label=f"p99={p99:.3f} ms")
    axis.set_xlabel("latency (ms)")
    axis.set_ylabel("samples")
    axis.set_title(f"{args.title}: distribution")
    axis.grid(True, alpha=0.3)
    axis.legend()
    fig.tight_layout()
    histogram_path = outdir / f"{prefix}_histogram.png"
    fig.savefig(histogram_path, dpi=160)
    plt.close(fig)

    buckets: dict[int, list[float]] = {}
    for elapsed, latency in zip(times_s, latencies_ms, strict=True):
        buckets.setdefault(int(elapsed), []).append(latency)

    seconds = sorted(buckets)
    q50: list[float] = []
    q95: list[float] = []
    q99: list[float] = []
    for second in seconds:
        values = sorted(buckets[second])
        q50.append(percentile(values, 0.50))
        q95.append(percentile(values, 0.95))
        q99.append(percentile(values, 0.99))

    fig, axis = plt.subplots(figsize=(14, 4))
    axis.plot(seconds, q50, label="p50", linewidth=1)
    axis.plot(seconds, q95, label="p95", linewidth=1)
    axis.plot(seconds, q99, label="p99", linewidth=1)
    axis.set_xlabel("elapsed second")
    axis.set_ylabel("latency (ms)")
    axis.set_title(f"{args.title}: per-second percentiles")
    axis.grid(True, alpha=0.3)
    axis.legend()
    fig.tight_layout()
    percentiles_path = outdir / f"{prefix}_percentiles.png"
    fig.savefig(percentiles_path, dpi=160)
    plt.close(fig)

    print(f"wrote {timeseries_path}")
    print(f"wrote {histogram_path}")
    print(f"wrote {percentiles_path}")
    print(f"wrote {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
