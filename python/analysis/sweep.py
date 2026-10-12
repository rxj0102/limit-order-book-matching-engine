#!/usr/bin/env python3
"""Parameter sweep: risk aversion (gamma) x constant latency, many seeds per cell.

DATA SOURCE: every feed here is SYNTHETIC (lobkit.synthetic_feed, a seeded random order flow
generator). Nothing in the output is market data and nothing in it is evidence that the strategy
would make or lose money anywhere. The sweep exists to exercise the simulator and the analysis
code and to show how much results move from seed to seed. Patterns in these tables are not
findings about the strategy; see the limitations section written into the report.

Usage (from the repository root, after building with the python preset):

    PYTHONPATH=build/python/python python3 python/analysis/sweep.py --out docs/results/phase6

Outputs in --out: sweep_report.md (tables and limitations), sweep_runs.csv (one row per run),
environment.txt, and plots/*.png. Every figure is mean and spread across seeds; there is no
single run figure.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import csv
import datetime
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

import lobkit
from lobkit import _core as core

USD_PER_HALF_UNIT = 1.0 / (2.0 * core.PRICE_UNITS_PER_DOLLAR)
GRID_POINTS = 200

DEFAULT_GAMMAS = (0.01, 0.05, 0.1, 0.5, 1.0)
DEFAULT_LATENCIES_US = (0, 100, 1000, 5000, 10000)

LIMITATIONS = """\
## Limitations

Read these before any number above.

- The data is synthetic. The feed is a seeded random order flow generator. It has no real
  order arrival structure, no clustering, no information content and no market impact, so
  nothing here says how this strategy would behave on a real market.
- Latency is a constant for each of the three legs (submit, acknowledgement, market data). Real
  latency has jitter and tails, and jitter matters most when it reorders a cancel and a fill.
  A constant latency understates those effects.
- Hidden (non displayed) executions are not applied to the book in replay. A real quote can be
  filled by flow that this replay never shows, so fill rates are biased low.
- The fill model is an approximation: an own order fills only when a feed execution reaches
  the queue behind the feed orders that were ahead of it, and own fills do not change the feed.
  The artifact rate columns count the messages where the replayed book and the model disagreed.
- The order arrival intensity parameter k is not calibrated to any data. The default is a
  placeholder.
- Volatility sigma is a crude exponentially weighted estimate of the squared mid change per
  second over fixed intervals.
- Fees are zero in this sweep.
- Synthetic results are not evidence of profitability. No pattern in these tables or plots is
  interpreted as a finding about the strategy, and none should be.
