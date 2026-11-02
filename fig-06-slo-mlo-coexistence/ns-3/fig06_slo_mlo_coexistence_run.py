"""
Runner + plotter for scratch/fig06_slo_mlo_coexistence
Two modes: STR and EMLSR (BSS2 only; BSS0/BSS1 always SLO).
10 independent runs each, per-BSS throughput, 95 % CI bars.

Extra features vs. run_all.py:
  - Per-run results saved to OUTPUT_DIR/*_result.csv — script resumes
    automatically after a crash without re-running completed jobs.
  - RERUN_MISSING: retries failed jobs sequentially after the parallel sweep.
  - Plot is always produced at the end, even with partial data.
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
NS3_PROGRAM   = "scratch/fig06_slo_mlo_coexistence"
SIMTIME       = 10.0
PAYLOAD       = 1500
NMDPUS        = 1024
RUNS          = 10
RNG_START     = 1001
CI            = 0.95
MAX_WORKERS   = os.cpu_count() or 8
OUTPUT_DIR    = "thr_runs_4_1_3"
PLOT_DIR      = "plots"
PLOT_FILE     = os.path.join(PLOT_DIR, "4_1_3.svg")
MAX_RETRIES   = 3
RERUN_MISSING = True  # after the full sweep, retry every job that failed

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(PLOT_DIR, exist_ok=True)

MODES      = ["STR", "EMLSR"]   # only affects BSS2
BSS_LABELS = ["BSS0\n(SLO ch42)", "BSS1\n(SLO ch106)", "BSS2\n(MLO ch42+106)"]

RE_BSS = [
    re.compile(r"MacRx BSS0[^:]*:\s*([0-9.]+)\s*Mb/s"),
    re.compile(r"MacRx BSS1[^:]*:\s*([0-9.]+)\s*Mb/s"),
    re.compile(r"MacRx BSS2[^:]*:\s*([0-9.]+)\s*Mb/s"),
]

os.makedirs(OUTPUT_DIR, exist_ok=True)


# ---------------------------------------------------------------------------
# Per-run result persistence helpers

def _result_path(mode, rng_run):
    return os.path.join(OUTPUT_DIR, f"{mode}_rng{rng_run}_result.csv")


def save_result(mode, rng_run, b0, b1, b2):
    with open(_result_path(mode, rng_run), "w") as f:
        f.write(f"{b0},{b1},{b2}\n")


def load_existing_results(raw):
    """Populate *raw* from saved result files; return set of (mode, rng_run) done."""
    done_set = set()
    pattern  = re.compile(r"^([A-Za-z]+)_rng(\d+)_result\.csv$")
    if not os.path.isdir(OUTPUT_DIR):
        return done_set
    for fname in os.listdir(OUTPUT_DIR):
        m = pattern.match(fname)
        if not m:
            continue
        mode_s, rr_s = m.group(1), int(m.group(2))
        if mode_s not in MODES:
            continue
        try:
            with open(os.path.join(OUTPUT_DIR, fname)) as f:
                b0, b1, b2 = map(float, f.read().strip().split(","))
            raw[mode_s][0].append(b0)
            raw[mode_s][1].append(b1)
            raw[mode_s][2].append(b2)
            done_set.add((mode_s, rr_s))
        except Exception:
            pass  # corrupt file — treat as missing
    return done_set


# ---------------------------------------------------------------------------

def _run_once(mode, rng_run):
    """Single attempt: return (bss0, bss1, bss2) Mb/s or raise RuntimeError."""
    args = (
        f"{NS3_PROGRAM} "
        f"--RngRun={rng_run} "
        f"--simTime={SIMTIME} "
        f"--payloadSize={PAYLOAD} "
        f"--nMpdus={NMDPUS} "
        f"--mloMode={mode}"
    )
    cmd = ["./ns3", "run", args]
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        log = os.path.join(OUTPUT_DIR, f"{mode}_rng{rng_run}_err.txt")
        with open(log, "w") as f:
            f.write("CMD:\n" + " ".join(cmd)
                    + "\n\nSTDOUT:\n" + result.stdout
                    + "\n\nSTDERR:\n" + result.stderr)
        raise RuntimeError(f"exit={result.returncode}, log={log}")

    vals = [None, None, None]
    for line in result.stdout.splitlines():
        for i, rx in enumerate(RE_BSS):
            m = rx.search(line)
            if m:
                vals[i] = float(m.group(1))

    if any(v is None for v in vals):
        log = os.path.join(OUTPUT_DIR, f"{mode}_rng{rng_run}_stdout.txt")
        with open(log, "w") as f:
            f.write(result.stdout)
        raise RuntimeError(f"Missing MacRx line(s): {vals}, log={log}")

    return tuple(vals)


def run_one(mode, rng_run):
    last_exc = None
    for attempt in range(1, MAX_RETRIES + 1):
        try:
            return _run_once(mode, rng_run)
        except RuntimeError as e:
            last_exc = e
            print(f"  [RETRY {attempt}/{MAX_RETRIES}] {mode} rng{rng_run}: {e}")
    raise RuntimeError(f"Failed after {MAX_RETRIES} attempts: {last_exc}")


def ci_hw(data, confidence=CI):
    from _robust import ci_hw_robust
    return ci_hw_robust(data, confidence=confidence)


def plot_results(raw, runs_label):
    means = {m: [] for m in MODES}
    hw    = {m: [] for m in MODES}

    print("\n===== Aggregated results =====")
    for m in MODES:
        print(f"\n{m}:")
        for b in range(3):
            mn, h = ci_hw(raw[m][b])
            means[m].append(mn)
            hw[m].append(h)
            print(f"  BSS{b}: {mn:.2f} ± {h:.2f} Mb/s  (n={len(raw[m][b])})")

    n_modes = len(MODES)
    x       = np.arange(n_modes)
    width   = 0.25
    colors  = {0: "#4C72B0", 1: "#DD8452", 2: "#55A868"}
    bss_labels_short = ["BSS0 (SLO ch42)", "BSS1 (SLO ch106)", "BSS2 (MLO ch42+106)"]
    offsets = [-width, 0, width]

    fig, ax = plt.subplots(figsize=(11, 6))

    for bss_idx in range(3):
        mode_values = [means[m][bss_idx] for m in MODES]
        mode_errs   = [hw[m][bss_idx]    for m in MODES]
        bars = ax.bar(
            x + offsets[bss_idx], mode_values, width,
            yerr=mode_errs, capsize=6,
            label=bss_labels_short[bss_idx],
            color=colors[bss_idx], edgecolor="black", linewidth=0.8,
            error_kw=dict(elinewidth=1.8, ecolor="black", capthick=1.8),
            zorder=3,
        )
        for bar, mn, h in zip(bars, mode_values, mode_errs):
            top = float(mn + h)
            ax.text(bar.get_x() + bar.get_width() / 2.0, top,
                    f"{mn:.1f}", ha="center", va="bottom",
                    fontsize=8, fontweight="bold")

    all_tops = [means[m][b] + hw[m][b] for m in MODES for b in range(3)]
    y_top = max(all_tops) if all_tops else 1.0
    ax.set_ylim(0, y_top * 1.25)
    ax.set_xticks(x)
    ax.set_xticklabels(MODES, fontsize=12, fontweight="bold")
    ax.set_ylabel("Throughput [Mbit/s]", fontsize=12)
    ax.set_xlabel("BSS2 Mode", fontsize=12)
    ax.set_title(
        f"Coexistence scenario – per-BSS MacRx throughput\n"
        f"BSS2 mode: STR vs EMLSR  |  "
        f"Mean ± {int(CI*100)}% CI  |  {runs_label}",
        fontsize=12,
    )
    ax.legend(title="BSS Configuration", fontsize=10, title_fontsize=10, loc="upper left")
    ax.yaxis.grid(True, linestyle="--", alpha=0.45, zorder=0)
    ax.set_axisbelow(True)
    plt.tight_layout()
    plt.savefig(PLOT_FILE, format="svg")
    plt.show()
    print(f"\n[OK] Plot saved to {PLOT_FILE}")


# ---------------------------------------------------------------------------

def main():
    jobs = [(mode, rr)
            for mode in MODES
            for rr   in range(RNG_START, RNG_START + RUNS)]

    # raw[mode][bss_idx] = list of floats
    raw         = {m: [[] for _ in range(3)] for m in MODES}
    failed_jobs = []
    done        = 0

    # ── Load results from any previous run ────────────────────────────────
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

    # ── First pass (parallel) ─────────────────────────────────────────────
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as ex:
        fut = {ex.submit(run_one, mode, rr): (mode, rr)
               for mode, rr in pending_jobs}
        for future in as_completed(fut):
            mode, rr = fut[future]
            done += 1
            try:
                b0, b1, b2 = future.result()
                raw[mode][0].append(b0)
                raw[mode][1].append(b1)
                raw[mode][2].append(b2)
                save_result(mode, rr, b0, b1, b2)
                print(f"  [{done:>3}/{total}]  {mode:<5}  RngRun={rr}"
                      f"  BSS0={b0:.1f}  BSS1={b1:.1f}  BSS2={b2:.1f}  Mb/s")
            except Exception as e:
                print(f"  [{done:>3}/{total}]  [ERROR]  {mode}  RngRun={rr}: {e}")
                failed_jobs.append((mode, rr))

    # ── Optional rerun of failed jobs (sequential) ────────────────────────
    if RERUN_MISSING and failed_jobs:
        print(f"\n{'='*60}")
        print(f"  Rerunning {len(failed_jobs)} failed job(s) ...\n")
        still_failed = []
        for idx, (mode, rr) in enumerate(failed_jobs, 1):
            print(f"  [RERUN {idx}/{len(failed_jobs)}]  {mode}  rng{rr}")
            try:
                b0, b1, b2 = run_one(mode, rr)
                raw[mode][0].append(b0)
                raw[mode][1].append(b1)
                raw[mode][2].append(b2)
                save_result(mode, rr, b0, b1, b2)
                print(f"    -> BSS0={b0:.1f}  BSS1={b1:.1f}  BSS2={b2:.1f}  Mb/s  [OK]")
            except Exception as e:
                print(f"    -> [STILL FAILED] {e}")
                still_failed.append((mode, rr))
        if still_failed:
            print(f"\n  WARNING: {len(still_failed)} job(s) could not be recovered:")
            for mode, rr in still_failed:
                print(f"    {mode}  rng{rr}")
        else:
            print("\n  All previously-failed jobs recovered successfully.")
        print(f"{'='*60}\n")

    # ── Plot (always, even with partial data) ─────────────────────────────
    plot_results(raw, f"{RUNS} runs")


if __name__ == "__main__":
    main()
