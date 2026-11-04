#!/usr/bin/env python3
"""Regenerates the results section of docs/BENCHMARKS.md from the citable files in results/.

Tables come only from this script, so every published number traces back to a committed results file.
"""

import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPORT = ROOT / "docs/BENCHMARKS.md"
START = "<!-- results:start -->"
END = "<!-- results:end -->"


def load_citable():
    files = []
    for path in sorted((ROOT / "results").glob("*.json")):
        data = json.loads(path.read_text())
        if data.get("citable"):
            files.append((path, data))
    return files


def turbo_label(data):
    return {"0": "on", "1": "off"}.get(data["machine"]["no_turbo"], "unknown")


def latest_per_configuration(files):
    """Keeps the newest result for each book, profile and turbo setting. Older files stay in results/."""
    latest = {}
    for path, data in files:
        for entry in data["entries"]:
            latest[(entry["book"], entry["profile"], turbo_label(data))] = (path, data, entry)
    return latest


def render_section(rows):
    lines = []
    lines.append("### Latency and throughput\n")
    lines.append("Medians across runs. Average comes from untimed runs and percentiles from timed runs, which")
    lines.append("include the timer's own cost, shown in the last column. All values are nanoseconds.\n")
    lines.append("| Book | Workload | Turbo | Average | p50 | p99 | p99.9 | p99.99 | Max | Timer p50 | Source |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for (book, profile, turbo), (path, _, entry) in rows:
        summary = entry["summary"]
        latency = summary["latency"]
        cells = [book, profile, turbo, f"{summary['mean_ns']['median']:.1f}"]
        cells += [f"{latency[key]['median']:.0f}" for key in ("p50_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns")]
        cells += [f"{summary['timer_p50_ns']:.1f}", f"[file](../results/{path.name})"]
        lines.append("| " + " | ".join(cells) + " |")

    lines.append("\n### Hardware counters per command\n")
    lines.append("From untimed runs, counting user-mode events during the measured loop only.\n")
    lines.append("| Book | Workload | Turbo | Cycles | Instructions | L1d loads | L1d misses | LLC misses | Branch misses |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for (book, profile, turbo), (_, _, entry) in rows:
        perf = entry["summary"].get("perf_per_command")
        if perf is None:
            continue
        values = [perf[key] for key in ("cycles", "instructions", "l1d_loads", "l1d_load_misses", "llc_load_misses", "branch_misses")]
        lines.append("| " + " | ".join([book, profile, turbo] + [f"{value:.2f}" for value in values]) + " |")

    lines.append("\n### Workload contents\n")
    lines.append("Measured from each generated log, including warmup.\n")
    lines.append("| Workload | Commands | Limit | Market | Cancel | Modify | Trades | Rejects | Peak resting |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    seen = set()
    for (_, profile, _), (_, _, entry) in rows:
        if profile in seen:
            continue
        seen.add(profile)
        workload = entry["workload"]
        total = sum(workload[f"{kind}_commands"] for kind in ("limit", "market", "cancel", "modify"))
        lines.append(
            f"| {profile} | {total} | {workload['limit_commands']} | {workload['market_commands']} "
            f"| {workload['cancel_commands']} | {workload['modify_commands']} | {workload['trade_events']} "
            f"| {workload['rejected_events']} | {workload['max_resting_orders']} |"
        )

    lines.append("\n### Conditions\n")
    conditions = {}
    for _, (path, data, _) in rows:
        conditions[path.name] = data
    for name, data in conditions.items():
        machine = data["machine"]
        lines.append(
            f"- `{name}`: {machine['cpu_model']}, {data['build']['compiler']}, flags `{data['build']['flags'].strip()}`, "
            f"governor {machine['governor']}, turbo {turbo_label(data)}, online CPUs {machine['online_cpus']}, "
            f"commit `{data['git']['sha'][:8]}`."
        )
    return "\n".join(lines)


def main():
    latest = latest_per_configuration(load_citable())
    rows = sorted(latest.items())
    body = render_section(rows) if rows else "No citable results yet."
    text = REPORT.read_text()
    before, rest = text.split(START, 1)
    _, after = rest.split(END, 1)
    REPORT.write_text(f"{before}{START}\n{body}\n{END}{after}")
    print(f"{len(rows)} configurations in {REPORT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
