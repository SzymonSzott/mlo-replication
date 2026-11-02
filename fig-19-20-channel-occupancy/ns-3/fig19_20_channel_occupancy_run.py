#!/usr/bin/env python3
"""
run_all_rerun.py  --  Latency Scenario #5 (4.2.6)

Based on [17]: Carrascosa-Zamacois et al.,
    "Wi-Fi multi-link operation: An experimental study of latency and throughput",
    IEEE/ACM Transactions on Networking 32.1 (2023), pp. 308–322.

Sweeps over:
  - numLinks:     1 (SLO), 2 (MLO-STR)
  - OBSS config:  symmetric {10%/10%, 40%/40%, 70%/70%}
                  asymmetric {10%/40%, 10%/70%, 40%/70%}  (primary/secondary)
  - offeredLoad:  normalised loads × single-link capacity
                  normLoad ∈ {0.2, 0.4, 0.6, 0.8}  → ~{20, 40, 60, 80} Mb/s
                  (paper-aligned: 20 MHz / MCS 9 / 2 SS ≈ 100 Mb/s useful)
  - RngRun:       1 … RUNS  (default 10)

Channel occupancy mapping:
  numObssStas=1 → 10 %  (1 × 10 Mb/s / 100 Mb/s)
  numObssStas=4 → 40 %  (4 × 10 Mb/s / 100 Mb/s)
  numObssStas=7 → 70 %  (7 × 10 Mb/s / 100 Mb/s)

SLO always uses ch0 (primary / less-congested channel).
For SLO, OBSS-2 is not created (numObssStas2=0 is forced in the C++ code).

Persistence: per-run CSV files → resume after crash / add more runs.
Re-run:      failed jobs retried sequentially after the parallel pass.

Plots produced (in plots/):
    4_2_6_sym.svg   — Fig. 4.12: symmetric occupancy (bar subfigures)
    4_2_6_asym.svg  — Fig. 4.13: asymmetric occupancy (bar subfigures)
"""

# >>> thesis-style shim (auto-injected)
import sys as _sys, os as _os
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), _os.pardir, "_common"))
import thesis_style as _thesis_style  # noqa: F401  installs unified plot style
# <<< thesis-style shim

import csv
import os
import re
import subprocess
from review_run_record import save_run_record
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from itertools import product
from statistics import mean, stdev

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.lines import Line2D

import joanna_plots  # noqa: E402  shared matplotlib plotter (scratch/_common)

# ---------------------------------------------------------------------------
#  Configuration
# ---------------------------------------------------------------------------
NS3_BINARY    = "build/scratch/ns3.45-fig19_20_channel_occupancy-optimized"
OUTPUT_DIR    = "thr_runs_4_2_6_e2e"
PLOT_DIR      = "plots"

# Paper-aligned Carrascosa TON [17]: 20 MHz channels (ch 36, ch 100),
# OBSS topology: each "n" in (n1, n2) is the count of INDEPENDENT OBSS BSSs
# (one AP+STA each) on the channel.
#
# Parameterisation follows the THESIS methodology (Kozioł §3.1 /
# §3.2.11), which is what the MATLAB side implements -- NOT Carrascosa's
# testbed numbers. Two corrections vs. the previous settings:
#
#  1. generateTrafficParamsOBSS "fixes the offered traffic to 10 % of the
#     maximum achievable throughput ... to achieve channel occupancy equal to
#     70 %, an OBSS consisting of an AP and seven stations is required".
#     So one OBSS BSS == 10 % of the single-link baseline. The old 6.0 Mb/s
#     made "10/40/70 %" actually 0.5/2.2/3.8 % -- 18x too weak.
#
#  2. generateTrafficParams computes
#         totalOfferedTraffic = expectedLoad * maxThr
#     where maxThr is the baseline max for the LINK CONFIGURATION (Table 4.1),
#     not an occupancy-dependent constant. The old PAPER_SLO_MAX_BY_PRIMARY
#     used Carrascosa's measured testbed rates (37/22/6.8 Mb/s), giving
#     offered loads of 7-30 Mb/s against a ~1116 Mb/s link -- under 3 %
#     utilisation, so the scenario never reached the saturation knee it exists
#     to show.
#
# Baseline = ns-3's own scenario 4.1.1 measurements (thr_runs_4_1_1,
# 80 MHz, MCS 11, A-MPDU 1024), mirroring how the thesis normalises against
# its own Table 4.1 (927 / 1854 Mb/s). This scenario independently saturates
# at 1119.6 / 2211.2 Mb/s, i.e. within 1 % of these values, confirming the
# two configurations now agree.
BASELINE_MAX_THR = {1: 1116.3, 2: 2230.4}   # Mb/s, keyed by numLinks

