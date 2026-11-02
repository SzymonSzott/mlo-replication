# Simulation code for "A Replication Study of IEEE 802.11 Multi-Link Operation Performance Research"

Joanna Koziol, Pawel Wityk, Szymon Szott (AGH University of Krakow) and Stefano Avallone
(University of Naples).

Simulation scripts for the results in the paper, which replicates 13 MLO scenarios from
eight studies in the MATLAB WLAN Toolbox and in ns-3.

## Layout

One folder per figure in the paper, named after its number; figures from a single sweep
share a folder. Each has a `matlab/` and an `ns-3/` subfolder.

Scripts are named after the figure: `figNN_<name>.m` is the MATLAB scenario,
`figNN_<name>.cc` the ns-3 scenario, and `figNN_<name>_run.py` or `_run.sh` the driver
that sweeps its parameters. A sweep feeding two figures carries both numbers. The
`mlojobs` cases use the same keys.

`fig04_13_modes.m` and `fig15_load_under_contention.m` each feed two figures and appear in
both of their folders.

`common/matlab/` contains the node placement and traffic functions in `supporting_func/`,
and `mlojobs.m`, which returns the parameter combinations of a scenario.

Figures with no folder are topology and protocol illustrations, or Discussion summaries
computed from the scenarios above.

## MATLAB

Requires MATLAB R2024b with the WLAN Toolbox and the Wireless Network Simulation Library
support package.

Each `figNN_*.m` is a function returning the throughput, the latency, or both for one
parameter combination, depending on what the figure needs. `mlojobs.m` lists the
combinations, so job `i` can be run on its own:

```matlab
addpath(genpath('common/matlab'), 'fig-07-obss-activity/matlab');
C = mlojobs("fig07");
job = C{i};
out = cell(1, nargout(job.f));
[out{:}] = feval(job.f, job.a{:});
```

The scenarios also use the MathWorks helper classes from the example "802.11be
System-Level Simulation Using STR Multi-Link Operation" (`hPerformanceViewer`,
`hSLSTGaxMultiFrequencySystemChannel` and the classes they depend on). These are
MathWorks-copyrighted and are not redistributed here. Open that example in MATLAB and add
them to the path alongside `common/matlab`.

## ns-3

Copy the `.cc` file into the `scratch/` directory of an ns-3 tree, build, then run the
driver. The drivers contain build paths from the machine the simulations were run on, so
adjust those first.

`fig-10-11`, `fig-21-22-23` and `fig-25` were run with ns-3.46.1, the rest with ns-3.45.

## Earlier repositories

The code here comes from two repositories written during the study:

- **MATLAB:** <https://github.com/jokozo/MLO-Performance-Analysis>, by Joanna Koziol.
- **ns-3:** <https://github.com/pawelwityk/mlo-wifi7-perf-tests>, by Pawel Wityk.
