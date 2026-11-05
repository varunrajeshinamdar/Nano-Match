#!/usr/bin/env python3
"""Runs benchmarks repeatedly and records raw results together with the conditions they ran under.

Each result file says whether it is citable. A run is citable only from a clean release build on AC power
with the performance governor. Anything else is an experiment, and the file says why.
"""

import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CPU_ROOT = pathlib.Path("/sys/devices/system/cpu")

# Measured commands per profile. The primary profile runs long enough to cover more than 10M new orders.
MEASURED_COMMANDS = {
    "realistic": 25_000_000,
    "add_cancel": 5_000_000,
    "sweep": 5_000_000,
    "realistic_1k": 5_000_000,
    "realistic_100k": 5_000_000,
    "realistic_1m": 5_000_000,
}
WARMUP_COMMANDS = 5_000_000
SEED = 1
MODES = ("untimed", "timed")
PIPELINE_MODES = ("throughput", "latency")
RING_ITEMS = 50_000_000
PINGPONG_ITEMS = 5_000_000
# The ring moves one buffer's worth of items per scheduler slice when both threads share a core.
SAME_CORE_ITEMS = 2_000_000
JITTER_MEASURED_COMMANDS = 1_000_000
JITTER_REPLAYS = 20
GENERATOR_SOURCES = (
    "src/workload/generator.hpp",
    "src/workload/rng.hpp",
    "src/baseline/baseline_book.hpp",
    "include/nanomatch/book.hpp",
    "include/nanomatch/protocol.hpp",
    "include/nanomatch/types.hpp",
    "src/log_io.cpp",
    "tools/nm_gen.cpp",
)


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, capture_output=True, cwd=ROOT, **kwargs).stdout


def read_text(path, default="unknown"):
    try:
        return pathlib.Path(path).read_text().strip()
    except OSError:
        return default


def cmake_cache(build_dir):
    cache = {}
    for line in (build_dir / "CMakeCache.txt").read_text().splitlines():
        if line and line[0] not in "#/" and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    return cache


def describe_build(build_dir):
    cache = cmake_cache(build_dir)
    compiler = cache["CMAKE_CXX_COMPILER"]
    build_type = cache["CMAKE_BUILD_TYPE"]
    return {
        "directory": str(build_dir.relative_to(ROOT)),
        "type": build_type,
        "compiler": run([compiler, "--version"]).splitlines()[0],
        "flags": cache.get("CMAKE_CXX_FLAGS", "") + " " + cache.get(f"CMAKE_CXX_FLAGS_{build_type.upper()}", ""),
    }


def describe_git():
    sha = run(["git", "rev-parse", "HEAD"]).strip()
    # Earlier result files don't affect what's being measured, so they don't make the tree dirty.
    changes = run(["git", "status", "--porcelain", "--", ".", ":(exclude)results"])
    return {"sha": sha, "dirty": bool(changes.strip())}


def describe_machine(cpu):
    cpu_model = next(
        line.split(":", 1)[1].strip() for line in read_text("/proc/cpuinfo").splitlines() if line.startswith("model name")
    )
    supplies = [read_text(path) for path in pathlib.Path("/sys/class/power_supply").glob("*/online")]
    siblings = read_text(CPU_ROOT / f"cpu{cpu}/topology/thread_siblings_list") if cpu is not None else "unknown"
    return {
        "cpu_model": cpu_model,
        "kernel": platform.release(),
        "cmdline": read_text("/proc/cmdline"),
        "governor": read_text(CPU_ROOT / f"cpu{cpu if cpu is not None else 0}/cpufreq/scaling_governor"),
        "no_turbo": read_text(CPU_ROOT / "intel_pstate/no_turbo"),
        "online_cpus": read_text(CPU_ROOT / "online"),
        "pinned_core_thread_siblings": siblings,
        "transparent_hugepages": read_text("/sys/kernel/mm/transparent_hugepage/enabled"),
        "perf_event_paranoid": read_text("/proc/sys/kernel/perf_event_paranoid"),
        "nmi_watchdog": read_text("/proc/sys/kernel/nmi_watchdog"),
        "ac_power": any(value == "1" for value in supplies),
        "load_average_at_start": read_text("/proc/loadavg"),
    }


