#!/usr/bin/env python3
"""
run_all_rerun.py  --  Latency Scenario #4A (4.2.4)

Based on [6] (Carrascosa-Zamacois et al., PIMRC 2023).
Sweep: numLinks x offeredLoad x RngRun
numLinks=1 -> SLO (4 exclusive channels, no contention)
numLinks=2 -> STR2 (2 links per BSS, cross-pair sharing per Fig. 3.5b)
numLinks=4 -> STR4 (4 links per BSS, all BSSs share all channels)

Persistence: per-run CSV result files (resume after crash / add more runs).
Re-run: failed jobs retried sequentially after the parallel pass.
"""

# >>> thesis-style shim (auto-injected)
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), _os.pardir, "_common"))
import thesis_style as _thesis_style  # noqa: F401  installs unified plot style
# <<< thesis-style shim

import os
import re
import subprocess
from review_run_record import save_run_record
import sys
import csv
from concurrent.futures import ThreadPoolExecutor, as_completed
from itertools import product
from statistics import mean, stdev

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# ---------------------------------------------------------------------------
#  Configuration
# ---------------------------------------------------------------------------
# Direct binary path — avoids ./ns3 Python/cmake overhead (~3-5 s/job)
NS3_BINARY   = "build/scratch/ns3.45-fig15_load_under_contention-optimized"
OUTPUT_DIR   = "thr_runs_4_2_4_e2e"
PLOT_DIR     = "plots"
PLOT_FILE    = os.path.join(PLOT_DIR, "4_2_4_e2e.svg")

NUM_LINKS      = [1, 2, 4]           # 1=SLO, 2=STR2, 4=STR4
OFFERED_LOADS  = list(range(100, 2600, 100))  # 100 to 2500 Mb/s (25 points)
RUNS           = 10   # 10 seeds × Poisson = statistically sufficient for CIs
SIM_TIME       = 3.0  # 3 s — Poisson is stationary; still ~400k pkts per point
PAYLOAD        = 1500
NMDPUS         = 1024
STARTUP_GUARD  = 2.0  # exclude association/setup transients

MAX_WORKERS    = 11    # leave 1 core for OS on 12-core machine
RERUN_MISSING  = True

# Two-sided 95% Student-t critical values for df=1..30.
T95_CRITICAL = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447,
    7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179,
    13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101,
    19: 2.093, 20: 2.086, 21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064,
    25: 2.060, 26: 2.056, 27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042,
}

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(PLOT_DIR, exist_ok=True)

RE_LAT = re.compile(r"Mean DL Latency:\s*([0-9.]+)\s*ms")
RE_P50 = re.compile(r"DL Latency p50:\s*([0-9.]+)\s*ms")
RE_P95 = re.compile(r"DL Latency p95:\s*([0-9.]+)\s*ms")
RE_P99 = re.compile(r"DL Latency p99:\s*([0-9.]+)\s*ms")
RE_THR = re.compile(r"Mean DL Throughput:\s*([0-9.]+)\s*Mb/s")

MODE_LABELS = {1: "SLO (1 link)", 2: "STR2 (2 links)", 4: "STR4 (4 links)"}

# ---------------------------------------------------------------------------
#  Persistence helpers
# ---------------------------------------------------------------------------

def _result_path(nl: int, load: int, rng: int) -> str:
    return os.path.join(OUTPUT_DIR, f"L{nl}_Load{load}_rng{rng}_result.csv")


def _key(nl: int, load: int) -> tuple:
    return (nl, load)


def _parse_result_row(text: str):
    """Return (lat, p50, p95, p99, thr) or None. Handles 2, 4, and 5-field rows."""
    reader = csv.reader([text])
    row = next(reader, [])
    parts = [p.strip() for p in row if p.strip()]
    if len(parts) >= 5:
        return tuple(float(p) for p in parts[:5])
    if len(parts) >= 4:
        lat, p50, p99, thr = (float(p) for p in parts[:4])
        return lat, p50, p99, p99, thr  # p95 fallback
    if len(parts) >= 2:
        lat, thr = float(parts[0]), float(parts[1])
        return lat, lat, lat, lat, thr
    return None


