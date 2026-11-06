"""
Runner + plotter for scratch/fig04_throughput_modes
Compares L1/L2/L3 throughput across MLO configurations.

Extra features vs. run_all.py:
  - Parallel execution via ThreadPoolExecutor (was sequential in run_all.py).
  - Per-run results saved to OUTPUT_DIR/*_result.csv — resumes automatically
    after a crash without re-running completed jobs.
  - RERUN_MISSING: retries failed jobs sequentially after the parallel sweep.
  - Summary CSV and plot are always produced at the end.
"""

# >>> thesis-style shim (auto-injected)
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), _os.pardir, "_common"))
import thesis_style as _thesis_style  # noqa: F401  installs unified plot style
# <<< thesis-style shim
import subprocess
import csv
import re
import os
import numpy as np
import matplotlib.pyplot as plt
from concurrent.futures import ThreadPoolExecutor, as_completed
from scipy import stats

# ===== CONFIG =====
NS3_PROGRAM   = "scratch/fig04_throughput_modes"
SIMTIME       = 30.0
PAYLOAD       = 1500
N_MPDUS       = 1024
RUNS          = 10
RNG_START     = 1001
CI            = 0.95
MAX_WORKERS   = os.cpu_count() or 8
OUTPUT_DIR    = "thr_runs_all"
SUMMARY_CSV   = "throughput_summary_all.csv"
PLOT_DIR      = "plots"
PLOT_FILE     = os.path.join(PLOT_DIR, "4_1_2.svg")
MAX_RETRIES   = 3
RERUN_MISSING = True  # after the full sweep, retry every job that failed

# (label, mloMode, numLinks)
CONFIGS = [
    ("SL 1 link",      "SLO",   1),
    ("EMLSR 2 links",  "EMLSR", 2),
    ("STR 2 links",    "STR",   2),
    ("EMLSR 3 links",  "EMLSR", 3),
    ("STR 3 links",    "STR",   3),
]

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(PLOT_DIR, exist_ok=True)

RE_L3      = re.compile(r"Average L3 throughput \(FlowMonitor\):\s*([0-9.]+)\s*Mb/s")
RE_MACRX   = re.compile(r"MacRx throughput \(STA received bits / simTime\):\s*([0-9.]+)\s*Mb/s")


# ---------------------------------------------------------------------------
# Per-run result persistence helpers

def _safe(label):
    return label.replace(" ", "_")


def _result_path(label, rng_run):
    return os.path.join(OUTPUT_DIR, f"{_safe(label)}_rng{rng_run}_result.csv")


def save_result(label, rng_run, res):
    """Persist a successful result dict."""
    with open(_result_path(label, rng_run), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(res.keys()))
        w.writeheader()
        w.writerow(res)


def load_existing_results(raw):
    """Populate *raw* from saved result files; return set of (label, rng_run) done."""
    done_set = set()
    # Match: <safe_label>_rng<N>_result.csv
    pattern  = re.compile(r"^(.+)_rng(\d+)_result\.csv$")
    label_set = {_safe(lbl): lbl for lbl, _, _ in CONFIGS}
    if not os.path.isdir(OUTPUT_DIR):
        return done_set
    for fname in os.listdir(OUTPUT_DIR):
        m = pattern.match(fname)
        if not m:
            continue
        safe_s, rr_s = m.group(1), int(m.group(2))
        if safe_s not in label_set:
            continue
        label = label_set[safe_s]
        try:
            with open(os.path.join(OUTPUT_DIR, fname), newline="") as f:
                reader = csv.DictReader(f)
                row = next(reader)
            res = {k: (float(v) if v not in (None, "", "None") else None)
                   for k, v in row.items()}
            raw[label].append((rr_s, res))
            done_set.add((label, rr_s))
        except Exception:
            pass  # corrupt file — treat as missing
    return done_set


# ---------------------------------------------------------------------------

