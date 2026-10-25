#!/usr/bin/env bash
# Print everything that must appear next to a published benchmark number.
#
#   ./scripts/machine_info.sh
#
# Paste the output into the README table whenever the numbers change.

set -uo pipefail

line() {
    printf '%-22s %s\n' "$1" "$2"
}

cpu="unknown"
if [[ -r /proc/cpuinfo ]]; then
    cpu="$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)"
fi

cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo unknown)"

ram="unknown"
if [[ -r /proc/meminfo ]]; then
    kb="$(awk '/^MemTotal:/ {print $2; exit}' /proc/meminfo)"
    if [[ -n "${kb:-}" ]]; then
        ram="$(awk -v kb="$kb" 'BEGIN {printf "%.1f GiB", kb / 1048576}')"
    fi
fi

kernel="$(uname -sr 2>/dev/null || echo unknown)"

line "cpu" "$cpu"
line "logical cores" "$cores"
line "ram" "$ram"
line "kernel" "$kernel"

if [[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
    line "cpu governor" "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
fi

line "date" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

if command -v g++ >/dev/null 2>&1; then
    line "g++" "$(g++ --version | head -1)"
fi
if command -v clang++ >/dev/null 2>&1; then
    line "clang++" "$(clang++ --version | head -1)"
fi
if command -v cmake >/dev/null 2>&1; then
    line "cmake" "$(cmake --version | head -1)"
fi

# The exact flags this build used are printed by the binary itself:
echo
echo "run: ./build/itchstream stats <file> --repeat 10   # prints machine + build + flags"
