"""
Resumable runner + plotter for scratch/fig08_09_transmission_duration
Throughput Scenario #4: MPDU aggregation & TXOP limit vs. OBSS channel occupancy.

Sweep:
  nMpdus      ∈ {64, 512, 1024}
  txopLimitMs ∈ {0, 3}
  ch2OccPct   ∈ {0, 10, 20, 30, 40, 50, 60, 70}
  ch1OccPct   ∈ {0, 10}
  -> 3 × 2 × 8 × 2 × RUNS = 960 simulations (10 runs each)

Resume behaviour:
  Each completed run is saved to OUTPUT_DIR/<tag>_result.csv immediately.
  On re-launch, existing result files are loaded and those jobs are skipped.
  After the parallel sweep, any still-failed job is retried sequentially
  (RERUN_MISSING=True) to reduce resource contention.

Aggregated results are also written to OUTPUT_DIR/scenario4_results.csv at the end.
"""

# >>> thesis-style shim (auto-injected)
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), _os.pardir, "_common"))
import thesis_style as _thesis_style  # noqa: F401  installs unified plot style
# <<< thesis-style shim
import subprocess
import re
import os
import csv
import itertools
import numpy as np
import matplotlib.pyplot as plt
from concurrent.futures import ThreadPoolExecutor, as_completed
from scipy import stats

# ===== CONFIG =====
NS3_PROGRAM   = "scratch/fig08_09_transmission_duration"
SIMTIME       = 10.0
PAYLOAD       = 1500
RUNS          = 10
RNG_START     = 1001
CI            = 0.95
MAX_WORKERS   = os.cpu_count() or 8
OUTPUT_DIR    = "thr_runs_4_1_5"
PLOT_DIR      = "plots"
PLOT_FILE     = os.path.join(PLOT_DIR, "4_1_5.svg")
MAX_RETRIES   = 3
RERUN_MISSING = True   # retry failed jobs sequentially after the parallel sweep

NMDPUS_LIST  = [64, 512, 1024]
TXOP_LIST    = [0.0, 3.0]           # ms
CH2_OCC_LIST = list(range(0, 71, 10))   # 0..70 %
CH1_OCC_LIST = [0.0, 10.0]          # %

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(PLOT_DIR, exist_ok=True)

RE_DL = re.compile(r"MainBSS DL MacRx:\s*([0-9.]+)\s*Mb/s")


# ---------------------------------------------------------------------------
# Per-run persistence helpers

def _result_tag(nm, tx, c2, c1, rr):
    return f"n{nm}_t{int(tx)}_ch2_{int(c2)}_ch1_{int(c1)}_rng{rr}"


def _result_path(nm, tx, c2, c1, rr):
    return os.path.join(OUTPUT_DIR, f"{_result_tag(nm, tx, c2, c1, rr)}_result.csv")


def save_result(nm, tx, c2, c1, rr, dl):
    with open(_result_path(nm, tx, c2, c1, rr), "w") as f:
        f.write(f"{dl}\n")


def load_existing_results(raw):
    """Populate *raw* from saved per-run result files.
    Returns set of completed job tuples (nm, tx, c2, c1, rr)."""
    done_set = set()
    pattern  = re.compile(
        r"^n(\d+)_t(\d+)_ch2_(\d+)_ch1_(\d+)_rng(\d+)_result\.csv$"
    )
    if not os.path.isdir(OUTPUT_DIR):
        return done_set
    for fname in os.listdir(OUTPUT_DIR):
        m = pattern.match(fname)
        if not m:
            continue
        nm_s  = int(m.group(1))
        tx_s  = float(m.group(2))
        c2_s  = float(m.group(3))
        c1_s  = float(m.group(4))
        rr_s  = int(m.group(5))
        # Validate against sweep space
        if (nm_s not in NMDPUS_LIST or tx_s not in TXOP_LIST
                or c2_s not in CH2_OCC_LIST or c1_s not in CH1_OCC_LIST):
            continue
        try:
            with open(os.path.join(OUTPUT_DIR, fname)) as f:
                dl = float(f.read().strip())
            raw[(nm_s, tx_s, c1_s)][c2_s].append(dl)
            done_set.add((nm_s, tx_s, c2_s, c1_s, rr_s))
        except Exception:
            pass  # corrupt file — treat as missing
    return done_set


# ---------------------------------------------------------------------------