# One OBSS BSS = 10 % of the single-link baseline.
OBSS_RATE_PER_STA   = round(0.10 * BASELINE_MAX_THR[1], 1)   # 111.6 Mb/s

NORM_LOADS    = [0.2, 0.4, 0.6, 0.8]

# OBSS configurations: (n1, n2, label, title_for_plot)
#   n1 = OBSS BSSs on ch0 (primary,  used by SLO)
#   n2 = OBSS BSSs on ch1 (secondary, used only by MLO)
OBSS_CONFIGS = [
    # ── symmetric ──────────────────────────────────────────────────────────
    (1, 1, "sym10",     "10 % on both channels"),
    (4, 4, "sym40",     "40 % on both channels"),
    (7, 7, "sym70",     "70 % on both channels"),
    # ── asymmetric (primary < secondary, so SLO picks primary) ────────────
    (1, 4, "asym10_40", "Primary 10 %, secondary 40 %"),
    (1, 7, "asym10_70", "Primary 10 %, secondary 70 %"),
    (4, 7, "asym40_70", "Primary 40 %, secondary 70 %"),
]


def loads_for_config(num_links: int) -> list[int]:
    """Return offered loads [Mbit/s] for a link configuration.

    Thesis generateTrafficParams: offered load = expectedLoad * maxThr, where
    maxThr is the baseline max achievable throughput for that link count.
    Normalisation is therefore keyed by numLinks (SLO vs MLO-STR), not by
    OBSS occupancy.
    """
    max_thr = BASELINE_MAX_THR[num_links]
    return [max(1, round(f * max_thr)) for f in NORM_LOADS]

NUM_LINKS_LIST = [1, 2]   # 1 = SLO, 2 = MLO-STR
RUNS           = 10
SIM_TIME       = 5.0      # s (shorter than thesis default for speed; use 10 for publication)
STARTUP_GUARD  = 2.0      # s; exclude association/setup transients
PAYLOAD        = 1500
NMDPUS         = 1024  # A-MPDU limit -- thesis Table 3.12 / §3.2.11:
                       # "MCS 11 and the MPDU aggregation limit of 1024".
                       # (was 1, following Carrascosa's single-MPDU testbed
                       # model, which is not what the MATLAB side simulates.)
MAX_WORKERS    = 10
RERUN_MISSING  = True

# Two-sided 95 % Student-t critical values (df = n-1).
T95 = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447,
    7: 2.365,  8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179,
    13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101,
    19: 2.093, 20: 2.086, 25: 2.060, 30: 2.042,
}

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(PLOT_DIR,   exist_ok=True)

RE_LAT = re.compile(r"Mean DL Latency:\s*([0-9.]+)\s*ms")
RE_P50 = re.compile(r"DL Latency p50:\s*([0-9.]+)\s*ms")
RE_P95 = re.compile(r"DL Latency p95:\s*([0-9.]+)\s*ms")
RE_P99 = re.compile(r"DL Latency p99:\s*([0-9.]+)\s*ms")
RE_THR = re.compile(r"Mean DL Throughput:\s*([0-9.]+)\s*Mb/s")

MODE_LABELS = {1: "SLO", 2: "MLO-STR"}

# ---------------------------------------------------------------------------
#  Persistence helpers
# ---------------------------------------------------------------------------

def _result_path(nl: int, n1: int, n2: int, load: int, rng: int) -> str:
    return os.path.join(OUTPUT_DIR,
                        f"NL{nl}_N1{n1}_N2{n2}_Load{load}_rng{rng}_result.csv")