def _run_once(label, mlo_mode, num_links, rng_run):
    """Single attempt: return result dict or raise RuntimeError."""
    cmd = [
        "./ns3", "run",
        f"{NS3_PROGRAM} "
        f"--RngRun={rng_run} --simTime={SIMTIME} --payloadSize={PAYLOAD} "
        f"--nMpdus={N_MPDUS} --numLinks={num_links} --mloMode={mlo_mode}"
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    stdout = result.stdout

    if result.returncode != 0:
        log = os.path.join(OUTPUT_DIR, f"{_safe(label)}_rng{rng_run}_err.txt")
        with open(log, "w") as f:
            f.write("STDOUT:\n" + stdout + "\n\nSTDERR:\n" + result.stderr)
        raise RuntimeError(f"exit={result.returncode}, log={log}")

    l3 = macrx = None
    for line in stdout.splitlines():
        if m := RE_L3.search(line):
            l3 = float(m.group(1))
        elif m := RE_MACRX.search(line):
            macrx = float(m.group(1))

    if macrx is None:
        log = os.path.join(OUTPUT_DIR, f"{_safe(label)}_rng{rng_run}_stdout.txt")
        with open(log, "w") as f:
            f.write(stdout)
        raise RuntimeError(f"MacRx throughput not found, log={log}")

    return {
        "L3_Mbps":    l3,
        "MacRx_Mbps": macrx,
    }


def run_one(label, mlo_mode, num_links, rng_run):
    last_exc = None
    for attempt in range(1, MAX_RETRIES + 1):
        try:
            return _run_once(label, mlo_mode, num_links, rng_run)
        except RuntimeError as e:
            last_exc = e
            print(f"  [RETRY {attempt}/{MAX_RETRIES}] {label} rng{rng_run}: {e}")
    raise RuntimeError(f"Failed after {MAX_RETRIES} attempts: {last_exc}")


def ci_hw(data, confidence=CI):
    from _robust import ci_hw_robust
    return ci_hw_robust(data, confidence=confidence)


def write_summary_csv(raw):
    all_rows = []
    for label, mlo_mode, num_links in CONFIGS:
        for run_idx, (rr, res) in enumerate(sorted(raw[label], key=lambda x: x[0]), 1):
            all_rows.append({
                "config_label": label,
                "mloMode": mlo_mode,
                "numLinks": num_links,
                "run_idx": run_idx,
                "RngRun": rr,
                **res,
            })
    fieldnames = [
        "config_label", "mloMode", "numLinks", "run_idx", "RngRun",
        "MacRx_Mbps", "L3_Mbps",
    ]
    with open(SUMMARY_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(all_rows)
    print(f"[OK] Summary saved to {SUMMARY_CSV}")


def plot_results(raw, runs_label):
    cfg_labels = []
    cfg_means  = []
    cfg_hw     = []

    print("\n===== Aggregated results =====")
    for label, _, _ in CONFIGS:
        vals = [res["MacRx_Mbps"]
                for _, res in raw[label]
                if res.get("MacRx_Mbps") is not None]
        if not vals:
            continue
        mn, h = ci_hw(vals)
        cfg_labels.append(label)
        cfg_means.append(mn)
        cfg_hw.append(h)
        print(f"  {label}: {mn:.3f} ± {h:.3f} Mb/s  (n={len(vals)})")

    if not cfg_means:
        print("[WARN] No data to plot.")
        return

    cfg_means_arr = np.array(cfg_means)
    cfg_hw_arr    = np.array(cfg_hw)
    baseline      = cfg_means_arr[0]
    rel           = (cfg_means_arr - baseline) / baseline * 100.0
    x             = np.arange(len(cfg_labels))

    fig, ax = plt.subplots(figsize=(8, 6))
    bars = ax.bar(x, cfg_means_arr, yerr=cfg_hw_arr, capsize=6,
                  color="#4C72B0", edgecolor="black", linewidth=0.8,
                  error_kw=dict(elinewidth=1.8, ecolor="black", capthick=1.8),
                  zorder=3)
    ax.set_xticks(x)
    ax.set_xticklabels(cfg_labels, fontsize=10)
    ax.set_ylabel("MacRx throughput [Mbit/s]", fontsize=12)
    ax.set_ylim(0, cfg_means_arr.max() * 1.3)
    ax.set_title(
        f"MacRx throughput vs. MLO mode / number of links\n"
        f"Mean ± {int(CI*100)}% CI  |  {runs_label}",
        fontsize=12,
    )
    ax.yaxis.grid(True, linestyle="--", alpha=0.45, zorder=0)
    ax.set_axisbelow(True)

    for i, (bar, mn, h, r) in enumerate(zip(bars, cfg_means_arr, cfg_hw_arr, rel)):
        top = float(mn + h)
        sign = "+" if r >= 0 else ""
        label_text = "" if i == 0 else f"{sign}{r:.1f}%"
        ax.text(bar.get_x() + bar.get_width() / 2.0,
                top + cfg_means_arr.max() * 0.02,
                label_text, ha="center", va="bottom", fontsize=10)

    plt.tight_layout()
    plt.savefig(PLOT_FILE, format="svg")
    plt.show()
    print(f"[OK] Plot saved to {PLOT_FILE}")


# ---------------------------------------------------------------------------

def main():
    # Build job list: assign unique rng_runs per config (non-overlapping ranges)
    jobs = []
    rng = RNG_START
    config_rng = {}  # label -> list of rng_runs assigned
    for label, mlo_mode, num_links in CONFIGS:
        rng_runs = list(range(rng, rng + RUNS))
        config_rng[label] = rng_runs
        for rr in rng_runs:
            jobs.append((label, mlo_mode, num_links, rr))
        rng += RUNS

    # raw[label] = list of (rng_run, result_dict)
    raw         = {label: [] for label, _, _ in CONFIGS}
    failed_jobs = []
    done        = 0

    # ── Load results from any previous run ────────────────────────────────
    already_done = load_existing_results(raw)
    if already_done:
        print(f"  Loaded {len(already_done)} result(s) from previous run(s) in '{OUTPUT_DIR}'.")
    pending_jobs = [(lbl, mm, nl, rr) for lbl, mm, nl, rr in jobs
                    if (lbl, rr) not in already_done]
    skipped = len(jobs) - len(pending_jobs)
    if skipped:
        print(f"  Skipping {skipped} already-completed job(s).\n")

    if not pending_jobs:
        print("  All jobs already completed — skipping simulation phase.\n")
    else:
        print(f"Launching {len(pending_jobs)} simulation(s) on {MAX_WORKERS} workers ...\n")

    total = len(pending_jobs)

    # ── First pass (parallel) ─────────────────────────────────────────────
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as ex:
        fut = {ex.submit(run_one, lbl, mm, nl, rr): (lbl, mm, nl, rr)
               for lbl, mm, nl, rr in pending_jobs}
        for future in as_completed(fut):
            lbl, mm, nl, rr = fut[future]
            done += 1
            try:
                res = future.result()
                raw[lbl].append((rr, res))
                save_result(lbl, rr, res)
                thr = res["MacRx_Mbps"]
                print(f"  [{done:>3}/{total}]  {lbl:<18}  RngRun={rr}"
                      f"  MacRx={thr:.2f} Mb/s")
            except Exception as e:
                print(f"  [{done:>3}/{total}]  [ERROR] {lbl} rng{rr}: {e}")
                failed_jobs.append((lbl, mm, nl, rr))

    # ── Optional rerun of failed jobs (sequential) ────────────────────────
    if RERUN_MISSING and failed_jobs:
        print(f"\n{'='*60}")
        print(f"  Rerunning {len(failed_jobs)} failed job(s) ...\n")
        still_failed = []
        for idx, (lbl, mm, nl, rr) in enumerate(failed_jobs, 1):
            print(f"  [RERUN {idx}/{len(failed_jobs)}]  {lbl}  rng{rr}")
            try:
                res = run_one(lbl, mm, nl, rr)
                raw[lbl].append((rr, res))
                save_result(lbl, rr, res)
                thr = res["MacRx_Mbps"]
                print(f"    -> MacRx={thr:.2f} Mb/s  [OK]")
            except Exception as e:
                print(f"    -> [STILL FAILED] {e}")
                still_failed.append((lbl, mm, nl, rr))
        if still_failed:
            print(f"\n  WARNING: {len(still_failed)} job(s) could not be recovered:")
            for lbl, _, _, rr in still_failed:
                print(f"    {lbl}  rng{rr}")
        else:
            print("\n  All previously-failed jobs recovered successfully.")
        print(f"{'='*60}\n")

    # ── Summary CSV + Plot (always) ───────────────────────────────────────
    write_summary_csv(raw)
    plot_results(raw, f"{RUNS} runs per config")


if __name__ == "__main__":
    main()
