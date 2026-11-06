"""
Runner + plotter for scratch/fig07_obss_activity
Throughput Scenario #3: main BSS throughput vs. OBSS station count (0-10).
Modes: SLO, STR, EMLSR  |  10 runs each  |  3 x 11 x 10 = 330 simulations.

Extra features vs. run_all.py:
  - RERUN_MISSING: after the parallel sweep, any failed job is retried once
    more (sequentially, so any transient resource contention is reduced).
  - The plot is always produced at the end, even when some runs are absent
    (missing data-points get mean=0 / hw=0 so the chart is still readable).
"""

# >>> thesis-style shim (auto-injected)
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), _os.pardir, "_common"))
import thesis_style as _thesis_style  # noqa: F401  installs unified plot style
# <<< thesis-style shim
import subprocess
import re
import os
import numpy as np
import matplotlib.pyplot as plt
from concurrent.futures import ThreadPoolExecutor, as_completed
from scipy import stats

# ===== CONFIG =====
NS3_PROGRAM   = "scratch/fig07_obss_activity"
SIMTIME       = 10.0
PAYLOAD       = 1500
NMDPUS        = 1024
RUNS          = 10
RNG_START     = 1001
CI            = 0.95
MAX_WORKERS   = os.cpu_count() or 8
OUTPUT_DIR    = "thr_runs_4_1_4"
PLOT_DIR      = "plots"
PLOT_FILE     = os.path.join(PLOT_DIR, "4_1_4.svg")
MAX_RETRIES   = 3     # retries inside run_one() on transient error
RERUN_MISSING = True  # after the full sweep, retry every job that failed

MODES       = ["SLO", "STR", "EMLSR"]
DIRECTIONS  = ["DL", "UL"]
OBSS_COUNTS = list(range(11))   # 0 .. 10

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(PLOT_DIR, exist_ok=True)

RE_DL = re.compile(r"MainBSS DL MacRx:\s*([0-9.]+)\s*Mb/s")
RE_UL = re.compile(r"MainBSS UL MacRx:\s*([0-9.]+)\s*Mb/s")


# ---------------------------------------------------------------------------
# Per-run result persistence helpers — one file per (mode, direction, n, rng)

def _result_path(mode, direction, n_obss, rng_run):
    return os.path.join(OUTPUT_DIR,
                        f"{mode}_{direction}_n{n_obss}_rng{rng_run}_result.csv")


def save_result(mode, direction, n_obss, rng_run, thr):
    with open(_result_path(mode, direction, n_obss, rng_run), "w") as f:
        f.write(f"{thr}\n")


def load_existing_results(raw):
    """Populate raw[mode][direction][n] from saved files.
    Returns set of (mode, direction, n, rng) tuples already completed."""
    done_set = set()
    pattern  = re.compile(
        r"^([A-Za-z]+)_(DL|UL)_n(\d+)_rng(\d+)_result\.csv$"
    )
    if not os.path.isdir(OUTPUT_DIR):
        return done_set
    for fname in os.listdir(OUTPUT_DIR):
        m = pattern.match(fname)
        if not m:
            continue
        mode_s, dir_s, n_s, rr_s = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        if mode_s not in MODES or n_s not in OBSS_COUNTS:
            continue
        try:
            with open(os.path.join(OUTPUT_DIR, fname)) as f:
                thr = float(f.read().strip())
            raw[mode_s][dir_s][n_s].append(thr)
            done_set.add((mode_s, dir_s, n_s, rr_s))
        except Exception:
            pass
    return done_set


# ---------------------------------------------------------------------------

