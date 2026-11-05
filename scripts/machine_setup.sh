#!/usr/bin/env bash
#
# Prepares this machine for benchmarking and restores it afterwards. Run as root.
#
#   machine_setup.sh status
#   machine_setup.sh apply --turbo on|off [--offline <cpu,cpu,...>]
#   machine_setup.sh restore
#
# "apply" records the current settings, and "restore" puts them back. Nothing persists past a reboot.
# --offline takes CPUs out of service, which is how the hyperthread sibling of a benchmarked core is
# stopped from sharing that core's resources.

set -euo pipefail

STATE_FILE=/run/nanomatch-bench.state
CPU_ROOT=/sys/devices/system/cpu
NO_TURBO="$CPU_ROOT/intel_pstate/no_turbo"

governor_of_cpu0() { cat "$CPU_ROOT/cpu0/cpufreq/scaling_governor"; }

bring_all_cpus_online() {
    local cpu_dir
    for cpu_dir in "$CPU_ROOT"/cpu[0-9]*; do
        [[ -e "$cpu_dir/online" ]] && echo 1 >"$cpu_dir/online"
    done
}

set_governor() {
    local governor_file
    for governor_file in "$CPU_ROOT"/cpu[0-9]*/cpufreq/scaling_governor; do
        echo "$1" >"$governor_file"
    done
}

status() {
    echo "governor:           $(governor_of_cpu0)"
    echo "no_turbo:           $(cat "$NO_TURBO" 2>/dev/null || echo unknown)"
    echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
    echo "nmi_watchdog:       $(cat /proc/sys/kernel/nmi_watchdog)"
    echo "online CPUs:        $(cat "$CPU_ROOT/online")"
    local supply
    for supply in /sys/class/power_supply/*/online; do
        [[ -e "$supply" ]] && echo "AC power:           $(cat "$supply")"
    done
}

apply() {
    local turbo="" offline=""
    while (($#)); do
        case "$1" in
            --turbo) turbo="$2"; shift 2 ;;
            --offline) offline="$2"; shift 2 ;;
            *) echo "unknown option: $1" >&2; exit 2 ;;
        esac
    done
    if [[ "$turbo" != "on" && "$turbo" != "off" ]]; then
        echo "--turbo on|off is required" >&2
        exit 2
    fi

    # A second apply keeps the first saved state, so restore always returns to the original settings.
    if [[ ! -e "$STATE_FILE" ]]; then
        {
            echo "governor=$(governor_of_cpu0)"
            echo "no_turbo=$(cat "$NO_TURBO")"
            echo "paranoid=$(cat /proc/sys/kernel/perf_event_paranoid)"
            echo "watchdog=$(cat /proc/sys/kernel/nmi_watchdog)"
        } >"$STATE_FILE"
    fi

    # An offline CPU refuses governor writes, so apply can only be repeated from a known state.
    bring_all_cpus_online
    set_governor performance
    [[ "$turbo" == "on" ]] && echo 0 >"$NO_TURBO" || echo 1 >"$NO_TURBO"

    # Counters for the calling process only, without opening up kernel profiling.
    sysctl -q -w kernel.perf_event_paranoid=1
    # The watchdog permanently occupies a hardware counter, which could otherwise force multiplexing.
    sysctl -q -w kernel.nmi_watchdog=0

    local cpu
    for cpu in ${offline//,/ }; do
        echo 0 >"$CPU_ROOT/cpu$cpu/online"
    done

    # A benchmark that believes the wrong settings is worse than one that doesn't run.
    local expected_no_turbo=0
    [[ "$turbo" == "off" ]] && expected_no_turbo=1
    if [[ "$(cat "$NO_TURBO")" != "$expected_no_turbo" ]]; then
        echo "turbo setting did not take effect" >&2
        exit 1
    fi
    status
}

restore() {
    bring_all_cpus_online
    if [[ -e "$STATE_FILE" ]]; then
        local governor no_turbo paranoid watchdog
        governor=$(sed -n 's/^governor=//p' "$STATE_FILE")
        no_turbo=$(sed -n 's/^no_turbo=//p' "$STATE_FILE")
        paranoid=$(sed -n 's/^paranoid=//p' "$STATE_FILE")
        watchdog=$(sed -n 's/^watchdog=//p' "$STATE_FILE")
        set_governor "$governor"
        echo "$no_turbo" >"$NO_TURBO"
        sysctl -q -w kernel.perf_event_paranoid="$paranoid"
        sysctl -q -w kernel.nmi_watchdog="$watchdog"
        rm -f "$STATE_FILE"
    fi
    status
}

case "${1:-}" in
    status) status ;;
    apply) shift; apply "$@" ;;
    restore) restore ;;
    *) sed -n '3,7p' "$0" >&2; exit 2 ;;
esac