# ---------------------------------------------------------------------------
#  Run a single job + parse output
# ---------------------------------------------------------------------------

def run_job(nl: int, load: int, rng: int, force_refresh: bool = False) -> tuple:
    """Run one simulation and return (nl, load, rng, lat, p50, p95, p99, thr) or Nones."""
    result_file = _result_path(nl, load, rng)
    if os.path.exists(result_file) and not force_refresh:
        try:
            with open(result_file) as f:
                parsed = _parse_result_row(f.read().strip())
            if parsed is not None:
                lat, p50, p95, p99, thr = parsed
                return (nl, load, rng, lat, p50, p95, p99, thr)
        except Exception:
            pass

    cmd = [
        NS3_BINARY,
        f"--simTime={SIM_TIME}",
        f"--payloadSize={PAYLOAD}",
        f"--nMpdus={NMDPUS}",
        f"--numLinks={nl}",
        f"--offeredLoad={load}",
        f"--startupGuard={STARTUP_GUARD}",
        f"--RngRun={rng}",
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        save_run_record(result_file, cmd, result)
        lat_m = RE_LAT.search(result.stdout)
        p50_m = RE_P50.search(result.stdout)
        p95_m = RE_P95.search(result.stdout)
        p99_m = RE_P99.search(result.stdout)
        thr_m = RE_THR.search(result.stdout)
        if lat_m and thr_m:
            lat = float(lat_m.group(1))
            p50 = float(p50_m.group(1)) if p50_m else lat
            p95 = float(p95_m.group(1)) if p95_m else lat
            p99 = float(p99_m.group(1)) if p99_m else lat
            thr = float(thr_m.group(1))
            os.makedirs(OUTPUT_DIR, exist_ok=True)
            with open(result_file, "w") as f:
                f.write(f"{lat},{p50},{p95},{p99},{thr}\n")
            return (nl, load, rng, lat, p50, p95, p99, thr)
        else:
            print(f"  PARSE-FAIL L{nl} Load{load} rng{rng}", file=sys.stderr)
            print(f"  stdout: {result.stdout[-400:]}", file=sys.stderr)
    except Exception as e:
        print(f"  ERROR L{nl} Load{load} rng{rng}: {e}", file=sys.stderr)

    return (nl, load, rng, None, None, None, None, None)


# ---------------------------------------------------------------------------
#  Load existing results
# ---------------------------------------------------------------------------

def load_existing_results():
    """Return dict keyed by (nl, load) -> [(rng, lat, p50, p95, p99, thr)]."""
    results = {}
    if not os.path.isdir(OUTPUT_DIR):
        return results
    for fname in os.listdir(OUTPUT_DIR):
        m = re.match(r"L(\d+)_Load(\d+)_rng(\d+)_result\.csv$", fname)
        if not m:
            continue
        nl, load, rng = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if nl not in NUM_LINKS:
            continue
        fpath = os.path.join(OUTPUT_DIR, fname)
        try:
            with open(fpath) as f:
                parsed = _parse_result_row(f.read().strip())
            if parsed is None:
                continue
            lat, p50, p95, p99, thr = parsed
            key = _key(nl, load)
            results.setdefault(key, [])
            results[key].append((rng, lat, p50, p95, p99, thr))
        except Exception:
            pass
    return results


# ---------------------------------------------------------------------------
#  Plot
# ---------------------------------------------------------------------------

def ci95(values):
    if len(values) <= 1:
        return 0.0
    n = len(values)
    t = T95_CRITICAL.get(n - 1, 1.96)
    return t * stdev(values) / (n ** 0.5)


def plot_results(raw: dict) -> None:
    """Line plots: mean latency (+ p99) and throughput vs offered load per numLinks."""
    colors = {1: "#440154", 2: "#355f8d", 4: "#22a884"}
    markers = {1: "o", 2: "s", 4: "^"}

    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle("Latency Scenario #4A — DL Latency & Throughput vs. Offered Load")

    for nl in NUM_LINKS:
        xs, lat_m, lat_e, p99_m, p99_e, thr_m, thr_e = [], [], [], [], [], [], []
        for load in OFFERED_LOADS:
            rows = raw.get(_key(nl, load), [])
            if not rows:
                continue
            lats = [r[1] for r in rows]
            p99s = [r[4] for r in rows]
            thrs = [r[5] for r in rows]
            xs.append(load)
            lat_m.append(mean(lats));   lat_e.append(ci95(lats))
            p99_m.append(mean(p99s));   p99_e.append(ci95(p99s))
            thr_m.append(mean(thrs));   thr_e.append(ci95(thrs))

        if not xs:
            continue

        c, mk = colors[nl], markers[nl]
        axes[0].errorbar(xs, lat_m, yerr=lat_e, label=f"{MODE_LABELS[nl]} mean",
                         color=c, marker=mk, linestyle="-", markersize=4,
                         capsize=3, linewidth=1.6)
        axes[0].errorbar(xs, p99_m, yerr=p99_e, label=f"{MODE_LABELS[nl]} p99",
                         color=c, marker=mk, linestyle="dotted",
                         markersize=4, capsize=3, linewidth=1.2)
        axes[1].errorbar(xs, thr_m, yerr=thr_e, label=MODE_LABELS[nl],
                         color=c, marker=mk, linestyle="-", markersize=4,
                         capsize=3, linewidth=1.6)

    for ax, ylabel in zip(axes, ["End-to-end delay [ms]", "Aggregate Throughput [Mbit/s]"]):
        ax.set_xlabel("Total Offered Load [Mbit/s]")
        ax.set_ylabel(ylabel)
        ax.legend(fontsize=8)
        ax.grid(True, linestyle="--", alpha=0.5)
        ax.set_xlim(left=0)

    plt.tight_layout()
    plt.savefig(PLOT_FILE, format="svg")
    print(f"\n[plot] saved -> {PLOT_FILE}", flush=True)
    plt.close(fig)


# ---------------------------------------------------------------------------
#  Main sweep
# ---------------------------------------------------------------------------

def main():
    print("[*] Loading existing results...", flush=True)
    existing = load_existing_results()
    loaded = sum(len(v) for v in existing.values())
    print(f"[*] Found {loaded} existing run results", flush=True)

    print(f"[*] Scheduling jobs: numLinks={NUM_LINKS}, "
          f"loads={len(OFFERED_LOADS)} points, {RUNS} runs each", flush=True)

    jobs = list(product(NUM_LINKS, OFFERED_LOADS, range(1, RUNS + 1)))
    print(f"[*] Total jobs: {len(jobs)}", flush=True)

    failed = []
    completed = 0

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_job, nl, load, rng): (nl, load, rng)
                   for nl, load, rng in jobs}
        for future in as_completed(futures):
            completed += 1
            nl, load, rng, lat, p50, p95, p99, thr = future.result()
            if lat is None:
                failed.append((nl, load, rng))
            if completed % 50 == 0:
                print(f"  [{completed}/{len(jobs)}] completed", flush=True)

    print(f"[*] Parallel pass complete: {len(jobs) - len(failed)}/{len(jobs)} successful",
          flush=True)

    if RERUN_MISSING and failed:
        print(f"[*] Re-running {len(failed)} failed jobs sequentially...", flush=True)
        for i, (nl, load, rng) in enumerate(failed):
            run_job(nl, load, rng)
            if (i + 1) % 10 == 0:
                print(f"  [{i + 1}/{len(failed)}] re-run complete", flush=True)

    raw = load_existing_results()
    plot_results(raw)

    print("[*] All jobs complete!", flush=True)


if __name__ == "__main__":
    main()
