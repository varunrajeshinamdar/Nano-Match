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
    if "perf_per_command" in summary:
        perf = summary["perf_per_command"]
        print("  per command:      " + "  ".join(f"{key}={value:.2f}" for key, value in perf.items()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--books", default="baseline")
    parser.add_argument("--profiles", default=",".join(MEASURED_COMMANDS))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cpu", default="2", help="CPU to pin to, or 'none'")
    parser.add_argument("--label", default="bench")
    parser.add_argument("--build-dir", default="build/release")
    parser.add_argument("--data-dir", default="data")
    parser.add_argument("--out-dir", default="results")
    parser.add_argument("--smoke", action="store_true", help="short run to check the pipeline; never citable")
    args = parser.parse_args()

    build_dir = ROOT / args.build_dir
    data_dir = ROOT / args.data_dir
    out_dir = ROOT / args.out_dir
    cpu = None if args.cpu == "none" else int(args.cpu)

    subprocess.run(["cmake", "--build", str(build_dir)], check=True, cwd=ROOT, capture_output=True)

    git = describe_git()
    build = describe_build(build_dir)
    machine = describe_machine(cpu)
    reasons = not_citable_reasons(git, build, machine, args.runs, args.smoke)

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

    now = datetime.datetime.now(datetime.timezone.utc)
    result = {
        "schema": 1,
        "label": args.label,
        "timestamp_utc": now.isoformat(timespec="seconds"),
        "citable": not reasons,
        "not_citable_because": reasons,
        "git": git,
        "build": build,
        "machine": machine,
        "entries": entries,
    }
    out_dir.mkdir(exist_ok=True)
    name = f"{now.strftime('%Y%m%dT%H%M%S')}_{git['sha'][:8]}_{args.label}.json"
    path = out_dir / name
    path.write_text(json.dumps(result, indent=1) + "\n")
    print(f"wrote {path.relative_to(ROOT)} citable={result['citable']} {reasons or ''}")


if __name__ == "__main__":
    main()