def _parse_row(text: str):
    """Return (lat, p50, p95, p99, thr) floats, or None."""
    reader = csv.reader([text])
    row = next(reader, [])
    parts = [p.strip() for p in row if p.strip()]
    if len(parts) >= 5:
        return tuple(float(p) for p in parts[:5])
    if len(parts) >= 4:
        lat, p50, p99, thr = (float(p) for p in parts[:4])
        return lat, p50, p99, p99, thr      # p95 fallback = p99
    if len(parts) >= 2:
        lat, thr = float(parts[0]), float(parts[1])
        return lat, lat, lat, lat, thr
    return None


# ---------------------------------------------------------------------------
#  Run a single simulation
# ---------------------------------------------------------------------------

def run_job(nl: int, n1: int, n2: int, load: int, rng: int) -> tuple:
    """Run one simulation; return (nl, n1, n2, load, rng, lat, p50, p95, p99, thr)."""
    rpath = _result_path(nl, n1, n2, load, rng)
    if os.path.exists(rpath):
        try:
            with open(rpath) as f:
                parsed = _parse_row(f.read().strip())
            if parsed:
                lat, p50, p95, p99, thr = parsed
                return (nl, n1, n2, load, rng, lat, p50, p95, p99, thr)
        except Exception:
            pass

    # For SLO, always pass n2=0 so the C++ code skips OBSS-2.
    effective_n2 = n2 if nl == 2 else 0

    args = (
        f"--simTime={SIM_TIME} --payloadSize={PAYLOAD} --nMpdus={NMDPUS} "
        f"--offeredLoad={load} --numLinks={nl} "
        f"--numObssStas1={n1} --numObssStas2={effective_n2} "
        f"--obssRatePerSta={OBSS_RATE_PER_STA} "
        f"--startupGuard={STARTUP_GUARD} --RngRun={rng}"
    )
    cmd = [NS3_BINARY] + args.split()

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        save_run_record(rpath, cmd, result)
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
            with open(rpath, "w") as f:
                f.write(f"{lat},{p50},{p95},{p99},{thr}\n")
            return (nl, n1, n2, load, rng, lat, p50, p95, p99, thr)
        else:
            print(f"  PARSE-FAIL NL{nl} N1={n1} N2={n2} Load={load} rng={rng}",
                  file=sys.stderr)
            print(f"  stdout tail: {result.stdout[-300:]}", file=sys.stderr)
    except Exception as e:
        print(f"  ERROR NL{nl} N1={n1} N2={n2} Load={load} rng={rng}: {e}",
              file=sys.stderr)
    return (nl, n1, n2, load, rng, None, None, None, None, None)


# ---------------------------------------------------------------------------
#  Load existing results
# ---------------------------------------------------------------------------

def load_results() -> dict:
    """Return dict: (nl, n1, n2, load) → [(rng, lat, p50, p95, p99, thr)]."""
    results: dict = {}
    if not os.path.isdir(OUTPUT_DIR):
        return results
    pat = re.compile(r"NL(\d+)_N1(\d+)_N2(\d+)_Load(\d+)_rng(\d+)_result\.csv$")
    for fname in os.listdir(OUTPUT_DIR):
        m = pat.match(fname)
        if not m:
            continue
        nl, n1, n2, load, rng = (int(m.group(i)) for i in range(1, 6))
        try:
            with open(os.path.join(OUTPUT_DIR, fname)) as f:
                parsed = _parse_row(f.read().strip())
            if parsed is None:
                continue
            lat, p50, p95, p99, thr = parsed
            key = (nl, n1, n2, load)
            results.setdefault(key, [])
            results[key].append((rng, lat, p50, p95, p99, thr))
        except Exception:
            pass
    return results


# ---------------------------------------------------------------------------
#  Statistics helpers
# ---------------------------------------------------------------------------

def ci95(values):
    from _robust import ci_hw_robust
    return ci_hw_robust(values, confidence=0.95)[1]


def _filtered(values):
    from _robust import remove_outliers_robust
    return remove_outliers_robust(values)


# ---------------------------------------------------------------------------
#  Plotting
# ---------------------------------------------------------------------------

