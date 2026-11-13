# Result data

Every figure folder has a `data/` subfolder holding the simulation output behind that
figure. The data is the final campaign: seven repetitions per parameter point in MATLAB, 4256
jobs over twelve scenarios, and ten seeds per parameter point in ns-3, run on
ns-3.46.1.

Three kinds of file appear:

| file | what it is |
| --- | --- |
| `matlab-runs.csv` | one row per MATLAB job |
| `matlab-figure.csv` | one row per parameter point, averaged over the seven repetitions |
| `ns-3-runs.csv` | one row per ns-3 run |

All three carry a header row naming every column.

The repository does not include the MATLAB aggregation and plotting scripts, so
`matlab-figure.csv` is the only form in which the plotted MATLAB values appear. The ns-3
drivers aggregate and plot from `ns-3-runs.csv` themselves.

## Which file belongs to which figure

| folder | paper | MATLAB runs | MATLAB figure data | ns-3 runs |
| --- | --- | --- | --- | --- |
| `fig-04-throughput-modes` | Fig. 4 | 385 | `matlab-figure.csv` | 50 |
| `fig-06-slo-mlo-coexistence` | Fig. 6 | 14 | `matlab-figure.csv` | 20 |
| `fig-07-obss-activity` | Fig. 7 | 462 | `matlab-figure.csv` | 660 |
| `fig-08-09-transmission-duration` | Figs. 8-9 | 336 | `matlab-figure.csv` | 960 |
| `fig-10-11-user-density-channel-width` | Figs. 10-11 | 420 | `matlab-figure.csv` | 1320 |
| `fig-12-load-without-contention` | Fig. 12 | 630 | `matlab-figure.csv` | 1670 |
| `fig-13-latency-modes` | Fig. 13 | see Fig. 4 | `matlab-figure.csv` | 1250 |
| `fig-15-load-under-contention` | Fig. 15 | 84 | `matlab-figure.csv` | 750 |
| `fig-17-link-assignment` | Fig. 17 | 63 | `matlab-figure.csv` | 1250 |
| `fig-19-20-channel-occupancy` | Figs. 19-20 | 336 | `matlab-figure.csv` | 480 |
| `fig-21-22-23-legacy-coexistence` | Figs. 21-23 | 126 | `matlab-figure-network.csv`, `matlab-figure-per-device.csv` | 231 |
| `fig-25-vr` | Fig. 25 | 980 | `matlab-figure.csv`, `matlab-latency.csv` | 1400 |
| `table-05-baseline-throughput` | Table 5 | - | - | 80 |

Two folders share runs with another:

- Figures 4 and 13 come from the same MATLAB sweep. The per-run data is in
  `fig-04-throughput-modes/data/matlab-runs.csv` for both.
- Three of the five bars in Figure 17 come from the Figure 15 sweep; the other two are in
  this folder's own `matlab-runs.csv`.

## What is not here

Table 5's MATLAB column comes from a separate saturation measurement, hard-coded as the
normalisation constant in `mlojobs.m`, and has no campaign data of its own.

The Discussion figures are summaries computed from the scenarios above and have no sweep
of their own.

The twelfth MATLAB scenario is the full-buffer half of the transmission-duration study.
No figure in the paper uses it, so neither it nor its 420 jobs appear here.

## `matlab-runs.csv`

Written one line per job by the runner, then concatenated in job-index order, so row `i`
is job `i`. Each parameter point occupies seven consecutive rows, one per repetition;
`mlojobs.m` gives the parameters of each job index.

Every file starts with `scenario,job,elapsed_s`: the scenario key, the job index and the
wall-clock seconds the job took. The columns after those are the numeric outputs of the
scenario function, flattened in the order they are declared, and they are named after the
output they come from. An output that is a vector occupies as many columns as it has
elements, numbered from one.

| folder | columns after `elapsed_s` |
| --- | --- |
| `fig-04-throughput-modes` | throughput (Mbit/s), average latency (s), two 99th-percentile latencies, one per station (s) |
| `fig-06-slo-mlo-coexistence` | throughput of the MLO BSS and of the two single-link BSSs (Mbit/s) |
| `fig-07-obss-activity` | throughput (Mbit/s) |
| `fig-08-09-transmission-duration` | throughput (Mbit/s), average latency (s) |
| `fig-10-11-user-density-channel-width` | average latency (s) |
| `fig-12-load-without-contention` | throughput (Mbit/s), median latency (s), 99th-percentile latency (s) |
| `fig-15-load-under-contention`, `fig-17-link-assignment` | throughput per BSS (4, Mbit/s), median latency per station (8, s), 99th-percentile latency per station (8, s) |
| `fig-19-20-channel-occupancy` | throughput (Mbit/s), the output declared `latencyMainSTA` (s), which the figure uses as the 95th percentile, average latency (s) |
| `fig-21-22-23-legacy-coexistence` | throughput per station (50, Mbit/s), multi-link stations first, then legacy ones |
| `fig-25-vr` | downlink latency (s), uplink latency (s), each compared against the Wi-Fi Alliance limit for its direction, 5 ms and 2 ms |

Not every column is used by a figure. Where a station is idle in a given configuration its
entry is `NaN`.

## `matlab-figure.csv`

One row per parameter point. A `_mean` column is the mean over the seven repetitions. A
`_std` column is the half-width of the 95% confidence interval, `1.96 * s / sqrt(n)`,
despite the name. Latencies are in milliseconds, throughput in Mbit/s.

Two files carry columns the figure does not use, written as zeros rather than measured:
`avg_latency_mean` and `avg_latency_std` in `fig-08-09-transmission-duration`, and
`thr_mean` and `thr_std` in `fig-15-load-under-contention` and `fig-17-link-assignment`.

`fig-25-vr/data/matlab-figure.csv` is the feasibility result the figure draws: for each
direction, mode and MCS, the narrowest channel that meets the limit, or `NaN` where none
does. `matlab-latency.csv` next to it holds the latency behind that test, one row per
direction, mode, bandwidth and MCS.

## `ns-3-runs.csv`

One row per run. `RngRun` is the seed, ten per parameter point. Throughput is in Mbit/s and latency in milliseconds. The remaining columns are the
sweep parameters, named as the driver sets them.

`fig-21-22-23-legacy-coexistence` is the exception: it has thirteen seeds at seventeen of
its eighteen points and ten at the remaining one. Its EMLSR runs also skip `RngRun`
values, because seeds on which the simulation crashed were replaced rather than
renumbered.