def _run_once(nm, tx, c2, c1, rng_run):
    """Single attempt: return dl [Mbit/s] or raise RuntimeError."""
    args = (
        f"{NS3_PROGRAM} "
        f"--RngRun={rng_run} "
        f"--simTime={SIMTIME} "
        f"--payloadSize={PAYLOAD} "
        f"--nMpdus={nm} "
        f"--txopLimitMs={tx} "
        f"--ch2OccPct={c2} "
        f"--ch1OccPct={c1}"
    )
    cmd    = ["./ns3", "run", args]
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        log = os.path.join(OUTPUT_DIR, f"{_result_tag(nm, tx, c2, c1, rng_run)}_err.txt")
        with open(log, "w") as f:
            f.write("CMD:\n" + " ".join(cmd)
                    + "\n\nSTDOUT:\n" + result.stdout
                    + "\n\nSTDERR:\n" + result.stderr)
        raise RuntimeError(f"exit={result.returncode}, log={log}")

    for line in result.stdout.splitlines():
        m = RE_DL.search(line)
        if m:
            return float(m.group(1))

    log = os.path.join(OUTPUT_DIR, f"{_result_tag(nm, tx, c2, c1, rng_run)}_stdout.txt")
    with open(log, "w") as f:
        f.write(result.stdout)
    raise RuntimeError(f"DL line not found; log={log}")


def run_one(nm, tx, c2, c1, rng_run):
    """Return dl; retries up to MAX_RETRIES times on error."""
    last_exc = None
    for attempt in range(1, MAX_RETRIES + 1):
        try:
            return _run_once(nm, tx, c2, c1, rng_run)
        except RuntimeError as e:
            last_exc = e
            print(f"  [RETRY {attempt}/{MAX_RETRIES}] "
                  f"n={nm} txop={tx}ms ch2={c2}% ch1={c1}% rng{rng_run}: {e}")
    raise RuntimeError(f"Failed after {MAX_RETRIES} attempts: {last_exc}")


def ci_hw(data, confidence=CI):
    from _robust import ci_hw_robust
    return ci_hw_robust(data, confidence=confidence)


# ---------------------------------------------------------------------------

def aggregate_and_plot(raw):
    means = {k: [] for k in raw}
    hw    = {k: [] for k in raw}

    print("\n===== Aggregated results =====")
    for k, c2_dict in raw.items():
        nm, tx, c1 = k
        print(f"\nnMpdus={nm}  txop={tx}ms  ch1={c1}%:")
        for c2 in CH2_OCC_LIST:
            mn, h = ci_hw(c2_dict[c2])
            means[k].append(mn)
            hw[k].append(h)
            print(f"  ch2={int(c2):>2}%: {mn:.2f} ± {h:.2f} Mb/s  (n={len(c2_dict[c2])})")

    # ── Save aggregated CSV ────────────────────────────────────────────────
    csv_file = os.path.join(OUTPUT_DIR, "scenario4_results.csv")
    with open(csv_file, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["nMpdus", "txopLimitMs", "ch1OccPct", "ch2OccPct",
                         "mean_dl_mbps", "ci_hw_mbps", "n_runs"])
        for k, c2_dict in raw.items():
            nm, tx, c1 = k
            for c2_idx, c2 in enumerate(CH2_OCC_LIST):
                mn = means[k][c2_idx]
                h  = hw[k][c2_idx]
                writer.writerow([nm, tx, int(c1), int(c2),
                                 f"{mn:.4f}", f"{h:.4f}", len(c2_dict[c2])])
    print(f"\n[OK] Aggregated results saved to {csv_file}")

    # ── Per-praca-Fig style: one PDF per ch1 occupancy, two TXOP panels each ─
    colors     = {64: "#4C72B0", 512: "#DD8452", 1024: "#55A868"}
    markers    = {64: "o",        512: "s",       1024: "^"}
    x          = np.array(CH2_OCC_LIST)

    for c1 in CH1_OCC_LIST:
        fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharey=True)
        for ax_idx, tx in enumerate(TXOP_LIST):
            ax = axes[ax_idx]
            for nm in NMDPUS_LIST:
                k      = (nm, tx, c1)
                mn_arr = np.array(means[k])
                hw_arr = np.array(hw[k])
                ax.errorbar(
                    x, mn_arr, yerr=hw_arr,
                    label=f"MPDU aggregation limit={nm}",
                    color=colors[nm],
                    linestyle="--",
                    marker=markers[nm],
                    markersize=6,
                    linewidth=1.6,
                    capsize=3,
                    capthick=1.0,
                    elinewidth=1.0,
                    zorder=3,
                )
            ax.set_xlabel("Channel occupancy [second link]", fontsize=11)
            ax.set_xticks(x)
            ax.set_xticklabels([f"{c2/100:.1f}" for c2 in CH2_OCC_LIST])
            ax.set_xlim(CH2_OCC_LIST[0] - 5, CH2_OCC_LIST[-1] + 5)
            ax.yaxis.grid(True, linestyle="--", alpha=0.4, zorder=0)
            ax.set_axisbelow(True)
            sub = "(a)" if ax_idx == 0 else "(b)"
            ax.set_title(f"{sub} TXOP limit = {int(tx)} ms", fontsize=11)
            if ax_idx == 0:
                ax.set_ylabel("Throughput [Mbit/s]", fontsize=11)
            ax.legend(fontsize=9, loc="upper right")

        occ_label = f"{int(c1)}% occupancy on first channel" \
            if c1 > 0 else "no occupancy on first channel"
        fig.suptitle(
            f"Throughput for different MPDU aggregation settings, {occ_label}\n"
            f"5 GHz ch42 + ch106, 80 MHz, EHT MCS 11  |  "
            f"Mean \u00b1 {int(CI*100)}% CI  |  {RUNS} runs",
            fontsize=11,
        )
        plt.tight_layout()
        out_basename = f"4_1_5_occ{int(c1)}.svg"
        out_path = os.path.join(PLOT_DIR, out_basename)
        plt.savefig(out_path, format="svg")
        plt.close(fig)
        print(f"[OK] Plot saved to {out_path}")