def _run_once(mode, direction, n_obss, rng_run):
    """Single attempt: return throughput in Mb/s for the requested direction."""
    args = (
        f"{NS3_PROGRAM} "
        f"--RngRun={rng_run} "
        f"--simTime={SIMTIME} "
        f"--payloadSize={PAYLOAD} "
        f"--nMpdus={NMDPUS} "
        f"--mloMode={mode} "
        f"--direction={direction} "
        f"--nObss={n_obss}"
    )
    cmd = ["./ns3", "run", args]
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        log = os.path.join(OUTPUT_DIR,
                           f"{mode}_{direction}_n{n_obss}_rng{rng_run}_err.txt")
        with open(log, "w") as f:
            f.write("CMD:\n" + " ".join(cmd)
                    + "\n\nSTDOUT:\n" + result.stdout
                    + "\n\nSTDERR:\n" + result.stderr)
        raise RuntimeError(f"exit={result.returncode}, log={log}")

    rx = RE_DL if direction == "DL" else RE_UL
    thr = None
    for line in result.stdout.splitlines():
        m = rx.search(line)
        if m:
            thr = float(m.group(1))
            break

    if thr is None:
        log = os.path.join(OUTPUT_DIR,
                           f"{mode}_{direction}_n{n_obss}_rng{rng_run}_stdout.txt")
        with open(log, "w") as f:
            f.write(result.stdout)
        raise RuntimeError(f"{direction} line not found; log={log}")

    return thr


def run_one(mode, direction, n_obss, rng_run):
    last_exc = None
    for attempt in range(1, MAX_RETRIES + 1):
        try:
            return _run_once(mode, direction, n_obss, rng_run)
        except RuntimeError as e:
            last_exc = e
            print(f"  [RETRY {attempt}/{MAX_RETRIES}] {mode} {direction} "
                  f"nObss={n_obss} rng{rng_run}: {e}")
    raise RuntimeError(f"Failed after {MAX_RETRIES} attempts: {last_exc}")


def ci_hw(data, confidence=CI):
    from _robust import ci_hw_robust
    return ci_hw_robust(data, confidence=confidence)


def plot_results(raw, runs_label):
    """Render the praca-style grouped bar plot via plot_bar.plot()."""
    import importlib.util
    _mod_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "plot_bar.py")
    _spec = importlib.util.spec_from_file_location("_plot_bar_4_1_4", _mod_path)
    _pb = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_pb)
    _pb.plot(raw)
    return


def _plot_results_legacy_line(raw, runs_label):
    """Original line plot kept for reference; no longer the default output."""
    DIRS       = ["DL", "UL"]
    colors     = {"SLO": "#4C72B0", "STR": "#55A868", "EMLSR": "#DD8452"}
    markers    = {"SLO": "o",       "STR": "s",        "EMLSR": "^"}
    linestyles = {"DL": "-",        "UL": "--"}
    x          = np.array(OBSS_COUNTS)

    means = {m: {d: [] for d in DIRS} for m in MODES}
    hw    = {m: {d: [] for d in DIRS} for m in MODES}

    print("\n===== Aggregated results =====")
    for m in MODES:
        print(f"\n{m}:")
        for n in OBSS_COUNTS:
            for d in DIRS:
                mn, h = ci_hw(raw[m][d][n])
                means[m][d].append(mn)
                hw[m][d].append(h)
            print(f"  nObss={n:>2}: "
                  f"DL {means[m]['DL'][-1]:.2f}±{hw[m]['DL'][-1]:.2f}  "
                  f"UL {means[m]['UL'][-1]:.2f}±{hw[m]['UL'][-1]:.2f} Mb/s  "
                  f"(n={len(raw[m]['DL'][n])})")

    fig, ax = plt.subplots(figsize=(11, 6))

    for m in MODES:
        for d in DIRS:
            mn_arr = np.array(means[m][d])
            hw_arr = np.array(hw[m][d])
            ax.errorbar(
                x, mn_arr, yerr=hw_arr,
                label=f"{m} {d}",
                color=colors[m],
                linestyle=linestyles[d],
                marker=markers[m],
                markersize=6,
                linewidth=2,
                capsize=5,
                capthick=1.5,
                elinewidth=1.5,
                zorder=3,
            )

    ax.set_xlabel("Number of OBSS stations per link", fontsize=12)
    ax.set_ylabel("Main BSS Throughput [Mbit/s]", fontsize=12)
    ax.set_xticks(x)
    ax.set_xlim(-0.3, 10.3)
    ax.set_ylim(bottom=0)
    ax.set_title(
        f"Scenario 3 \u2013 Main BSS throughput vs. OBSS load\n"
        f"5 GHz (ch42) + 6 GHz (ch7), 80\u202fMHz, EHT MCS\u202f13  |  "
        f"Mean \u00b1 {int(CI*100)}% CI  |  {runs_label}",
        fontsize=12,
    )
    ax.legend(title="Mode / Direction", fontsize=10, title_fontsize=10,
              ncol=2, loc="upper right")
    ax.yaxis.grid(True, linestyle="--", alpha=0.45, zorder=0)
    ax.set_axisbelow(True)

    plt.tight_layout()
    plt.savefig(PLOT_FILE, format="svg")
    plt.show()
    print(f"\n[OK] Plot saved to {PLOT_FILE}")


