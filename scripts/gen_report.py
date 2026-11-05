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
BOOK_ORDER = {"baseline": 0, "baseline_pool": 1, "fast": 2, "fast_4k": 3}


def load_citable():
    files = []
    for path in sorted((ROOT / "results").glob("*.json")):
        data = json.loads(path.read_text())
        if data.get("citable"):
            files.append((path, data))
    return files


def turbo_label(data):
    return {"0": "on", "1": "off"}.get(data["machine"]["no_turbo"], "unknown")


def source_link(path):
    return f"[file](../results/{path.name})"


def latest_per_book_configuration(files):
    """Keeps the newest result for each book, profile, build and turbo setting. Older files stay in results/."""
    latest = {}
    for path, data in files:
        if data.get("suite", "book") != "book":
            continue
        for entry in data["entries"]:
            key = (entry["profile"], BOOK_ORDER.get(entry["book"], 9), entry["book"], turbo_label(data), data["label"].split("_turbo")[0])
            latest[key] = (path, data, entry)
    return latest


def newest_of_suite(files, suite):
    matching = [(path, data) for path, data in files if data.get("suite") == suite]
    return matching[-1] if matching else None


def render_latency(rows):
    lines = ["### Latency\n"]
    lines.append("Medians across runs, in nanoseconds. Average comes from untimed runs, and percentiles from timed")
    lines.append("runs, which include the timer's own cost shown in the second-last column.\n")
    lines.append("| Workload | Book | Turbo | Average | p50 | p99 | p99.9 | p99.99 | Max | Timer p50 | Source |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for (profile, _, book, turbo, _), (path, _, entry) in rows:
        summary = entry["summary"]
        latency = summary["latency"]
        cells = [profile, book, turbo, f"{summary['mean_ns']['median']:.1f}"]
        cells += [f"{latency[key]['median']:.0f}" for key in ("p50_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns")]
        cells += [f"{summary['timer_p50_ns']:.1f}", source_link(path)]
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def render_by_kind(rows):
    lines = ["\n### Latency by kind of command\n"]
    lines.append("From the same timed runs as above, in nanoseconds, including the timer's cost.\n")
    lines.append("| Workload | Book | Turbo | Kind | Share | p50 | p99 | p99.9 | p99.99 |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for (profile, _, book, turbo, _), (_, _, entry) in rows:
        by_kind = entry["summary"].get("latency_by_kind")
        if by_kind is None or profile != "realistic":
            continue
        total = sum(values["samples"] for values in by_kind.values())
        for kind, values in by_kind.items():
            cells = [f"{values[key]:.0f}" for key in ("p50_ns", "p99_ns", "p99_9_ns", "p99_99_ns")]
            lines.append(f"| {profile} | {book} | {turbo} | {kind} | {values['samples'] / total:.1%} | " + " | ".join(cells) + " |")
    return lines


def render_counters(rows):
    lines = ["\n### Hardware counters per command\n"]
    lines.append("From untimed runs, counting user-mode events during the measured loop only.\n")
    lines.append("| Workload | Book | Turbo | Cycles | Instructions | L1d loads | L1d misses | LLC misses | Branch misses |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for (profile, _, book, turbo, _), (_, _, entry) in rows:
        perf = entry["summary"].get("perf_per_command")
        if perf is None:
            continue
        values = [perf[key] for key in ("cycles", "instructions", "l1d_loads", "l1d_load_misses", "llc_load_misses", "branch_misses")]
        lines.append("| " + " | ".join([profile, book, turbo] + [f"{value:.2f}" for value in values]) + " |")
    return lines


def render_comparison(files):
    """Compares books measured together in one file, so that machine conditions match."""
    lines = ["\n### Compared with the baseline\n"]
    lines.append("Reduction relative to `baseline`, from books measured in the same results file. L1d is misses per")
    lines.append("command. The `baseline_pool` row isolates what comes from avoiding the general-purpose allocator,")
    lines.append("and the `fast` row adds everything else that differs between the designs.\n")
    lines.append("| Workload | Book | Turbo | Average latency reduction | L1d miss reduction | L1d miss rate | Baseline L1d miss rate |")
    lines.append("|---|---|---|---|---|---|---|")
    newest = {}
    for path, data in files:
        if data.get("suite", "book") == "book" and data["label"].split("_turbo")[0] == "book":
            newest[turbo_label(data)] = data
    for turbo, data in sorted(newest.items()):
        by_profile = {}
        for entry in data["entries"]:
            by_profile.setdefault(entry["profile"], {})[entry["book"]] = entry
        for profile, books in by_profile.items():
            base = books.get("baseline")
            if base is None or "perf_per_command" not in base["summary"]:
                continue
            base_perf = base["summary"]["perf_per_command"]
            for book in ("baseline_pool", "fast"):
                other = books.get(book)
                if other is None or "perf_per_command" not in other["summary"]:
                    continue
                perf = other["summary"]["perf_per_command"]
                latency = 1 - other["summary"]["mean_ns"]["median"] / base["summary"]["mean_ns"]["median"]
                misses = 1 - perf["l1d_load_misses"] / base_perf["l1d_load_misses"]
                rate = perf["l1d_load_misses"] / perf["l1d_loads"]
                base_rate = base_perf["l1d_load_misses"] / base_perf["l1d_loads"]
                lines.append(
                    f"| {profile} | {book} | {turbo} | {latency:.1%} | {misses:.1%} | {rate:.2%} | {base_rate:.2%} |"
                )
    return lines


def render_workloads(rows):
    lines = ["\n### Workload contents\n"]
    lines.append("Measured from each generated log, including warmup.\n")
    lines.append("| Workload | Commands | Limit | Market | Cancel | Modify | Trades | Rejects | Peak resting | Best bid travel (ticks) |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    seen = set()
    for (profile, _, _, _, _), (_, _, entry) in rows:
        if profile in seen:
            continue
        seen.add(profile)
        workload = entry["workload"]
        total = sum(workload[f"{kind}_commands"] for kind in ("limit", "market", "cancel", "modify"))
        travel = workload["highest_best_bid"] - workload["lowest_best_bid"]
        lines.append(
            f"| {profile} | {total} | {workload['limit_commands']} | {workload['market_commands']} "
            f"| {workload['cancel_commands']} | {workload['modify_commands']} | {workload['trade_events']} "
            f"| {workload['rejected_events']} | {workload['max_resting_orders']} | {travel} |"
        )
    return lines


def render_ring(result):
    path, data = result
    lines = ["\n### SPSC ring\n"]
    lines.append(f"One item is one push and its matching pop. Ring capacity is 4,096 items. Turbo {turbo_label(data)}. {source_link(path)}\n")
    lines.append("| Payload (bytes) | Index alignment | Cached indices | Placement | Median items/s | Peak items/s |")
    lines.append("|---|---|---|---|---|---|")
    for entry in data["entries"]:
        if entry["kind"] != "throughput":
            continue
        rate = entry["summary"]["items_per_second"]
        lines.append(
            f"| {entry['payload_bytes']} | {entry['index_alignment']} | {'yes' if entry['cached_indices'] else 'no'} "
            f"| {entry['placement'].replace('_', ' ')} | {rate['median'] / 1e6:.1f}M | {rate['max'] / 1e6:.1f}M |"
        )
    lines.append("\nRound trip of one 32-byte item between two cores, through two rings, with no queue built up. Nanoseconds:\n")
    lines.append("| Index alignment | p50 | p99 | p99.9 | p99.99 | Max |")
    lines.append("|---|---|---|---|---|---|")
    for entry in data["entries"]:
        if entry["kind"] != "pingpong":
            continue
        trip = entry["summary"]["round_trip"]
        cells = [f"{trip[key]['median']:.0f}" for key in ("p50_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns")]
        lines.append(f"| {entry['index_alignment']} | " + " | ".join(cells) + " |")
    return lines


def render_pipeline(result):
    path, data = result
    lines = ["\n### Pipeline\n"]
    lines.append("Gateway thread, ring, and book on another core, on the `realistic` workload. Throughput is at saturation.")
    lines.append("Latency sends one command at a time, from just before the push to just after the book finishes.")
    lines.append(f"Turbo {turbo_label(data)}. {source_link(path)}\n")
    lines.append("| Book | Median commands/s | Peak commands/s | p50 ns | p99 ns | p99.9 ns | p99.99 ns | Max ns | Timer p50 ns |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for entry in data["entries"]:
        summary = entry["summary"]
        rate = summary["commands_per_second"]
        latency = summary["latency"]
        cells = [f"{latency[key]['median']:.0f}" for key in ("p50_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns")]
        lines.append(
            f"| {entry['book']} | {rate['median'] / 1e6:.1f}M | {rate['max'] / 1e6:.1f}M | " + " | ".join(cells) + f" | {summary['timer_p50_ns']:.1f} |"
        )
    return lines


def render_jitter(result):
    path, data = result
    lines = ["\n### Timer resolution and timing repeatability\n"]
    lines.append("One log replayed repeatedly, so that all variation comes from the machine. These are three different")
    lines.append(f"quantities and shouldn't be confused with one another. Turbo {turbo_label(data)}. {source_link(path)}\n")
    lines.append("| Book | One TSC tick | Timer p50 | Timer p99 | Run-to-run stddev of the mean | Per-command stddev, p50 | Per-command stddev, p99 |")
    lines.append("|---|---|---|---|---|---|---|")
    for entry in data["entries"]:
        summary = entry["summary"]
        timer = summary["timer"]
        per_command = summary["per_command"]
        lines.append(
            f"| {entry['book']} | {summary['tick_ns']:.3f} ns | {timer['p50_ns']:.1f} ns | {timer['p99_ns']:.1f} ns "
            f"| {summary['run_to_run_mean']['stddev_ns']:.3f} ns | {per_command['stddev_p50_ns']:.1f} ns | {per_command['stddev_p99_ns']:.1f} ns |"
        )
    return lines


def render_conditions(rows, extra):
    lines = ["\n### Conditions\n"]
    conditions = {}
    for _, (path, data, _) in rows:
        conditions[path.name] = data
    for result in extra:
        conditions[result[0].name] = result[1]
    for name, data in conditions.items():
        machine = data["machine"]
        lines.append(
            f"- `{name}`: {machine['cpu_model']}, {data['build']['compiler']}, flags `{data['build']['flags'].strip()}`, "
            f"governor {machine['governor']}, turbo {turbo_label(data)}, online CPUs {machine['online_cpus']}, "
            f"commit `{data['git']['sha'][:8]}`."
        )
    return lines


def main():
    files = load_citable()
    latest = latest_per_book_configuration(files)
    rows = sorted(latest.items())
    extra = [result for result in (newest_of_suite(files, suite) for suite in ("ring", "pipeline", "jitter")) if result]

    lines = []
    if rows:
        lines += render_latency(rows) + render_by_kind(rows) + render_counters(rows) + render_comparison(files) + render_workloads(rows)
    ring = newest_of_suite(files, "ring")
    pipeline = newest_of_suite(files, "pipeline")
    jitter = newest_of_suite(files, "jitter")
    if ring:
        lines += render_ring(ring)
    if pipeline:
        lines += render_pipeline(pipeline)
    if jitter:
        lines += render_jitter(jitter)
    lines += render_conditions(rows, extra)

    body = "\n".join(lines) if rows or extra else "No citable results yet."
    text = REPORT.read_text()
    before, rest = text.split(START, 1)
    _, after = rest.split(END, 1)
    REPORT.write_text(f"{before}{START}\n{body}\n{END}{after}")
    print(f"{len(rows)} book configurations and {len(extra)} other suites in {REPORT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
