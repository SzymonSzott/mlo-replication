#!/usr/bin/env python3
"""
run_all_rerun.py  —  Latency Scenario #3 (4.2.3)
Sweep: numLinks × MLO mode (STR/EMLSR) × offeredLoad × RngRun
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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "_common"))
import joanna_plots as jp  # noqa: E402  shared plotter (parser-only pattern)
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

# ---------------------------------------------------------------------------
#  Configuration
# ---------------------------------------------------------------------------
NS3_BINARY   = "build/scratch/ns3.45-fig13_latency_modes-optimized"
OUTPUT_DIR   = "thr_runs_4_2_3_e2e"
PLOT_DIR     = "plots"
PLOT_FILE    = os.path.join(PLOT_DIR, "4_2_3_e2e.svg")

NUM_LINKS      = [1, 2, 3]
MLO_MODES      = ["STR", "EMLSR"]         # EMLSR is valid for numLinks >= 2
OFFERED_LOADS  = list(range(100, 2600, 100))   # 100 … 2500 Mb/s  (25 points)
RUNS           = 10
SIM_TIME       = 5.0
PAYLOAD        = 1500
NMDPUS         = 1024

MAX_WORKERS    = 11
RERUN_MISSING  = True
NORMALIZED_FRACTIONS = [0.1, 0.3, 0.5, 0.7, 0.9]
PLOT_RNG_RUNS = list(range(1, RUNS + 1))

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
RE_P1 = re.compile(r"DL Latency p1:\s*([0-9.]+)\s*ms")
RE_P99 = re.compile(r"DL Latency p99:\s*([0-9.]+)\s*ms")
RE_THR = re.compile(r"Mean DL Throughput:\s*([0-9.]+)\s*Mb/s")

# ---------------------------------------------------------------------------
#  Persistence helpers
# ---------------------------------------------------------------------------

def _result_path(nl: int, mode: str, load: int, rng: int) -> str:
    fname = f"L{nl}_M{mode}_Load{load}_rng{rng}_result.csv"
    return os.path.join(OUTPUT_DIR, fname)

def _key(nl: int, mode: str, load: int) -> tuple:
    """Result cache key."""
    return (nl, mode, load)

def _mode_to_bool(mode: str) -> bool:
    """Convert mode string to emlsr boolean."""
    return mode == "EMLSR"


def _is_valid_combo(nl: int, mode: str) -> bool:
    """EMLSR requires multi-link operation."""
    return not (mode == "EMLSR" and nl < 2)


def _parse_result_row(text: str, require_p99: bool = False):
    """Parse a result CSV row from either the legacy or current format.

    Legacy files contain ``latency,throughput``.
    Current files contain ``latency,p1,p99,throughput``.
    """
    reader = csv.reader([text])
    row = next(reader, [])
    parts = [item.strip() for item in row if item.strip()]
    if len(parts) >= 4:
        lat, p1, p99, thr = (float(parts[0]), float(parts[1]), float(parts[2]), float(parts[3]))
        return lat, p1, p99, thr
    if len(parts) >= 3:
        lat, p99, thr = float(parts[0]), float(parts[1]), float(parts[2])
        return lat, lat, p99, thr
    if len(parts) >= 2:
        if require_p99:
            return None
        lat, thr = float(parts[0]), float(parts[1])
        return lat, lat, lat, thr
    return None


PLOT_LOADS = [300, 800, 1300, 1800, 2300]

# ---------------------------------------------------------------------------
#  Run a single job + parse output
# ---------------------------------------------------------------------------

def run_job(nl: int, mode: str, load: int, rng: int, force_refresh: bool = False) -> tuple:
    """
    Run: ./ns3 run "scratch/fig13_latency_modes --numLinks=nl --emlsr=bool \
                                  --offeredLoad=load --RngRun=rng"
    Parse output for latency and throughput.
    Returns: (nl, mode, load, rng, lat_ms, p1_ms, p99_ms, thr_mbps) or
    (nl, mode, load, rng, None, None, None, None) on failure.
    """
    result_file = _result_path(nl, mode, load, rng)
    if os.path.exists(result_file) and not force_refresh:
        try:
            with open(result_file) as f:
                parsed = _parse_result_row(f.read().strip())
            if parsed is not None:
                lat, p1, p99, thr = parsed
                return (nl, mode, load, rng, lat, p1, p99, thr)
        except Exception:
            pass

    emlsr_bool = _mode_to_bool(mode)
    cmd = [
        NS3_BINARY,
        f"--simTime={SIM_TIME}",
        f"--payloadSize={PAYLOAD}",
        f"--nMpdus={NMDPUS}",
        f"--numLinks={nl}",
        f"--emlsr={str(emlsr_bool).lower()}",
        f"--offeredLoad={load}",
        f"--RngRun={rng}",
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        save_run_record(result_file, cmd, result)
        lat_m = RE_LAT.search(result.stdout)
        p1_m = RE_P1.search(result.stdout)
        p99_m = RE_P99.search(result.stdout)
        thr_m = RE_THR.search(result.stdout)
        if lat_m and p1_m and p99_m and thr_m:
            lat = float(lat_m.group(1))
            p1 = float(p1_m.group(1))
            p99 = float(p99_m.group(1))
            thr = float(thr_m.group(1))
            # Persist result
            os.makedirs(OUTPUT_DIR, exist_ok=True)
            with open(result_file, 'w') as f:
                f.write(f"{lat},{p1},{p99},{thr}\n")
            return (nl, mode, load, rng, lat, p1, p99, thr)
    except Exception as e:
        print(f"  ERROR L{nl} M{mode} Load{load} rng{rng}: {e}", file=sys.stderr)

    return (nl, mode, load, rng, None, None, None, None)

# ---------------------------------------------------------------------------
#  Load existing results
# ---------------------------------------------------------------------------

def load_existing_results(require_p99: bool = False):
    """Load existing result files into a dict keyed by (nl, mode, load).

    If *require_p99* is true, skip legacy two-field CSVs and keep only files
    that already contain the packet-level percentile columns.
    """
    results = {}
    if not os.path.isdir(OUTPUT_DIR):
        return results

    for fname in os.listdir(OUTPUT_DIR):
        m = re.match(r"L(\d+)_M(\w+)_Load(\d+)_rng(\d+)_result\.csv$", fname)
        if not m:
            continue
        nl, mode, load, rng = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))
        key = _key(nl, mode, load)
        fpath = os.path.join(OUTPUT_DIR, fname)
        try:
            with open(fpath) as f:
                parsed = _parse_result_row(f.read().strip(), require_p99=require_p99)
                if parsed is None:
                    continue
                lat, p1, p99, thr = parsed
            if key not in results:
                results[key] = []
            results[key].append((rng, lat, p1, p99, thr))
        except Exception:
            pass

    return results


def refresh_plot_data():
    """Refresh only the loads used by the plot, selected per configuration."""
    print("[*] Refreshing plot loads with packet-level percentile data...", flush=True)
    base_results = load_existing_results(require_p99=False)
    selected_loads = _select_plot_loads(base_results)
    jobs = []
    for nl in NUM_LINKS:
        for mode in MLO_MODES:
            if not _is_valid_combo(nl, mode):
                continue
            for load in selected_loads.get((nl, mode), []):
                for rng in PLOT_RNG_RUNS:
                    jobs.append((nl, mode, load, rng))

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = [executor.submit(run_job, nl, mode, load, rng, True) for nl, mode, load, rng in jobs]
        for future in as_completed(futures):
            future.result()


def _select_plot_loads(raw):
    """Pick representative offered-load points for each configuration.

    The selection is based on each configuration's own throughput scale so the
    resulting plot compares comparable operating regions.
    """
    selected = {}
    for nl, mode in [(1, "STR"), (2, "EMLSR"), (2, "STR"), (3, "EMLSR"), (3, "STR")]:
        if not _is_valid_combo(nl, mode):
            continue

        load_to_thr = {}
        for (key_nl, key_mode, load), rows in raw.items():
            if key_nl != nl or key_mode != mode or not rows:
                continue
            load_to_thr[load] = mean(row[4] for row in rows)

        if not load_to_thr:
            continue

        capacity = max(load_to_thr.values())
        chosen = []
        used = set()
        for fraction in NORMALIZED_FRACTIONS:
            target = capacity * fraction
            best_load = min(
                load_to_thr,
                key=lambda load: (abs(load_to_thr[load] - target), load),
            )
            if best_load in used:
                remaining = [load for load in load_to_thr if load not in used]
                if remaining:
                    best_load = min(
                        remaining,
                        key=lambda load: (abs(load_to_thr[load] - target), load),
                    )
            used.add(best_load)
            chosen.append(best_load)

        selected[(nl, mode)] = chosen

    return selected


def _save_pdf_svg(fig, name):
    """save_fn callback: emit both .pdf and .svg into PLOT_DIR/<name>."""
    base = os.path.join(PLOT_DIR, name)
    fig.savefig(base + ".pdf", format="pdf")
    fig.savefig(base + ".svg", format="svg")
    print(f"[plot] saved → {base}.{{pdf,svg}}", flush=True)
    plt.close(fig)


def _joanna_df(raw):
    """Convert raw[(nl, mode, load)] into the DataFrame schema
    jp.plot_latency_bars expects: columns isSTR, numLinks, expectedLoad,
    nin_latency_mean, nin_latency_std, avg_latency_mean."""
    from _robust import ci_hw_robust

    selected_loads = _select_plot_loads(raw)
    mode_to_isstr = {"STR": 1, "EMLSR": 0}
    rows = []
    for (nl, mode), chosen in selected_loads.items():
        for fraction, load in zip(NORMALIZED_FRACTIONS, chosen):
            key = (nl, mode, load)
            if key not in raw or not raw[key]:
                continue
            mean_samples = [row[1] for row in raw[key]]
            p99_samples  = [row[3] for row in raw[key]]
            _, p99_ci = ci_hw_robust(p99_samples, confidence=0.95)
            rows.append({
                "isSTR":            mode_to_isstr[mode],
                "numLinks":         nl,
                "expectedLoad":     fraction,
                "nin_latency_mean": mean(p99_samples),
                "nin_latency_std":  p99_ci,
                "avg_latency_mean": mean(mean_samples),
            })
    return pd.DataFrame(rows)


def plot_results(raw):
    """Render 4_2_3_e2e via jp.plot_latency_bars (MATLAB-identical style)."""
    df = _joanna_df(raw)
    if df.empty:
        print("[plot] no data to plot", flush=True)
        return
    name = os.path.splitext(os.path.basename(PLOT_FILE))[0]
    jp.plot_latency_bars(df, name, _save_pdf_svg, ylabel="End-to-end delay [ms]")


# ---------------------------------------------------------------------------
#  Main sweep
# ---------------------------------------------------------------------------

def main():
    print("[*] Loading existing results...", flush=True)
    existing = load_existing_results(require_p99=True)

    print(f"[*] Scheduling jobs: numLinks={NUM_LINKS}, modes={MLO_MODES}, "
          f"loads={len(OFFERED_LOADS)} points, {RUNS} runs each", flush=True)

    # Generate all jobs
    jobs = []
    for nl, mode, load, rng in product(NUM_LINKS, MLO_MODES, OFFERED_LOADS, range(1, RUNS + 1)):
        if _is_valid_combo(nl, mode):
            jobs.append((nl, mode, load, rng))

    print(f"[*] Total jobs: {len(jobs)}", flush=True)

    # Run in parallel
    completed = 0
    failed = []
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_job, nl, mode, load, rng): (nl, mode, load, rng)
                   for nl, mode, load, rng in jobs}
        for future in as_completed(futures):
            completed += 1
            nl, mode, load, rng, lat, p1, p99, thr = future.result()
            if lat is None or p1 is None or p99 is None or thr is None:
                failed.append((nl, mode, load, rng))
            if completed % 50 == 0:
                print(f"  [{completed}/{len(jobs)}] completed", flush=True)

    print(f"[*] Parallel pass complete: {len(jobs) - len(failed)}/{len(jobs)} successful", 
          flush=True)

    # Retry failed jobs
    if RERUN_MISSING and failed:
        print(f"[*] Re-running {len(failed)} failed jobs sequentially...", flush=True)
        for i, (nl, mode, load, rng) in enumerate(failed):
            run_job(nl, mode, load, rng)
            if (i + 1) % 10 == 0:
                print(f"  [{i + 1}/{len(failed)}] re-run complete", flush=True)

    refresh_plot_data()
    raw = load_existing_results(require_p99=True)
    plot_results(raw)

    print("[*] All jobs complete!", flush=True)

if __name__ == "__main__":
    main()