# ---------------------------------------------------------------------------

def main():
    all_keys = list(itertools.product(NMDPUS_LIST, TXOP_LIST, CH2_OCC_LIST, CH1_OCC_LIST))
    all_jobs = [(nm, tx, c2, c1, rr)
                for nm, tx, c2, c1 in all_keys
                for rr in range(RNG_START, RNG_START + RUNS)]

    raw = {(nm, tx, c1): {c2: [] for c2 in CH2_OCC_LIST}
           for nm, tx, c1 in itertools.product(NMDPUS_LIST, TXOP_LIST, CH1_OCC_LIST)}

    # ── Resume from previous run ──────────────────────────────────────────
    already_done = load_existing_results(raw)
    if already_done:
        print(f"  Loaded {len(already_done)} result(s) from '{OUTPUT_DIR}'.")
    pending = [j for j in all_jobs if j not in already_done]
    skipped = len(all_jobs) - len(pending)
    if skipped:
        print(f"  Skipping {skipped} already-completed job(s).\n")
    if not pending:
        print("  All jobs already completed — skipping simulation phase.\n")
    else:
        print(f"Launching {len(pending)} simulation(s) on {MAX_WORKERS} workers ...\n")

    failed_jobs = []
    done        = 0
    total       = len(pending)

    # ── First pass: parallel ──────────────────────────────────────────────
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as ex:
        fut = {ex.submit(run_one, nm, tx, c2, c1, rr): (nm, tx, c2, c1, rr)
               for nm, tx, c2, c1, rr in pending}
        for future in as_completed(fut):
            nm, tx, c2, c1, rr = fut[future]
            done += 1
            try:
                dl = future.result()
                raw[(nm, tx, c1)][c2].append(dl)
                save_result(nm, tx, c2, c1, rr, dl)
                print(f"  [{done:>3}/{total}]  n={nm:<4}  txop={int(tx)}ms  "
                      f"ch2={int(c2):>2}%  ch1={int(c1):>2}%  "
                      f"rng={rr}  DL={dl:.1f} Mb/s")
            except Exception as e:
                print(f"  [{done:>3}/{total}]  [ERROR] "
                      f"n={nm} txop={tx}ms ch2={c2}% ch1={c1}% rng{rr}: {e}")
                failed_jobs.append((nm, tx, c2, c1, rr))

    # ── Second pass: sequential rerun of failures ─────────────────────────
    if RERUN_MISSING and failed_jobs:
        print(f"\n{'='*60}")
        print(f"  Rerunning {len(failed_jobs)} failed job(s) sequentially ...\n")
        still_failed = []
        for idx, (nm, tx, c2, c1, rr) in enumerate(failed_jobs, 1):
            print(f"  [RERUN {idx}/{len(failed_jobs)}]  "
                  f"n={nm} txop={tx}ms ch2={c2}% ch1={c1}% rng{rr}")
            try:
                dl = run_one(nm, tx, c2, c1, rr)
                raw[(nm, tx, c1)][c2].append(dl)
                save_result(nm, tx, c2, c1, rr, dl)
                print(f"    -> DL={dl:.1f} Mb/s  [OK]")
            except Exception as e:
                print(f"    -> [STILL FAILED] {e}")
                still_failed.append((nm, tx, c2, c1, rr))
        if still_failed:
            print(f"\n  WARNING: {len(still_failed)} job(s) permanently failed:")
            for nm, tx, c2, c1, rr in still_failed:
                print(f"    n={nm} txop={tx}ms ch2={c2}% ch1={c1}% rng{rr}")
        else:
            print("\n  All previously-failed jobs recovered.")
        print(f"{'='*60}\n")

    aggregate_and_plot(raw)


if __name__ == "__main__":
    main()
