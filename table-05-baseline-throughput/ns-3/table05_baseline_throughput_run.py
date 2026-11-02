"""
Runner + table printer for scratch/table05_baseline_throughput
Scenarios x nMpdus values x 10 RNG runs – fully parallel.
Prints a summary table: Scenario | nMpdus=64 | nMpdus=1024

Extra features vs. run_all.py:
  - Per-run results saved to OUTPUT_DIR/*_result.csv — resumes automatically
    after a crash without re-running completed jobs.
  - RERUN_MISSING: retries failed jobs sequentially after the parallel sweep.
  - Summary table is always printed at the end, even with partial data.
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
from concurrent.futures import ThreadPoolExecutor, as_completed
from scipy import stats

# ===== CONFIG =====
NS3_PROGRAM   = "scratch/table05_baseline_throughput"
SIMTIME       = 30.0  # bumped from 10s — longer steady-state averaging
                      # helps 2L 160 MHz STR throughput stabilise above
                      # the previous ns-3 queue-feed dip

PAYLOAD       = 1500
RUNS          = 10
RNG_START     = 1001
CI            = 0.95
MAX_WORKERS   = os.cpu_count() or 8
OUTPUT_DIR    = "thr_runs_4_1_1"
MAX_RETRIES   = 3
RERUN_MISSING = True  # after the full sweep, retry every job that failed

# (label, mloMode, numLinks, channelWidth_MHz)
SCENARIOS = [
    ("1L  80 MHz",      "SLO", 1, 80),
    ("1L 160 MHz",      "SLO", 1, 160),
    ("2L STR  80 MHz",  "STR", 2, 80),
    ("2L STR 160 MHz",  "STR", 2, 160),
]

NMPDU_VALUES = [64, 1024]  # ns-3 hard cap is 1024 (802.11be standard limit)

RE_MACRX = re.compile(
    r"MacRx throughput \(STA received bits / simTime\):\s*([0-9.]+)\s*Mb/s"
)

os.makedirs(OUTPUT_DIR, exist_ok=True)


# ---------------------------------------------------------------------------
# Per-run result persistence helpers

def _safe(label):
    return label.replace(" ", "_")


def _result_path(label, nMpdus, rng_run):
    return os.path.join(OUTPUT_DIR, f"{_safe(label)}_nm{nMpdus}_rng{rng_run}_result.csv")


def save_result(label, nMpdus, rng_run, value):
    with open(_result_path(label, nMpdus, rng_run), "w") as f:
        f.write(f"{value}\n")


def load_existing_results(results):
    """Populate *results* from saved result files; return set of done job keys."""
    done_set = set()
    pattern  = re.compile(r"^(.+)_nm(\d+)_rng(\d+)_result\.csv$")
    label_map = {_safe(lbl): (sc_idx, lbl)
                 for sc_idx, (lbl, _, _, _) in enumerate(SCENARIOS)}
    nmpdus_set = set(NMPDU_VALUES)
    if not os.path.isdir(OUTPUT_DIR):
        return done_set
    for fname in os.listdir(OUTPUT_DIR):
        m = pattern.match(fname)
        if not m:
            continue
        safe_s, nm_s, rr_s = m.group(1), int(m.group(2)), int(m.group(3))
        if safe_s not in label_map or nm_s not in nmpdus_set:
            continue
        sc_idx, label = label_map[safe_s]
        nm_idx = NMPDU_VALUES.index(nm_s)
        try:
            with open(os.path.join(OUTPUT_DIR, fname)) as f:
                value = float(f.read().strip())
            results[sc_idx][nm_idx].append(value)
            done_set.add((sc_idx, nm_idx, rr_s))
        except Exception:
            pass  # corrupt file — treat as missing
    return done_set


# ---------------------------------------------------------------------------

def _run_once(label, mlo_mode, num_links, cw, nMpdus, rng_run):
    """Single attempt: return MacRx float or raise RuntimeError."""
    args = (
        f"{NS3_PROGRAM} "
        f"--RngRun={rng_run} "
        f"--simTime={SIMTIME} "
        f"--payloadSize={PAYLOAD} "
        f"--nMpdus={nMpdus} "
        f"--numLinks={num_links} "
        f"--mloMode={mlo_mode} "
        f"--channelWidth={cw}"
    )
    cmd = ["./ns3", "run", args]
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        safe = f"{_safe(label)}_n{nMpdus}"
        log = os.path.join(OUTPUT_DIR, f"{safe}_rng{rng_run}_err.txt")
        with open(log, "w") as f:
            f.write("CMD:\n" + " ".join(cmd)
                    + "\n\nSTDOUT:\n" + result.stdout
                    + "\n\nSTDERR:\n" + result.stderr)
        raise RuntimeError(f"exit={result.returncode}, log={log}")

    for line in result.stdout.splitlines():
        m = RE_MACRX.search(line)
        if m:
            return float(m.group(1))

    safe = f"{_safe(label)}_n{nMpdus}"
    log = os.path.join(OUTPUT_DIR, f"{safe}_rng{rng_run}_stdout.txt")
    with open(log, "w") as f:
        f.write(result.stdout)
    raise RuntimeError(f"MacRx line not found, log={log}")


def run_one(label, mlo_mode, num_links, cw, nMpdus, rng_run):
    last_exc = None
    for attempt in range(1, MAX_RETRIES + 1):
        try:
            return _run_once(label, mlo_mode, num_links, cw, nMpdus, rng_run)
        except RuntimeError as e:
            last_exc = e
            print(f"  [RETRY {attempt}/{MAX_RETRIES}] {label} nMpdus={nMpdus} "
                  f"rng{rng_run}: {e}")
    raise RuntimeError(f"Failed after {MAX_RETRIES} attempts: {last_exc}")


def ci_hw(data, confidence=CI):
    from _robust import ci_hw_robust
    return ci_hw_robust(data, confidence=confidence)


def print_table(results):
    COL = 20
    headers = [f"nMpdus = {n}" for n in NMPDU_VALUES]
    top = f"┌{'─'*(COL+2)}┬{'─'*(COL+2)}┬{'─'*(COL+2)}┐"
    mid = f"├{'─'*(COL+2)}┼{'─'*(COL+2)}┼{'─'*(COL+2)}┤"
    bot = f"└{'─'*(COL+2)}┴{'─'*(COL+2)}┴{'─'*(COL+2)}┘"

    def row_str(cells):
        return "│ " + " │ ".join(f"{c:<{COL}}" for c in cells) + " │"

    print(f"\n{top}")
    print(row_str(["Scenario"] + headers))
    print(mid)
    for sc_idx, (label, _, _, _) in enumerate(SCENARIOS):
        cells = [label]
        for nm_idx in range(len(NMPDU_VALUES)):
            vals = results[sc_idx][nm_idx]
            if vals:
                mean, hw = ci_hw(vals)
                cells.append(f"{mean:.1f} ± {hw:.1f} Mb/s")
            else:
                cells.append("N/A")
        print(row_str(cells))
    print(bot)
    print(f"Values: mean ± {int(CI*100)}% CI  ({RUNS} independent runs per cell)\n")


# ---------------------------------------------------------------------------

def main():
    jobs = [
        (sc_idx, nm_idx, label, mlo_mode, num_links, cw, nMpdus, rr)
        for sc_idx, (label, mlo_mode, num_links, cw) in enumerate(SCENARIOS)
        for nm_idx, nMpdus in enumerate(NMPDU_VALUES)
        for rr in range(RNG_START, RNG_START + RUNS)
    ]

    # results[sc_idx][nm_idx] = list of MacRx values
    results     = [[[] for _ in NMPDU_VALUES] for _ in SCENARIOS]
    failed_jobs = []
    done        = 0

    # ── Load results from any previous run ────────────────────────────────
    already_done = load_existing_results(results)
    if already_done:
        print(f"  Loaded {len(already_done)} result(s) from previous run(s) in '{OUTPUT_DIR}'.")
    pending_jobs = [j for j in jobs if (j[0], j[1], j[7]) not in already_done]
    skipped = len(jobs) - len(pending_jobs)
    if skipped:
        print(f"  Skipping {skipped} already-completed job(s).\n")

    if not pending_jobs:
        print("  All jobs already completed — skipping simulation phase.\n")
    else:
        print(
            f"Launching {len(pending_jobs)} simulation(s)  "
            f"on {MAX_WORKERS} workers ...\n"
        )

    total = len(pending_jobs)

    # ── First pass (parallel) ─────────────────────────────────────────────
    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as ex:
        fut = {
            ex.submit(run_one, label, mlo_mode, num_links, cw, nMpdus, rr):
                (sc_idx, nm_idx, label, nMpdus, rr)
            for sc_idx, nm_idx, label, mlo_mode, num_links, cw, nMpdus, rr in pending_jobs
        }
        for future in as_completed(fut):
            sc_idx, nm_idx, label, nMpdus, rr = fut[future]
            done += 1
            try:
                v = future.result()
                results[sc_idx][nm_idx].append(v)
                save_result(label, nMpdus, rr, v)
                print(f"  [{done:>3}/{total}]  {label:<18}  "
                      f"nMpdus={nMpdus:<5}  RngRun={rr}  ->  {v:.2f} Mb/s")
            except Exception as e:
                print(f"  [{done:>3}/{total}]  [ERROR]  {label}  "
                      f"nMpdus={nMpdus}  RngRun={rr}: {e}")
                failed_jobs.append(
                    (sc_idx, nm_idx, label,
                     SCENARIOS[sc_idx][1], SCENARIOS[sc_idx][2], SCENARIOS[sc_idx][3],
                     nMpdus, rr)
                )

    # ── Optional rerun of failed jobs (sequential) ────────────────────────
    if RERUN_MISSING and failed_jobs:
        print(f"\n{'='*60}")
        print(f"  Rerunning {len(failed_jobs)} failed job(s) ...\n")
        still_failed = []
        for idx, (sc_idx, nm_idx, label, mlo_mode, num_links, cw, nMpdus, rr) in \
                enumerate(failed_jobs, 1):
            print(f"  [RERUN {idx}/{len(failed_jobs)}]  {label}  "
                  f"nMpdus={nMpdus}  rng{rr}")
            try:
                v = run_one(label, mlo_mode, num_links, cw, nMpdus, rr)
                results[sc_idx][nm_idx].append(v)
                save_result(label, nMpdus, rr, v)
                print(f"    -> {v:.2f} Mb/s  [OK]")
            except Exception as e:
                print(f"    -> [STILL FAILED] {e}")
                still_failed.append((label, nMpdus, rr))
        if still_failed:
            print(f"\n  WARNING: {len(still_failed)} job(s) could not be recovered:")
            for label, nMpdus, rr in still_failed:
                print(f"    {label}  nMpdus={nMpdus}  rng{rr}")
        else:
            print("\n  All previously-failed jobs recovered successfully.")
        print(f"{'='*60}\n")

    # ── Summary table (always) ────────────────────────────────────────────
    print_table(results)


if __name__ == "__main__":
    main()
