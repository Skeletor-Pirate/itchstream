#!/usr/bin/env bash
# Cut a message-aligned slice out of NASDAQ's gzipped sample file.
#
# The full-day 5.0 files are 4-18 GB compressed and 15-40 GB uncompressed. This
# streams the download through zcat and stops reading once the slice is big
# enough, so cutting a 300 MB slice transfers roughly 100 MB instead of 4 GB.
# Re-run it when you want the full day for the final numbers.
#
#   ./scripts/make_slice.sh 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/07302019.NASDAQ_ITCH50.gz' \
#       data/slice_20190730.itch 300000000
#
# NASDAQ's files are not redistributed by this repository. Check the current
# terms on the download page before you publish anything derived from them.

set -uo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <gzipped ITCH url> <output .itch> [bytes, default 300000000]" >&2
    exit 64
fi

url="$1"
output="$2"
budget="${3:-300000000}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v zcat >/dev/null 2>&1; then
    echo "error: zcat not found (install gzip)" >&2
    exit 69
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "error: curl not found" >&2
    exit 69
fi

echo "url      $url"
echo "output   $output"
echo "budget   $budget bytes (message-aligned)"

set -o pipefail
curl -sSL --fail "$url" \
    | zcat \
    | python3 "$root/tools/make_slice.py" "$budget" \
    > "$output"

status=$?
# 141 is SIGPIPE: expected and harmless, because the slicer stops reading as
# soon as it has enough bytes and the download is aborted behind it.
if [[ $status -ne 0 && $status -ne 141 ]]; then
    echo "error: pipeline failed with status $status" >&2
    exit $status
fi

if [[ ! -s "$output" ]]; then
    echo "error: $output is empty" >&2
    exit 65
fi

ls -l "$output"
echo
echo "next: ./build/itchstream stats '$output' --repeat 10"
echo "note: this slice is truncated relative to the trading day, and its last"
echo "      message is complete; the day's closing messages are not in it."