def not_citable_reasons(git, build, machine, requested_runs, smoke):
    reasons = []
    if smoke:
        reasons.append("smoke run")
    if git["dirty"]:
        reasons.append("uncommitted changes")
    if build["type"] != "Release":
        reasons.append(f"build type is {build['type']}")
    if machine["governor"] != "performance":
        reasons.append(f"governor is {machine['governor']}")
    if not machine["ac_power"]:
        reasons.append("not on AC power")
    if requested_runs < 3:
        reasons.append("fewer than 3 runs")
    return reasons


def generator_fingerprint():
    """Identifies everything that can change a generated log, so a stale log is never reused."""
    digest = hashlib.sha256()
    for name in GENERATOR_SOURCES:
        digest.update((ROOT / name).read_bytes())
    return digest.hexdigest()[:12]


def ensure_log(build_dir, data_dir, profile, seed, commands):
    path = data_dir / f"{profile}_s{seed}_n{commands}_{generator_fingerprint()}.nmc"
    if not path.exists():
        print(f"generating {path.name}", flush=True)
        data_dir.mkdir(exist_ok=True)
        partial = path.with_suffix(".partial")
        run(
            [
                str(build_dir / "tools/nm_gen"),
                "--profile", profile,
                "--seed", str(seed),
                "--commands", str(commands),
                "--out", str(partial),
            ]
        )
        shutil.move(str(partial) + ".json", str(path) + ".json")
        partial.rename(path)
    return path


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        while chunk := file.read(1 << 24):
            digest.update(chunk)
    return digest.hexdigest()


def run_once(build_dir, book, profile, mode, log, warmup, cpu):
    output = run(
        [
            str(build_dir / "bench/bench_book"),
            "--log", str(log),
            "--profile", profile,
            "--book", book,
            "--mode", mode,
            "--warmup", str(warmup),
            "--cpu", "none" if cpu is None else str(cpu),
        ]
    )
    return json.loads(output)


def check_consistency(records):
    """Every run of one book on one log must do identical work, whatever the mode."""
    identities = {(record["events"], record["sink_hash"]) for record in records}
    if len(identities) != 1:
        sys.exit(f"runs disagree about the work performed: {identities}")