"""


# ---- one run ---------------------------------------------------------------------------------------------

def resample_step(ts, values, grid):
    """Last observation carried forward onto `grid`; zero before the first sample."""
    idx = np.searchsorted(ts, grid, side="right") - 1
    out = np.where(idx >= 0, values[np.clip(idx, 0, None)], 0)
    return out


def run_cell(msgs, book, gamma, lat_us, grid_seconds):
    lat_ns = int(lat_us * 1000)
    sc = core.SimConfig(submit_latency_ns=lat_ns, ack_latency_ns=lat_ns, md_latency_ns=lat_ns,
                        maker_fee=0, taker_fee=0)
    ap = lobkit.strategy_params(book, gamma=gamma)
    r = core.run_simulation(book, sc, ap, msgs)
    p = r.path
    t0 = int(msgs["ts"][0])
    grid_ns = t0 + (np.asarray(grid_seconds) * 1e9).astype(np.uint64)
    comp = {}
    for name in ("spread2", "inventory2", "fees2", "total2", "inventory"):
        comp[name] = resample_step(p["ts"], p[name], grid_ns)
    scale = USD_PER_HALF_UNIT
    pnl = r.pnl
    nmsg = max(r.stats["feed_messages"], 1)
    bs = r.book_stats
    rec = {
        "total": pnl["total_dollars"], "spread": pnl["spread_capture_dollars"],
        "inventory_pnl": pnl["inventory_pnl_dollars"], "fees": pnl["fees_paid_dollars"],
        "fills": r.stats["fills"], "shares": r.stats["shares_filled"],
        "final_inventory": pnl["inventory"], "max_abs_inventory": r.stats["max_abs_inventory"],
        "orders_sent": r.stats["orders_sent"], "orders_rejected": r.stats["orders_rejected"],
        "anomalies": bs["anomalies"], "feed_problems": bs["feed_problems"],
        "anomaly_rate": bs["anomalies"] / nmsg, "rejected_feed_rate": bs["feed_problems"] / nmsg,
        "identity_holds": int(pnl["identity_holds"]),
    }
    curves = {k: v.astype(np.float64) * (1.0 if k == "inventory" else scale) for k, v in comp.items()}
    return rec, curves


def sweep(seeds, messages, gammas, lats_us, workers):
    feeds = []
    for s in seeds:
        m = core.synthetic_feed(seed=s, messages=messages)
        feeds.append((m, lobkit.book_config_for(m)))
    spans = [(int(m["ts"][-1]) - int(m["ts"][0])) / 1e9 for m, _ in feeds]
    grid_s = np.linspace(0.0, min(spans), GRID_POINTS)
    jobs = [(si, gi, li) for si in range(len(seeds)) for gi in range(len(gammas)) for li in range(len(lats_us))]
    results = {}

    def work(job):
        si, gi, li = job
        m, b = feeds[si]
        return job, run_cell(m, b, gammas[gi], lats_us[li], grid_s)

    # run_simulation releases the GIL, so threads run in parallel.
    with cf.ThreadPoolExecutor(max_workers=workers) as ex:
        for job, out in ex.map(work, jobs):
            results[job] = out
    return results, grid_s, spans


# ---- statistics and tables ------------------------------------------------------------------------------

def stat(x):
    x = np.asarray(x, dtype=np.float64)
    sd = float(np.std(x, ddof=1)) if x.size > 1 else float("nan")
    return float(np.mean(x)), sd, float(np.min(x)), float(np.max(x))


def fmt(x, digits=2):
    m, sd, lo, hi = stat(x)
    return f"{m:.{digits}f} ± {sd:.{digits}f} [{lo:.{digits}f}, {hi:.{digits}f}]"


def cell_values(results, nseeds, gi, li, key):
    return [results[(si, gi, li)][0][key] for si in range(nseeds)]


def table(results, nseeds, gammas, lats_us, key, digits=2):
    head = "| gamma \\ latency (us) | " + " | ".join(str(l) for l in lats_us) + " |"
    sep = "|---|" + "---|" * len(lats_us)
    rows = [head, sep]
    for gi, g in enumerate(gammas):
        rows.append(f"| {g} | " + " | ".join(fmt(cell_values(results, nseeds, gi, li, key), digits)
                                             for li in range(len(lats_us))) + " |")
    return "\n".join(rows)


def artifact_table(results, nseeds, gammas, lats_us):
    """Fill-model artifact rate: share of feed messages that were anomalies or rejected feed ops."""
    head = ("| gamma | latency (us) | anomaly rate (mean, max over seeds) | rejected-feed rate (mean, max) |\n"
            "|---|---|---|---|")
    rows = [head]
    for gi, g in enumerate(gammas):
        for li, l in enumerate(lats_us):
            a = cell_values(results, nseeds, gi, li, "anomaly_rate")
            f = cell_values(results, nseeds, gi, li, "rejected_feed_rate")
            rows.append(f"| {g} | {l} | {100*np.mean(a):.3f}%, {100*np.max(a):.3f}% | "
                        f"{100*np.mean(f):.3f}%, {100*np.max(f):.3f}% |")
    return "\n".join(rows)


def overall_artifact(results):
    a = [v[0]["anomaly_rate"] for v in results.values()]
    f = [v[0]["rejected_feed_rate"] for v in results.values()]
    return (f"fill-model artifact rate over all {len(a)} runs: anomalies {100*np.mean(a):.3f}% of feed "
            f"messages on average (max {100*np.max(a):.3f}%), rejected feed operations "
            f"{100*np.mean(f):.3f}% (max {100*np.max(f):.3f}%)")


# ---- plots -------------------------------------------------------------------------------------------------------

def make_plots(results, grid_s, gammas, lats_us, nseeds, messages, outdir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    src = f"SYNTHETIC random feed, {nseeds} seeds x {messages} messages"
    plots = outdir / "plots"
    plots.mkdir(parents=True, exist_ok=True)
    ref_g, ref_l = len(gammas) // 2, len(lats_us) // 2

    def curves(gi, li, key):
        return np.array([results[(si, gi, li)][1][key] for si in range(nseeds)])

    # 1. PnL curve with decomposition, reference cell
    fig, ax = plt.subplots(figsize=(8, 4.8))
    for key, label, sign in (("total2", "total", 1), ("spread2", "spread capture", 1),
                             ("inventory2", "inventory PnL", 1), ("fees2", "fees (negated)", -1)):
        c = sign * curves(ref_g, ref_l, key)
        mean, sd = c.mean(0), c.std(0, ddof=1) if nseeds > 1 else np.zeros(c.shape[1])
        ax.plot(grid_s, mean, label=label)
        ax.fill_between(grid_s, mean - sd, mean + sd, alpha=0.15)
    ax.set_xlabel("seconds since first feed message")
    ax.set_ylabel("cumulative PnL, dollars")
    ax.set_title(f"PnL and decomposition, gamma={gammas[ref_g]}, latency={lats_us[ref_l]} us\n"
                 f"mean and 1 sd band over seeds. Data: {src}", fontsize=9)
    ax.legend(); fig.tight_layout(); fig.savefig(plots / "pnl_decomposition.png", dpi=130); plt.close(fig)

    # 2. inventory distribution by gamma at the reference latency (pooled over seeds and time)
    fig, ax = plt.subplots(figsize=(8, 4.8))
    for gi, g in enumerate(gammas):
        inv = curves(gi, ref_l, "inventory").ravel()
        ax.hist(inv, bins=np.arange(inv.min() - 0.5, inv.max() + 1.5, 20 if np.ptp(inv) > 400 else 10),
                histtype="step", density=True, label=f"gamma={g}")
    ax.set_xlabel("inventory, shares"); ax.set_ylabel("share of sampled time points (density)")
    ax.set_title(f"Inventory distribution, latency={lats_us[ref_l]} us, pooled over seeds and time\n"
                 f"Data: {src}", fontsize=9)
    ax.legend(); fig.tight_layout(); fig.savefig(plots / "inventory_distribution.png", dpi=130); plt.close(fig)

    # 3. spread capture vs inventory PnL (adverse selection proxy), one marker per cell
    fig, ax = plt.subplots(figsize=(7.5, 5.5))
    cmap = plt.get_cmap("viridis")
    for gi, g in enumerate(gammas):
        for li, l in enumerate(lats_us):
            sc_ = cell_values(results, nseeds, gi, li, "spread")
            ip = cell_values(results, nseeds, gi, li, "inventory_pnl")
            ax.errorbar(np.mean(sc_), np.mean(ip), xerr=np.std(sc_, ddof=1), yerr=np.std(ip, ddof=1),
                        fmt="o", ms=4 + 2 * li, color=cmap(gi / max(len(gammas) - 1, 1)), alpha=0.7,
                        label=f"gamma={g}" if li == 0 else None)
    ax.axhline(0, color="grey", lw=0.5); ax.axvline(0, color="grey", lw=0.5)
    ax.set_xlabel("spread capture, dollars (mean ± sd over seeds)")
    ax.set_ylabel("inventory PnL, dollars (mean ± sd over seeds)")
    ax.set_title("Spread capture vs inventory PnL (includes adverse selection and noise)\n"
                 f"marker size grows with latency. Data: {src}", fontsize=9)
    ax.legend(); fig.tight_layout(); fig.savefig(plots / "spread_vs_adverse_selection.png", dpi=130); plt.close(fig)

    # 4. sensitivity to latency and to risk aversion
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.6))
    xs = np.arange(len(lats_us))
    for gi, g in enumerate(gammas):
        tot = [cell_values(results, nseeds, gi, li, "total") for li in range(len(lats_us))]
        axes[0].errorbar(xs, [np.mean(t) for t in tot], yerr=[np.std(t, ddof=1) for t in tot], marker="o",
                         capsize=3, label=f"gamma={g}")
    axes[0].set_xticks(xs, [str(l) for l in lats_us])
    axes[0].set_xlabel("constant one-way latency, us (equal on all three legs)")
    axes[0].set_ylabel("total PnL, dollars (mean ± sd over seeds)")
    axes[0].set_title("Sensitivity to latency", fontsize=9); axes[0].legend(fontsize=7)
    gx = np.arange(len(gammas))
    for li, l in enumerate(lats_us):
        tot = [cell_values(results, nseeds, gi, li, "total") for gi in range(len(gammas))]
        axes[1].errorbar(gx, [np.mean(t) for t in tot], yerr=[np.std(t, ddof=1) for t in tot], marker="o",
                         capsize=3, label=f"latency={l} us")
    axes[1].set_xticks(gx, [str(g) for g in gammas])
    axes[1].set_xlabel("risk aversion gamma"); axes[1].set_ylabel("total PnL, dollars (mean ± sd over seeds)")
    axes[1].set_title("Sensitivity to risk aversion", fontsize=9); axes[1].legend(fontsize=7)
    fig.suptitle(f"Data: {src}", fontsize=9)
    fig.tight_layout(); fig.savefig(plots / "sensitivity.png", dpi=130); plt.close(fig)
    return sorted(p.name for p in plots.glob("*.png"))


# ---- environment and report --------------------------------------------------------------------------------------

def environment_text(args, wall_s, spans):
    def sh(cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout.strip()
        except Exception:
            return "unknown"
    cpu = "unknown"
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip(); break
    except OSError:
        pass
    import matplotlib
    lines = [
        f"date (UTC): {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}",
        f"git commit: {sh(['git', 'rev-parse', '--short', 'HEAD'])}",
        f"cpu: {cpu}, logical cpus: {os.cpu_count()}",
        f"os: {platform.platform()}",
        f"virtualization: KVM guest VM, no governor control, timings are indicative only",
        f"python: {platform.python_version()}, numpy {np.__version__}, matplotlib {matplotlib.__version__}",
        f"compiler of the extension: {sh(['c++', '--version']).splitlines()[0] if sh(['c++', '--version']) else 'unknown'}",
        f"sweep wall time: {wall_s:.1f} s with {args.workers} worker threads "
        f"({args.seeds} seeds x {len(args.gammas)} gammas x {len(args.latencies_us)} latencies = "
        f"{args.seeds*len(args.gammas)*len(args.latencies_us)} simulations of {args.messages} messages)",
        f"feed span per seed: {min(spans):.1f} to {max(spans):.1f} seconds",
        f"command: {' '.join(sys.argv)}",
    ]
    return "\n".join(lines) + "\n"


def write_report(path, args, results, plot_names, env_text):
    n = args.seeds
    src = (f"SYNTHETIC. Seeded random order flow (lobkit.synthetic_feed), seeds 1 to {n}, "
           f"{args.messages} messages per seed.")
    parts = [
        "# Phase 6 sweep report (SYNTHETIC data)", "",
        f"Data source: {src}", "",
        "Each cell is mean ± sample standard deviation over seeds, with [min, max] over seeds. "
        "Latency is the same constant one-way value on the submit, acknowledgement and market data legs. "
        "Fees are zero. Dollar figures are for the whole synthetic session.", "",
        "Inventory PnL is the mark to mid component. It contains adverse selection but also plain "
        "price noise, so it is only a proxy for adverse selection.", "",
        "## Fill-model artifact rate", "",
        "Share of feed messages that were anomalies (queue order anomalies, feed adds that crossed a "
        "resting order, feed executes larger than the order) or rejected feed operations. These are the "
        "places where the replayed book and the fill model disagreed with the feed. A cell with a high rate "
        "says more about the fill model than about the strategy.", "",
        overall_artifact(results), "", artifact_table(results, n, args.gammas, args.latencies_us), "",
    ]
    for key, title, digits in (("total", "Total PnL, dollars", 2), ("spread", "Spread capture, dollars", 2),
                               ("inventory_pnl", "Inventory PnL, dollars", 2), ("fills", "Fills", 1),
                               ("max_abs_inventory", "Max absolute inventory, shares", 0)):
        parts += [f"## {title}", "", f"Data: {src}", "", table(results, n, args.gammas, args.latencies_us, key, digits),
                  "", f"Fill-model artifact rate for this table: {overall_artifact(results).split(': ', 1)[1]}", ""]
    parts += ["## Plots", "", f"Every title carries the data source. Files in plots/: {', '.join(plot_names)}.", ""]
    parts += [f"- ![{p}](plots/{p})" for p in plot_names]
    parts += ["", LIMITATIONS, "## Environment", "", "```", env_text.rstrip(), "```", ""]
    Path(path).write_text("\n".join(parts))


def write_raw(path, results, seeds, gammas, lats_us):
    keys = list(next(iter(results.values()))[0].keys())
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seed", "gamma", "latency_us"] + keys)
        for (si, gi, li), (rec, _) in sorted(results.items()):
            w.writerow([seeds[si], gammas[gi], lats_us[li]] + [repr(rec[k]) if isinstance(rec[k], float) else rec[k] for k in keys])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seeds", type=int, default=30)
    ap.add_argument("--messages", type=int, default=100000)
    ap.add_argument("--gammas", type=float, nargs="+", default=list(DEFAULT_GAMMAS))
    ap.add_argument("--latencies-us", type=float, nargs="+", default=list(DEFAULT_LATENCIES_US))
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--out", default="docs/results/phase6")
    args = ap.parse_args(argv)
    if args.seeds < 2:
        ap.error("at least 2 seeds are needed to report a spread")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    seeds = list(range(1, args.seeds + 1))
    t0 = time.perf_counter()
    results, grid_s, spans = sweep(seeds, args.messages, args.gammas, args.latencies_us, args.workers)
    wall = time.perf_counter() - t0
    bad = [k for k, v in results.items() if not v[0]["identity_holds"]]
    if bad:
        raise SystemExit(f"PnL identity failed in {len(bad)} runs, first {bad[0]}")
    plots = make_plots(results, grid_s, args.gammas, args.latencies_us, args.seeds, args.messages, out)
    env = environment_text(args, wall, spans)
    (out / "environment.txt").write_text(env)
    write_raw(out / "sweep_runs.csv", results, seeds, args.gammas, args.latencies_us)
    write_report(out / "sweep_report.md", args, results, plots, env)
    print(overall_artifact(results))
    print(f"sweep wall time {wall:.1f} s; wrote {out}")


if __name__ == "__main__":
    main()