# ---------------------------------------------------------------------------

def main():
    # Full job list: (mode, direction, n_obss, rng_run)
    jobs = [(mode, d, n, rr)
            for mode in MODES
            for d    in DIRECTIONS
            for n    in OBSS_COUNTS
            for rr   in range(RNG_START, RNG_START + RUNS)]

    raw         = {m: {d: {n: [] for n in OBSS_COUNTS} for d in DIRECTIONS}
                   for m in MODES}
    failed_jobs = []
    done        = 0

    already_done = load_existing_results(raw)
    if already_done:
        print(f"  Loaded {len(already_done)} result(s) from previous run(s) in '{OUTPUT_DIR}'.")
    pending_jobs = [j for j in jobs if j not in already_done]
    skipped      = len(jobs) - len(pending_jobs)
    if skipped:
        print(f"  Skipping {skipped} already-completed job(s).\n")

    if not pending_jobs:
        print("  All jobs already completed — skipping simulation phase.\n")
    else:
        print(f"Launching {len(pending_jobs)} simulation(s) on {MAX_WORKERS} workers ...\n")

    total = len(pending_jobs)

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as ex:
        fut = {ex.submit(run_one, mode, d, n, rr): (mode, d, n, rr)
               for mode, d, n, rr in pending_jobs}
        for future in as_completed(fut):
            mode, d, n, rr = fut[future]
            done += 1
            try:
                thr = future.result()
                raw[mode][d][n].append(thr)
                save_result(mode, d, n, rr, thr)
                print(f"  [{done:>3}/{total}]  {mode:<5} {d}  nObss={n:>2}  "
                      f"RngRun={rr}  thr={thr:.1f} Mb/s")
            except Exception as e:
                print(f"  [{done:>3}/{total}]  [ERROR] {mode} {d} nObss={n} rng{rr}: {e}")
                failed_jobs.append((mode, d, n, rr))

    if RERUN_MISSING and failed_jobs:
        print(f"\n{'='*60}")
        print(f"  Rerunning {len(failed_jobs)} failed job(s) ...\n")
        still_failed = []
        for idx, (mode, d, n, rr) in enumerate(failed_jobs, 1):
            print(f"  [RERUN {idx}/{len(failed_jobs)}]  {mode} {d}  nObss={n}  rng{rr}")
            try:
                thr = run_one(mode, d, n, rr)
                raw[mode][d][n].append(thr)
                save_result(mode, d, n, rr, thr)
                print(f"    -> thr={thr:.1f} Mb/s  [OK]")
            except Exception as e:
                print(f"    -> [STILL FAILED] {e}")
                still_failed.append((mode, d, n, rr))

        if still_failed:
            print(f"\n  WARNING: {len(still_failed)} job(s) could not be recovered:")
            for mode, d, n, rr in still_failed:
                print(f"    {mode} {d}  nObss={n}  rng{rr}")
        else:
            print("\n  All previously-failed jobs recovered successfully.")
        print(f"{'='*60}\n")

    plot_results(raw, f"{RUNS} runs")


if __name__ == "__main__":
    main()