def summarize(records):
    untimed = [record for record in records if record["mode"] == "untimed"]
    timed = [record for record in records if record["mode"] == "timed"]
    summary = {}
    if untimed:
        means = [record["mean_ns"] for record in untimed]
        summary["mean_ns"] = {"median": statistics.median(means), "min": min(means), "max": max(means)}
        rates = [record["commands_per_second"] for record in untimed]
        summary["commands_per_second"] = {"median": statistics.median(rates), "min": min(rates), "max": max(rates)}
        samples = [record["perf"] for record in untimed if record["perf"]["available"]]
        if samples:
            commands = untimed[0]["measured_commands"]
            summary["perf_per_command"] = {
                key: statistics.median(sample[key] for sample in samples) / commands
                for key in ("cycles", "instructions", "l1d_loads", "l1d_load_misses", "llc_load_misses", "branch_misses")
            }
            summary["perf_min_running_fraction"] = min(sample["running_fraction"] for sample in samples)
    if timed:
        keys = ("mean_ns", "p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "p99_999_ns", "max_ns")
        summary["latency"] = {
            key: {
                "median": statistics.median(record["latency"][key] for record in timed),
                "min": min(record["latency"][key] for record in timed),
                "max": max(record["latency"][key] for record in timed),
            }
            for key in keys
        }
        summary["timer_p50_ns"] = statistics.median(record["timer"]["p50_ns"] for record in timed)
        summary["latency_by_kind"] = {
            kind: {
                "samples": timed[0]["latency_by_kind"][kind]["samples"],
                **{
                    key: statistics.median(record["latency_by_kind"][kind][key] for record in timed)
                    for key in ("mean_ns", "p50_ns", "p99_ns", "p99_9_ns", "p99_99_ns")
                },
            }
            for kind in timed[0]["latency_by_kind"]
        }
    return summary


def print_summary(book, profile, summary):
    print(f"{book} / {profile}:")
    if "mean_ns" in summary:
        mean = summary["mean_ns"]
        print(f"  untimed mean ns:  {mean['median']:.1f} (range {mean['min']:.1f} to {mean['max']:.1f})")
    if "latency" in summary:
        latency = summary["latency"]
        print("  timed ns:         " + "  ".join(f"{key[:-3]}={latency[key]['median']:.0f}" for key in latency))
        print(f"  timer p50 ns:     {summary['timer_p50_ns']:.1f}")
        for kind, values in summary["latency_by_kind"].items():
            print(f"    {kind:11} n={values['samples']:<9} p50={values['p50_ns']:.0f} p99={values['p99_ns']:.0f} p99.9={values['p99_9_ns']:.0f}")
    if "perf_per_command" in summary:
        perf = summary["perf_per_command"]
        print("  per command:      " + "  ".join(f"{key}={value:.2f}" for key, value in perf.items()))


def spread(values):
    values = list(values)
    return {"median": statistics.median(values), "min": min(values), "max": max(values)}


def run_book_suite(args, build_dir, data_dir, cpu):
    entries = []
    for profile in args.profiles.split(","):
        measured = 20_000 if args.smoke else MEASURED_COMMANDS[profile]
        warmup = 5_000 if args.smoke else WARMUP_COMMANDS
        log = ensure_log(build_dir, data_dir, profile, SEED, warmup + measured)
        workload = json.loads(pathlib.Path(str(log) + ".json").read_text())
        log_sha256 = sha256_of(log)
        for book in args.books.split(","):
            print(f"running {book} / {profile}", flush=True)
            records = []
            # Alternating modes spreads any slow drift, such as heat, across both of them.
            for _ in range(args.runs):
                for mode in MODES:
                    records.append(run_once(build_dir, book, profile, mode, log, warmup, cpu))
            check_consistency(records)
            summary = summarize(records)
            print_summary(book, profile, summary)
            entries.append(
                {
                    "book": book,
                    "profile": profile,
                    "log": log.name,
                    "log_sha256": log_sha256,
                    "workload": workload,
                    "summary": summary,
                    "runs": records,
                }
            )
    return entries


def run_ring(build_dir, mode, payload, alignment, cached, items, producer_cpu, consumer_cpu):
    output = run(
        [
            str(build_dir / "bench/bench_ring"),
            "--mode", mode,
            "--payload", str(payload),
            "--alignment", str(alignment),
            "--cached", "1" if cached else "0",
            "--items", str(items),
            "--producer-cpu", "none" if producer_cpu is None else str(producer_cpu),
            "--consumer-cpu", "none" if consumer_cpu is None else str(consumer_cpu),
        ]
    )
    return json.loads(output)


def ring_throughput_entry(args, build_dir, payload, alignment, cached, placement, items, producer_cpu, consumer_cpu):
    print(f"ring throughput: {payload} bytes, alignment {alignment}, cached={cached}, {placement}", flush=True)
    records = [
        run_ring(build_dir, "throughput", payload, alignment, cached, items, producer_cpu, consumer_cpu)
        for _ in range(args.runs)
    ]
    rate = spread(record["result"]["items_per_second"] for record in records)
    print(f"  items/s: median {rate['median']:.0f} (range {rate['min']:.0f} to {rate['max']:.0f})")
    return {
        "kind": "throughput",
        "payload_bytes": payload,
        "index_alignment": alignment,
        "cached_indices": cached,
        "placement": placement,
        "summary": {"items_per_second": rate},
        "runs": records,
    }


def run_ring_suite(args, build_dir, cpu, peer_cpu):
    items = 200_000 if args.smoke else RING_ITEMS
    pingpong_items = 20_000 if args.smoke else PINGPONG_ITEMS
    same_core_items = 20_000 if args.smoke else SAME_CORE_ITEMS
    entries = []
    for payload in (8, 32):
        for alignment in (8, 64, 128):
            for cached in (False, True):
                entries.append(
                    ring_throughput_entry(args, build_dir, payload, alignment, cached, "cross_core", items, peer_cpu, cpu)
                )

    # The intended layout with the threads placed differently, to show how much placement matters.
    if cpu is not None:
        siblings = read_text(CPU_ROOT / f"cpu{cpu}/topology/thread_siblings_list")
        sibling = next((int(value) for value in siblings.replace("-", ",").split(",") if int(value) != cpu), None)
        if sibling is not None:
            entries.append(
                ring_throughput_entry(args, build_dir, 32, 64, True, "hyperthread_sibling", items, sibling, cpu)
            )
        entries.append(
            ring_throughput_entry(args, build_dir, 32, 64, True, "same_core", same_core_items, cpu, cpu)
        )

    for alignment in (8, 64):
        print(f"ring ping-pong: alignment {alignment}", flush=True)
        records = [
            run_ring(build_dir, "pingpong", 32, alignment, True, pingpong_items, peer_cpu, cpu)
            for _ in range(args.runs)
        ]
        keys = ("mean_ns", "p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns")
        summary = {
            "round_trip": {
                key: spread(record["result"]["round_trip"][key] for record in records) for key in keys
            }
        }
        print(f"  round trip p50 ns: {summary['round_trip']['p50_ns']['median']:.0f}")
        entries.append(
            {
                "kind": "pingpong",
                "payload_bytes": 32,
                "index_alignment": alignment,
                "cached_indices": True,
                "placement": "cross_core",
                "summary": summary,
                "runs": records,
            }
        )
    return entries


def run_pipeline_suite(args, build_dir, data_dir, cpu, peer_cpu):
    profile = "realistic"
    measured = 20_000 if args.smoke else MEASURED_COMMANDS[profile]
    warmup = 5_000 if args.smoke else WARMUP_COMMANDS
    log = ensure_log(build_dir, data_dir, profile, SEED, warmup + measured)
    log_sha256 = sha256_of(log)
    entries = []
    for book in args.books.split(","):
        records = []
        for _ in range(args.runs):
            for mode in PIPELINE_MODES:
                print(f"pipeline: {book} {mode}", flush=True)
                output = run(
                    [
                        str(build_dir / "bench/bench_pipeline"),
                        "--log", str(log),
                        "--profile", profile,
                        "--book", book,
                        "--mode", mode,
                        "--warmup", str(warmup),
                        "--gateway-cpu", "none" if peer_cpu is None else str(peer_cpu),
                        "--engine-cpu", "none" if cpu is None else str(cpu),
                    ]
                )
                records.append(json.loads(output))
        check_consistency(records)
        throughput = [record for record in records if record["mode"] == "throughput"]
        latency = [record for record in records if record["mode"] == "latency"]
        keys = ("mean_ns", "p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns")
        summary = {
            "commands_per_second": spread(record["result"]["commands_per_second"] for record in throughput),
            "latency": {key: spread(record["result"]["latency"][key] for record in latency) for key in keys},
            "timer_p50_ns": statistics.median(record["timer"]["p50_ns"] for record in records),
        }
        print(
            f"  {book}: commands/s median {summary['commands_per_second']['median']:.0f}, "
            f"latency p50 ns {summary['latency']['p50_ns']['median']:.0f}"
        )
        entries.append(
            {
                "book": book,
                "profile": profile,
                "log": log.name,
                "log_sha256": log_sha256,
                "summary": summary,
                "runs": records,
            }
        )
    return entries


def run_jitter_suite(args, build_dir, data_dir, cpu):
    profile = "realistic"
    measured = 20_000 if args.smoke else MEASURED_COMMANDS[profile]
    warmup = 5_000 if args.smoke else WARMUP_COMMANDS
    log = ensure_log(build_dir, data_dir, profile, SEED, warmup + measured)
    jitter_measured = 10_000 if args.smoke else JITTER_MEASURED_COMMANDS
    replays = 3 if args.smoke else JITTER_REPLAYS
    entries = []
    for book in args.books.split(","):
        print(f"jitter: {book}", flush=True)
        records = []
        for _ in range(args.runs):
            output = run(
                [
                    str(build_dir / "bench/bench_jitter"),
                    "--log", str(log),
                    "--profile", profile,
                    "--book", book,
                    "--warmup", str(warmup),
                    "--measured", str(jitter_measured),
                    "--replays", str(replays),
                    "--cpu", "none" if cpu is None else str(cpu),
                ]
            )
            records.append(json.loads(output))
        summary = {
            "tick_ns": records[0]["tick_ns"],
            "timer": {
                key: statistics.median(record["timer"][key] for record in records)
                for key in ("min_ns", "p50_ns", "p99_ns", "p99_9_ns", "max_ns")
            },
            "run_to_run_mean": {
                key: statistics.median(record["run_to_run_mean"][key] for record in records)
                for key in ("mean_ns", "stddev_ns", "min_ns", "max_ns")
            },
            "per_command": {
                key: statistics.median(record["per_command"][key] for record in records)
                for key in ("stddev_p50_ns", "stddev_p90_ns", "stddev_p99_ns", "range_p50_ns", "range_p99_ns")
            },
        }
        print(f"  timer p50 ns {summary['timer']['p50_ns']:.2f}, run-to-run stddev ns {summary['run_to_run_mean']['stddev_ns']:.3f}, "
              f"per-command stddev p50 ns {summary['per_command']['stddev_p50_ns']:.2f}")
        entries.append({"book": book, "profile": profile, "log": log.name, "summary": summary, "runs": records})
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("book", "ring", "pipeline", "jitter"), default="book")
    parser.add_argument("--books", default=None, help="defaults to all books for the book suite, otherwise the fast book")
    parser.add_argument("--profiles", default=",".join(MEASURED_COMMANDS))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cpu", default="2", help="CPU to pin to, or 'none'")
    parser.add_argument("--peer-cpu", default="4", help="the other thread's CPU in the ring and pipeline suites")
    parser.add_argument("--label", default=None)
    parser.add_argument("--build-dir", default="build/release")
    parser.add_argument("--data-dir", default="data")
    parser.add_argument("--out-dir", default="results")
    parser.add_argument("--smoke", action="store_true", help="short run to check the pipeline; never citable")
    args = parser.parse_args()

    if args.books is None:
        args.books = "baseline,baseline_pool,fast" if args.suite == "book" else "fast"
    build_dir = ROOT / args.build_dir
    data_dir = ROOT / args.data_dir
    out_dir = ROOT / args.out_dir
    cpu = None if args.cpu == "none" else int(args.cpu)
    peer_cpu = None if args.peer_cpu == "none" else int(args.peer_cpu)

    subprocess.run(["cmake", "--build", str(build_dir)], check=True, cwd=ROOT, capture_output=True)

    git = describe_git()
    build = describe_build(build_dir)
    machine = describe_machine(cpu)
    reasons = not_citable_reasons(git, build, machine, args.runs, args.smoke)
    # The label is read from the machine, so a file's name can't disagree with how it was measured.
    turbo = {"0": "on", "1": "off"}.get(machine["no_turbo"], "unknown")
    label = f"{args.label or args.suite}_turbo_{turbo}"

    if args.suite == "book":
        entries = run_book_suite(args, build_dir, data_dir, cpu)
    elif args.suite == "ring":
        entries = run_ring_suite(args, build_dir, cpu, peer_cpu)
    elif args.suite == "pipeline":
        entries = run_pipeline_suite(args, build_dir, data_dir, cpu, peer_cpu)
    else:
        entries = run_jitter_suite(args, build_dir, data_dir, cpu)

    now = datetime.datetime.now(datetime.timezone.utc)
    result = {
        "schema": 2,
        "suite": args.suite,
        "label": label,
        "timestamp_utc": now.isoformat(timespec="seconds"),
        "citable": not reasons,
        "not_citable_because": reasons,
        "git": git,
        "build": build,
        "machine": machine,
        "entries": entries,
    }
    out_dir.mkdir(exist_ok=True)
    name = f"{now.strftime('%Y%m%dT%H%M%S')}_{git['sha'][:8]}_{label}.json"
    path = out_dir / name
    path.write_text(json.dumps(result, indent=1) + "\n")
    print(f"wrote {path.relative_to(ROOT)} citable={result['citable']} {reasons or ''}")


if __name__ == "__main__":
    main()
