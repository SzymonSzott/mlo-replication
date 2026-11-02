#!/usr/bin/env python3
"""
run_all_rerun.py  --  Latency Scenario #4B (4.2.5)

Extends Scenario #4A with additional channel configurations.
Five modes covering different link-sharing topologies (Fig. 3.6):

  SLO    – 1 link/BSS, exclusive channels, no contention
  STR2   – 2 links/BSS, cross-pair sharing (same as Scenario #4A, Fig. 3.5b)
  EMLSR2 – same topology as STR2 with EMLSR activated
  STR1P1 – 2 links/BSS: one shared by all BSSs + one exclusive per BSS (Fig. 3.6a)
  STR5   – 5 links/BSS, all 4 BSSs share all 5 channels (Fig. 3.6b)

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
import pandas as pd

import joanna_plots  # noqa: E402  shared matplotlib plotter (scratch/_common)

# ---------------------------------------------------------------------------
#  Configuration
# ---------------------------------------------------------------------------
NS3_BINARY    = "build/scratch/ns3.45-fig17_link_assignment-optimized"
OUTPUT_DIR    = "thr_runs_4_2_5_e2e"
PLOT_DIR      = "plots"
PLOT_FILE     = os.path.join(PLOT_DIR, "4_2_5_e2e.svg")

STR_MODES      = ["SLO", "STR2", "EMLSR2", "STR1P1", "STR5"]
OFFERED_LOADS  = list(range(100, 2600, 100))  # 100 to 2500 Mb/s (25 points)
RUNS           = 10
SIM_TIME       = 3.0
PAYLOAD        = 1500
NMDPUS         = 1024
STARTUP_GUARD  = 2.0

MAX_WORKERS    = 11
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

MODE_LABELS = {
    "SLO":    "SLO (1 link)",
    "STR2":   "MLO-STR:2 (2 links)",
    "EMLSR2": "MLO-EMLSR:2 (2 links)",
    "STR1P1": "MLO-STR:1+1 (2 links)",
    "STR5":   "MLO-STR:5 (5 links)",
}

# ---------------------------------------------------------------------------
#  Persistence helpers
# ---------------------------------------------------------------------------

def _result_path(mode: str, load: int, rng: int) -> str:
    return os.path.join(OUTPUT_DIR, f"M{mode}_Load{load}_rng{rng}_result.csv")


def _key(mode: str, load: int) -> tuple:
    return (mode, load)


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

def run_job(mode: str, load: int, rng: int, force_refresh: bool = False) -> tuple:
    """Run one simulation and return (mode, load, rng, lat, p50, p95, p99, thr) or Nones."""
    result_file = _result_path(mode, load, rng)
    if os.path.exists(result_file) and not force_refresh:
        try:
            with open(result_file) as f:
                parsed = _parse_result_row(f.read().strip())
            if parsed is not None:
                lat, p50, p95, p99, thr = parsed
                return (mode, load, rng, lat, p50, p95, p99, thr)
        except Exception:
            pass

    cmd = [
        NS3_BINARY,
        f"--simTime={SIM_TIME}",
        f"--payloadSize={PAYLOAD}",
        f"--nMpdus={NMDPUS}",
        f"--strMode={mode}",
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
            return (mode, load, rng, lat, p50, p95, p99, thr)
        else:
            print(f"  PARSE-FAIL M{mode} Load{load} rng{rng}", file=sys.stderr)
            print(f"  stdout: {result.stdout[-400:]}", file=sys.stderr)
    except Exception as e:
        print(f"  ERROR M{mode} Load{load} rng{rng}: {e}", file=sys.stderr)

    return (mode, load, rng, None, None, None, None, None)


# ---------------------------------------------------------------------------
#  Load existing results
# ---------------------------------------------------------------------------

def load_existing_results():
    """Return dict keyed by (mode, load) -> [(rng, lat, p50, p95, p99, thr)]."""
    results = {}
    if not os.path.isdir(OUTPUT_DIR):
        return results
    for fname in os.listdir(OUTPUT_DIR):
        m = re.match(r"M(\w+)_Load(\d+)_rng(\d+)_result\.csv$", fname)
        if not m:
            continue
        mode, load, rng = m.group(1), int(m.group(2)), int(m.group(3))
        if mode not in STR_MODES:
            continue
        fpath = os.path.join(OUTPUT_DIR, fname)
        try:
            with open(fpath) as f:
                parsed = _parse_result_row(f.read().strip())
            if parsed is None:
                continue
            lat, p50, p95, p99, thr = parsed
            key = _key(mode, load)
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
    """Praca-style grouped bar chart: x = total traffic load [Mbit/s],
    bars per group = (mode, percentile). p50 darker, p99 lighter — matches
    Fig. 4.11. Rendering is delegated to joanna_plots.plot_10293865_5, the
    same function ieee-plot-test uses for the MATLAB side.
    """
    # Praca Fig. 4.11 only shows three load points: 100 / 1000 / 2500 Mbps.
    bar_loads = [l for l in (100, 1000, 2500) if l in OFFERED_LOADS]

    # isSTR/allChannelsShared/isNew encoding matches joanna_plots.
    # plot_10293865_5's `series` filters.
    MODE_FLAGS = {
        "SLO":    dict(isSTR=0, allChannelsShared=1, isNew=0),
        "STR2":   dict(isSTR=1, allChannelsShared=0, isNew=0),
        "EMLSR2": dict(isSTR=0, allChannelsShared=0, isNew=0),
        "STR1P1": dict(isSTR=1, allChannelsShared=0, isNew=1),
        "STR5":   dict(isSTR=1, allChannelsShared=1, isNew=0),
    }

    rows = []
    for mode in STR_MODES:
        for load in bar_loads:
            data_rows = raw.get(_key(mode, load), [])
            if not data_rows:
                continue
            p50s = [r[2] for r in data_rows]
            p99s = [r[4] for r in data_rows]
            rows.append({
                **MODE_FLAGS[mode], "expectedThr": load,
                "fiv_latency_mean": mean(p50s), "nin_latency_mean": mean(p99s),
                "nin_latency_std": ci95(p99s),
            })
    df = pd.DataFrame(rows)

    def _save(fig, _name):
        fig.savefig(PLOT_FILE, format="svg")
        print(f"\n[plot] saved -> {PLOT_FILE}", flush=True)

    joanna_plots.plot_10293865_5(
        df, "4_2_5_e2e", _save,
        ylabel="End-to-end delay [ms]",
    )


# ---------------------------------------------------------------------------
#  Main sweep
# ---------------------------------------------------------------------------

def main():
    print("[*] Loading existing results...", flush=True)
    existing = load_existing_results()
    loaded = sum(len(v) for v in existing.values())
    print(f"[*] Found {loaded} existing run results", flush=True)

    print(f"[*] Scheduling jobs: modes={STR_MODES}, "
          f"loads={len(OFFERED_LOADS)} points, {RUNS} runs each", flush=True)

    jobs = list(product(STR_MODES, OFFERED_LOADS, range(1, RUNS + 1)))
    print(f"[*] Total jobs: {len(jobs)}", flush=True)

    failed = []
    completed = 0

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_job, mode, load, rng): (mode, load, rng)
                   for mode, load, rng in jobs}
        for future in as_completed(futures):
            completed += 1
            mode, load, rng, lat, p50, p95, p99, thr = future.result()
            if lat is None:
                failed.append((mode, load, rng))
            if completed % 50 == 0:
                print(f"  [{completed}/{len(jobs)}] completed", flush=True)

    print(f"[*] Parallel pass complete: {len(jobs) - len(failed)}/{len(jobs)} successful",
          flush=True)

    if RERUN_MISSING and failed:
        print(f"[*] Re-running {len(failed)} failed jobs sequentially...", flush=True)
        for i, (mode, load, rng) in enumerate(failed):
            run_job(mode, load, rng)
            if (i + 1) % 10 == 0:
                print(f"  [{i + 1}/{len(failed)}] re-run complete", flush=True)

    raw = load_existing_results()
    plot_results(raw)

    print("[*] All jobs complete!", flush=True)


if __name__ == "__main__":
    main()