def _gather_series(raw: dict, nl: int, n1: int, n2: int):
    """Return per-load arrays for bar plotting."""
    xs, lat_m, lat_e, p95_m = [], [], [], []
    for norm_load, load in zip(NORM_LOADS, loads_for_config(nl)):
        key = (nl, n1, n2 if nl == 2 else 0, load)
        rows = raw.get(key, [])
        if not rows:
            continue
        lats = [r[1] for r in rows]
        p95s = [r[3] for r in rows]
        xs.append(norm_load)
        lats_clean = _filtered(lats)
        p95s_clean = _filtered(p95s)
        lat_m.append(mean(lats_clean));  lat_e.append(ci95(lats_clean))
        p95_m.append(mean(p95s_clean))
    return xs, lat_m, lat_e, p95_m


def _plot_set(raw: dict, configs: list, out_path: str, title_prefix: str) -> None:
    """
    configs: list of (n1, n2, label, title) entries.
    Produces a 1×len(configs) grid of bar-plot subfigures.
    Each subfigure shows grouped bars (SLO vs MLO-STR) per normalised load,
    with a translucent segment from mean latency up to 95th-percentile latency.
    """
    nfig = len(configs)
    # Lay out as up to 3 columns; wrap to 2 rows when more panels.
    if nfig <= 3:
        ncols, nrows = nfig, 1
    else:
        ncols = 3
        nrows = int(np.ceil(nfig / ncols))
    fig, axes = plt.subplots(nrows, ncols,
                             figsize=(5.0 * ncols, 4.2 * nrows),
                             sharey=False)
    if nfig == 1:
        axes = [axes]
    else:
        axes = np.array(axes).reshape(-1)
    fig.suptitle(title_prefix, fontsize=13)

    colors = {1: "#440154", 2: "#21918c"}   # SLO=blue, MLO=red
    width = 0.34
    x = np.arange(len(NORM_LOADS))
    xlabels = [f"{v:.1f}" for v in NORM_LOADS]

    for ax, (n1, n2, label, title) in zip(axes, configs):
        for idx, nl in enumerate(NUM_LINKS_LIST):
            xs, lat_m, lat_e, p95_m = _gather_series(raw, nl, n1, n2)
            if not xs:
                continue
            c = colors[nl]
            xpos = x + (-width / 2 if idx == 0 else width / 2)
            load_to_idx = {v: i for i, v in enumerate(NORM_LOADS)}
            bar_x = np.array([xpos[load_to_idx[v]] for v in xs])
            lat_vals = np.array(lat_m)
            lat_errs = np.array(lat_e)
            p95_vals = np.array(p95_m)
            p95_extra = np.maximum(0.0, p95_vals - lat_vals)

            ax.bar(bar_x, lat_vals, width=width, color=c,
                   edgecolor="black", linewidth=0.4,
                   yerr=lat_errs, capsize=3,
                   label=MODE_LABELS[nl], zorder=3)
            ax.bar(bar_x, p95_extra, width=width, bottom=lat_vals,
                   color=c, alpha=0.35,
                   edgecolor="black", linewidth=0.2,
                   zorder=3)
            ax.scatter(bar_x, p95_vals, marker="_", s=70,
                       color="black", linewidths=1.0, zorder=4)

        ax.set_title(title, fontsize=10)
        ax.set_xlabel("Normalised traffic load", fontsize=9)
        ax.set_ylabel("Channel access delay [ms]", fontsize=9)
        ax.set_xticks(x)
        ax.set_xticklabels(xlabels)
        p95_handle = Line2D([0], [0], marker="_", color="black", linestyle="",
                            markersize=10, label="95 %-ile")
        handles, labels = ax.get_legend_handles_labels()
        ax.legend(handles + [p95_handle], labels + ["95 %-ile"], fontsize=7)
        ax.grid(True, linestyle="--", alpha=0.5)

    # hide unused axes if grid is bigger than nfig
    for ax in axes[nfig:]:
        ax.axis("off")

    plt.tight_layout()
    plt.savefig(out_path, format="svg")
    print(f"[plot] saved → {out_path}", flush=True)
    plt.close(fig)


