#!/usr/bin/env bash
# Local benchmark, run the way the README says it was run.
#
#   ./scripts/bench.sh [file] [repeats] [core] [build-dir]
#
# Defaults: the 300 MB slice, 10 measured runs, core 2, ./build-rel.
#
# Method, fixed here so a number cannot drift from how it was produced:
#   * Release build, -O3, -march=native, sanitizers off
#   * process pinned to one core with taskset
#   * one warm-up run discarded by the tool itself, then N measured runs
#   * median reported, with min/max and the spread
#   * file copied onto the Linux filesystem, not measured across the 9p mount
#   * machine and exact compiler flags printed by the binary alongside the result

set -uo pipefail

file="${1:-/root/itch-data/slice_clean.itch}"
repeats="${2:-10}"
core="${3:-2}"
build="${4:-build-rel}"
binary="$build/itchstream"
results_dir="bench"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [[ ! -x "$binary" ]]; then
    echo "error: $binary not found. Configure a benchmark build first:" >&2
    echo "  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DITCHSTREAM_NATIVE=ON -DITCHSTREAM_TESTS=OFF" >&2
    echo "  cmake --build build-rel -j" >&2
    exit 69
fi

if [[ ! -f "$file" ]]; then
    echo "error: input '$file' not found. See data/README.md." >&2
    exit 66
fi

mkdir -p "$results_dir"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
out="$results_dir/$stamp.txt"

# Pin to one core when taskset exists: otherwise the scheduler moves the process
# between P-cores and E-cores on this class of CPU and the spread becomes noise.
pin=()
if command -v taskset >/dev/null 2>&1; then
    pin=(taskset -c "$core")
else
    echo "warning: taskset not found; numbers are unpinned" >&2
fi

run() {
    local label="$1"
    shift
    echo "### $label" | tee -a "$out"
    "${pin[@]}" "$binary" stats "$file" --repeat "$repeats" "$@" 2>&1 | tee -a "$out"
    echo | tee -a "$out"
}

{
    echo "itchstream benchmark  $stamp"
    echo "host  $(uname -sr)"
    echo "file  $file ($(stat -c %s "$file") bytes)"
    echo "core  $core, repeats $repeats (plus one discarded warm-up)"
    echo
} | tee -a "$out"

run "decode payloads (parse throughput)"
run "length-only walk (no payload decode)" --length-only

# FR-11: the same bytes twice must give the same counts, not just the same rate.
echo "### determinism (FR-11)" | tee -a "$out"
"${pin[@]}" "$binary" stats "$file" --json --repeat 1 > "$results_dir/$stamp.first.json" 2>/dev/null
"${pin[@]}" "$binary" stats "$file" --json --repeat 1 > "$results_dir/$stamp.second.json" 2>/dev/null
if python3 - "$results_dir/$stamp.first.json" "$results_dir/$stamp.second.json" <<'PY'
import json
import sys

first = json.load(open(sys.argv[1]))
second = json.load(open(sys.argv[2]))
keys = ("bytes", "messages", "decoded", "decode_failures", "skipped",
        "first_timestamp_ns", "last_timestamp_ns", "walk_ok", "by_type")
bad = [k for k in keys if first.get(k) != second.get(k)]
if bad:
    print("MISMATCH in", ", ".join(bad))
    raise SystemExit(1)
print(f"identical: {first['messages']} messages, {len(first['by_type'])} types")
PY
then
    echo "determinism ok" | tee -a "$out"
else
    echo "determinism FAILED" | tee -a "$out"
fi

echo | tee -a "$out"
echo "written to $out"