def plot_results(raw: dict) -> None:
    """One PDF/SVG per OBSS-occupancy config — six total.

    Rendering is delegated to joanna_plots.plot_9838765 (the same function
    ieee-plot-test uses for the MATLAB side). occ_code 1..6 maps to
    OBSS_CONFIGS in order (sym10, sym40, sym70, asym10_40, asym10_70,
    asym40_70), matching joanna_plots' occ_labels table exactly.
    """
    rows = []
    for occ_code, (n1, n2, label, title) in enumerate(OBSS_CONFIGS, 1):
        for nl in NUM_LINKS_LIST:
            xs, lat_m, lat_e, p95_m = _gather_series(raw, nl, n1, n2)
            is_mlo = 0 if nl == 1 else 1
            for load, avg, p95 in zip(xs, lat_m, p95_m):
                rows.append({
                    "expOccupancyListNum": occ_code, "isMLO": is_mlo,
                    "expectedLoad": load,
                    "avg_latency_mean": avg, "latency_95p_mean": p95,
                })
    df = pd.DataFrame(rows)

    # joanna_plots.plot_9838765 saves each figure as f"{base_name}_{occ_lbl}"
    # in the same OBSS_CONFIGS order used above, so the Nth call corresponds
    # to the Nth config — reuse that order to restore ns-3's own filenames.
    save_order = iter(label for _, _, label, _ in OBSS_CONFIGS)

    def _save(fig, _name):
        out_path = os.path.join(PLOT_DIR, f"4_2_6_e2e_{next(save_order)}.svg")
        fig.savefig(out_path, format="svg")
        print(f"[plot] saved → {out_path}", flush=True)

    # End-to-end (FlowMonitor) delay -- same quantity MATLAB measured, so
    # this uses plot_9838765's default ylabel rather than an override.
    joanna_plots.plot_9838765(df, "4_2_6_e2e", _save)


# ---------------------------------------------------------------------------
#  Main sweep
# ---------------------------------------------------------------------------

def main() -> None:
    print("[*] Loading existing results …", flush=True)
    existing = load_results()
    loaded = sum(len(v) for v in existing.values())
    print(f"[*] {loaded} existing run-results found.", flush=True)

    # Build job list: (nl, n1, n2, load, rng)
    # Loads are expectedLoad x baseline maxThr for the link count (thesis).
    jobs = []
    for nl in NUM_LINKS_LIST:
        for n1, n2, _, _ in OBSS_CONFIGS:
            eff_n2 = n2 if nl == 2 else 0
            for load in loads_for_config(nl):
                for rng in range(1, RUNS + 1):
                    jobs.append((nl, n1, eff_n2, load, rng))

    # De-duplicate (SLO produces same eff_n2=0 for different n2 configs)
    seen = set()
    unique_jobs = []
    for job in jobs:
        key = job  # (nl, n1, eff_n2, load, rng)
        if key not in seen:
            seen.add(key)
            unique_jobs.append(job)
    jobs = unique_jobs

    print(f"[*] Total unique jobs: {len(jobs)}", flush=True)

    failed = []
    completed = 0

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as ex:
        futures = {ex.submit(run_job, nl, n1, n2, load, rng): (nl, n1, n2, load, rng)
                   for nl, n1, n2, load, rng in jobs}
        for future in as_completed(futures):
            completed += 1
            result = future.result()
            if result[5] is None:  # lat is None → failed
                failed.append(result[:5])
            if completed % 20 == 0:
                print(f"  [{completed}/{len(jobs)}] done", flush=True)

    print(f"[*] Parallel pass: {len(jobs) - len(failed)}/{len(jobs)} successful",
          flush=True)

    if RERUN_MISSING and failed:
        print(f"[*] Re-running {len(failed)} failed jobs …", flush=True)
        for i, (nl, n1, n2, load, rng) in enumerate(failed):
            run_job(nl, n1, n2, load, rng)
            if (i + 1) % 5 == 0:
                print(f"  [{i + 1}/{len(failed)}] re-run done", flush=True)

    raw = load_results()
    plot_results(raw)
    print("[*] All done!", flush=True)


if __name__ == "__main__":
    main()
