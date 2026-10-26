# ItchStream

[![CI](https://github.com/Skeletor-Pirate/itchstream/actions/workflows/ci.yml/badge.svg)](https://github.com/Skeletor-Pirate/itchstream/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

A C++20 reader for NASDAQ TotalView-ITCH 5.0 files. It memory-maps a day of
messages, walks the 2-byte length framing, decodes the ten message types that
carry order-book information, and reports what it found. It can write one
symbol's messages back out as a valid ITCH file or as text, and re-emit a stream
at the feed's own pace, byte for byte. Single pass, single thread, no allocation
per message.

This is a parser and a measurement harness, not a trading system. There is no
live feed, no strategy, no GUI, no full-market book, and no Windows-specific
tuning. Real ITCH data never enters git; see [`data/README.md`](data/README.md).

Contents: [Status](#status) · [Build and run](#build-and-run) ·
[Use as a library](#use-as-a-library) · [Dump one symbol](#dump-one-symbol) ·
[Replay a stream](#replay-a-stream) · [Measured throughput](#measured-throughput) ·
[What M0 found in the real data](#what-m0-found-in-the-real-data) ·
[Where the requirements are proven](#where-the-requirements-are-proven) ·
[Design decisions](#design-decisions) · [Layout](#layout) · [License](#license)

Architecture notes live in [`docs/architecture.md`](docs/architecture.md), the
history in [`CHANGELOG.md`](CHANGELOG.md), and the build/test/fuzz/bench rules in
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## Status

| Milestone | Scope | State |
| --- | --- | --- |
| **M0** | Repo scaffold, mmap feed walker, decoder with `static_assert`ed wire sizes, `stats` mode, synthetic seed file | **done** |
| **M1** | Requirement pass (FR-1..FR-4), first parse benchmark with machine details published | **done** |
| **M2** | `dump` — one symbol's complete stream as a valid ITCH file, or as text | **done** |
| **M3** | `replay` — re-emit a stream at the feed's own pace, with `--speed`, `--limit`, `--symbol` | **done** |

Decoded types: `S` System Event, `R` Stock Directory, `A` Add Order, `F` Add
Order with MPID attribution, `E` Order Executed, `C` Order Executed with Price,
`X` Order Cancel, `D` Order Delete, `U` Order Replace, `P` Trade.

Everything else (`H`, `I`, `L`, `V`, `Y`, and anything NASDAQ adds later) is
skipped by its own length prefix and counted by type, so a file with message
types this version has never seen still parses end to end.

## Build and run

Requires CMake 3.20+ and a C++20 compiler. The test suite downloads Catch2
v3.5.2 on first configure.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DITCHSTREAM_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

No real data is needed to start — a 7 KB synthetic seed is committed, and it
contains all ten decoded types:

```bash
./build/itchstream stats data/synthetic_seed.itch
```

For a larger structural file, generate one:

```bash
./tools/gen_synthetic.py -o data/synthetic.itch --messages 1000000
./build/itchstream stats data/synthetic.itch
```

With a real day, or a slice of one (see [`data/README.md`](data/README.md)):

```bash
./scripts/make_slice.sh \
    'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/07302019.NASDAQ_ITCH50.gz' \
    data/slice_20190730.itch 300000000

./build/itchstream stats data/slice_20190730.itch
./build/itchstream stats data/slice_20190730.itch --json --repeat 10
```

`stats` options: `--repeat N` (N measured runs after one discarded warm-up),
`--length-only` (walk and count, skip payload decoding), `--json`, `--quiet`,
`--no-machine`.

Exit codes are part of the contract: `0` ok, `1` cannot open a file, `2` malformed
data, `3` bad usage, `4` not implemented yet, `5` message count differed between
runs of the same file, `6` symbol not found in the file, `7` cannot write the
output.

## Use as a library

The CLI is a thin shell around `itchstream_core`, a static library with public
headers. Another program can walk a file without going through `main`:

```cpp
#include <cstdio>
#include <itchstream/decoder.hpp>
#include <itchstream/feed.hpp>

int main(int argc, char** argv) {
    if (argc != 2) return 3;

    itchstream::MappedFile file;
    std::string error;
    if (!file.open(argv[1], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    const itchstream::WalkResult result = itchstream::walk(
        file.data(), file.size(), [](const itchstream::RawMessage& message) {
            const auto payload =
                itchstream::decode_payload(message.type, message.payload, message.length);
            if (payload) {
                if (const auto* trade = std::get_if<itchstream::Trade>(&*payload)) {
                    std::printf("trade %u @ %u\n", trade->shares, trade->price);
                }
            }
            return itchstream::WalkAction::Continue;
        });

    return result.ok() ? 0 : 2;  // describe(result) has the offset and the reason
}
```

Link `itchstream_core` and use `include/` from the target's public include
directory (`target_link_libraries(app PRIVATE itchstream_core)` is enough with
`add_subdirectory` or `FetchContent`). `RawMessage` is a view into the mapping:
it is valid only for the duration of the callback.

## Dump one symbol

```bash
# The symbol's complete message stream, byte for byte, as a valid ITCH file.
./build/itchstream dump data/slice_20190730.itch --symbol AAPL --out data/aapl.itch

# The same messages as text, one line each.
./build/itchstream dump data/slice_20190730.itch --symbol AAPL \
    --out data/aapl.txt --format text
```

`--format itch` (the default) rebuilds each 2-byte length prefix from the
validated length and copies the message body verbatim. The output re-parses with
zero length mismatches, and it contains the symbol's messages for types this
version does not decode (`I`, `H`, `L`, `Y`, ...) as well, because the locate is
at the same offset in every type. That is what makes a dump replayable and what
makes `stats` on the dump a real check on the dump rather than a restatement.

Filtering is by stock locate, resolved from the `R` Stock Directory message,
because six of the ten decoded types do not carry the symbol. `R`, `A`, `F` and
`P` are also matched by their embedded symbol, so a slice that begins mid-day
still catches an order whose directory entry has already passed. The remaining
edge is documented rather than hidden: a locate-only message (`E`, `C`, `X`,
`D`, `U`) that arrives before its directory entry cannot be attributed, and
`test_dump.cpp` pins that behaviour.

Measured on the 314 MB slice: `AAPL` is locate 14 with 9,120 messages and
293,281 bytes. Re-parsing the dump reports 9,120 messages, 8,980 decoded, 140
skipped, 0 failures. A separate Python scanner that counts messages by locate
over the source slice agrees on the count, and every message in the dump carries
locate 14.

Text output looks like this (skipped types included, marked with their length):

```
03:07:20.691254729  R    14 symbol=AAPL mkt=Q lot=100
03:07:21.214946946  H    14 raw=25B
03:07:21.214947816  Y    14 raw=20B
03:07:31.679213026  L    14 raw=26B
```

`dump` also takes `--json` for a machine-readable summary.

## Replay a stream

```bash
# The whole file at 10x the exchange's pace, straight into a consumer.
./build/itchstream replay data/slice_20190730.itch --speed 10 | ./consumer

# One symbol's six-hour day compressed into six seconds, to a file.
./build/itchstream replay data/slice_20190730.itch --symbol AAPL \
    --speed 3600 --out data/aapl_3600x.itch

# The first 10,000 messages, written out as fast as the walk can write them.
./build/itchstream replay data/slice_20190730.itch --limit 10000 --speed 100000 \
    > /dev/null
```

`replay` writes raw ITCH bytes: the same rebuilt length prefix and verbatim body
that `dump` produces, through the same `write_framed` call. So the stream
re-parses, and its bytes are the input's bytes. `--speed F` (default `1`) sets
the pace against the exchange timestamps, `--limit N` stops after N messages,
and `--symbol SYM` applies the same filter as `dump` — `replay --symbol` and
`dump` are two names for identical output.

Pacing is absolute, not incremental. Each message's deadline is

```
wall_start + (timestamp - first_timestamp) / speed
```

so a late wake-up costs that one message and nothing after it, instead of the
permanent drift that `sleep(gap since the last message)` accumulates.
`steady_clock` drives the pace; the exchange timestamp is data, not a clock.
Messages too short to carry a timestamp inherit the previous pace, and a
timestamp that goes backwards is clamped rather than wrapping into a
centuries-long deadline.

When the stream goes to stdout, the report goes to **stderr**, so
`itchstream replay x.itch | consumer` is a clean stream. With `--out FILE` the
report goes to stdout as it does for `stats` and `dump`, which means
`replay x.itch --symbol AAPL --speed 10 --out aapl.itch --json > aapl.json`
produces the stream and its summary in one pass. Bytes are flushed when a
message actually waited: a paced stream reaches its consumer on time, while a
fast replay is not syscall-bound.

On Windows, redirect with `cmd` or use `--out`: PowerShell's `>` re-encodes
native stdout, which corrupts any binary stream regardless of which program
wrote it. `--out` naming the input file is refused with exit `3` — the input is
mapped for the whole run, and truncating it under the walker is not a mistake
worth surviving.

Measured on the 314 MB slice:

* `replay --speed 1000000 --out` of the whole file is byte-identical to the
  source: `cmp` clean over 314,572,770 bytes, 9,907,683 messages, 3.4 s wall
  (the walk and the write, not the clock).
* `replay --symbol AAPL` and `dump --symbol AAPL` both produce 293,281-byte
  files, and `cmp` says they are the same file.
* The first 200 messages of the slice span 270.0575 s of exchange time. At 1x
  the run took 270.0576 s: 0.13 ms late after four and a half minutes.
* The AAPL stream spans 22,923 s of exchange time. At 100,000x it aims at
  229.234 ms of wall time and lands 0.01 ms to 0.07 ms late over 9,120 messages,
  so the error does not grow with the message count.

Accuracy is bounded by the platform's sleep granularity — tens of microseconds
under WSL2, not the nanosecond resolution of the timestamps. These are
`steady_clock` measurements on the machine published under
[Measured throughput](#measured-throughput).

## Measured throughput

Input: a 314,572,770-byte message-aligned slice of
`07302019.NASDAQ_ITCH50` — 9,907,683 messages, real NASDAQ data.

| | median | rate |
| --- | --- | --- |
| decode all ten types | 152.278 ms | 65.06 M msg/s, 2065.8 MB/s |
| walk lengths only (`--length-only`) | 77.873 ms | 127.23 M msg/s, 4039.6 MB/s |

Across three separate 10-run sessions the decode median landed between 147 and
160 ms (62-67 M msg/s) and length-only between 74 and 78 ms (127-134 M msg/s).
That band is the result; the extra digits are just the last run.

Machine and build, printed by the binary alongside the numbers:

```
Intel(R) Core(TM) Ultra 7 155H | 22 logical cores | 7.5 GiB | Linux 6.6.114.1-microsoft-standard-WSL2
g++ 13.3.0 | Release | optimised | -O3 -DNDEBUG -march=native
```

Method ([`scripts/bench.sh`](scripts/bench.sh), so the number cannot drift from
how it was produced):

* Release build, sanitizers off, `-march=native`, `taskset -c 2` — the CPU is a
  hybrid P/E part and an unpinned process wanders between core types.
* One warm-up run discarded by the tool, then 10 measured runs; median reported
  with min, max, and spread.
* The file is copied onto the Linux filesystem first; measuring across the
  Windows 9p mount measures the mount.
* Reported spread inside a single session: 10.7% decode, 21.1% length-only. That
  is a laptop under WSL2, not a benchmark rig — compare the two rows, not the
  third digit of either.
* Length-only runs about twice as fast as decode, so the walk is bounded by
  message dispatch and memory traffic rather than by field extraction.
* Sanitizer builds are not benchmarked. They are the CI correctness gate and
  they are several times slower by construction.
* Host power state is part of the machine. The numbers above were measured on
  AC at boost clocks; the same binary on battery at 1.4 GHz measured 36 M msg/s
  decode and 75 M msg/s length-only. Publish the power state next to the number
  or the number is not reproducible.

Numbers from synthetic files are labelled synthetic wherever they appear. The
synthetic generator is structurally faithful but its message mix is not real.

## What M0 found in the real data

Per-type counts over the 300 MB slice, which is how the spec table was checked
against reality rather than against itself:

| type | message | count | bytes |
| --- | --- | ---: | ---: |
| S | System Event | 2 | 24 |
| R | Stock Directory | 8,849 | 345,111 |
| A | Add Order | 3,829,844 | 137,874,384 |
| F | Add Order (MPID) | 331,943 | 13,277,720 |
| E | Order Executed | 38,530 | 1,194,430 |
| C | Order Executed with Price | 11 | 396 |
| X | Order Cancel | 634,900 | 14,602,700 |
| D | Order Delete | 3,684,511 | 70,005,709 |
| U | Order Replace | 393,234 | 13,763,190 |
| P | Trade | 9,399 | 413,556 |
| H | *skipped* Stock Trading Action | 8,851 | 221,275 |
| I | *skipped* NOII | 748,090 | 37,404,500 |
| L | *skipped* Market Participant Position | 210,669 | 5,477,394 |
| V | *skipped* MWCB Decline Level | 1 | 35 |
| Y | *skipped* Reg SHO Restriction | 8,849 | 176,980 |

9,907,683 messages, 8,931,223 decoded, 976,460 skipped, **0 decode failures and
0 length mismatches**. Every observed length agreed with the table compiled into
the decoder. Timestamps run 03:02:50.642174571 to 09:29:24.089391358
(nanoseconds since midnight, exchange time).

Two things this table settled: the wire-size table is right, and skipping
unknown types by length is not a corner case — 10% of a real day is message
types this build does not decode, including all of `I` (NOII, the auction
imbalance feed) and `L`.

## Where the requirements are proven

| | Test / artefact |
| --- | --- |
| FR-1 map a file and walk every message in order | `test_feed.cpp` `[FR-1]`, `MappedFile` in `feed.cpp` |
| FR-2 decode the ten types, wire sizes match the spec | per-type field tests in `test_decode.cpp`, `static_assert` per struct |
| FR-3 timestamp at the same offset for every type | one test over all ten decoded types |
| FR-4 per-type counts, message totals, skipped messages | `test_feed.cpp` `[FR-4]`, `stats.cpp` |
| FR-5 one symbol's complete stream, byte-exact and replayable | `test_dump.cpp` and `test_replay.cpp`, `dump.cpp`, `symbol_filter.cpp` |
| FR-10 malformed input fails cleanly, never UB | malformed matrix plus a loop over *every* prefix of a valid stream; `replay` stops at the break instead of writing past it |
| FR-11 same file in, identical counts out | unit test, `--repeat` agreement check in `bench.sh`, CI determinism job; replay emits the same bytes at 1x and 1000x |
| M3 paced re-emission | `test_replay.cpp`: byte identity, `--limit`, `--symbol`, 1x vs 50x timing, backwards timestamps, sink and truncation failures |

`ctest` runs 54 test cases / 2,021 assertions plus a seeded fuzz driver (20,000
mutations from a logged seed). Both are clean under ASan + UBSan in Debug.

The libFuzzer target instruments the library itself, not just the harness, so
coverage actually follows the walker, the decoders, the dump filter and the
byte-exact writer. A 60-second local run (Clang 18, ASan + UBSan, seeded with
`data/synthetic_seed.itch`) executed 2.18 million inputs over 360 edges, added
1,585 corpus entries, peaked at 530 MB of RSS, and found no crash. Replay's
pacing is not a fuzz target: random timestamps would fuzz the clock, and its
filter and write path are the ones `dump` already covers.

## Design decisions

**No casting mapped bytes to packed structs.** The mapped region is not
guaranteed aligned, and reinterpreting it as a struct is undefined behaviour
that happens to work until it does not. Every field is read with
`memcpy`-based big-endian loads (`endian.hpp`). Packed structs exist, but only
as a *decoded output* type — `sizeof(T) == kWireSize + 2` because the 6-byte
wire timestamp is held in a `uint64_t`. That is asserted, not assumed, and
documented at each struct.

**Prices stay integers.** ITCH prices are 4-implied-decimal integers. They are
never converted to `double`, so a price round-trips exactly and comparison is
integer comparison.

**Length is validated before dispatch.** A known type whose length disagrees
with the spec is a hard `LengthMismatch` error. An unknown type is skipped using
its own prefix, but the prefix must be between 1 and 64 bytes
(`kMaxPlausibleMessageLength`); a garbage length cannot walk the reader off the
end of the map. Truncation, a zero length, and a stray final byte are all
reported as errors with byte offsets, never as an out-of-bounds read.

**Zero per-message allocation.** Decoding fills a stack `Payload` variant. The
walk is a template over a per-message callback, so there is no virtual dispatch
and no queue between the map and the counter.

**A dump never re-encodes.** Binary output copies the validated message bodies
and rebuilds only the length prefix, so timestamps, prices and unknown message
types survive exactly; text output formats into a fixed stack buffer, so the
dump path allocates nothing per message either. The filter compares the locate
at its wire offset instead of decoding, which is what lets skipped types travel
with the symbol. `replay` writes through the same `write_framed`, so "dump" and
"replay" cannot disagree about what a message's bytes are.

**Pacing is absolute.** A replayer that sleeps for the gap since the last
message accumulates every wake-up it was late by; after ten million messages
that is a stream running minutes behind its own timestamps. Computing each
deadline from the first timestamp makes lateness local to one message. The pace
comes from `steady_clock`, never from the exchange timestamp, and a non-monotonic
timestamp is clamped instead of being allowed to wrap an unsigned subtraction
into a deadline decades away.

**The stream owns stdout.** When `replay` writes the stream to stdout, the
report moves to stderr. A summary line interleaved with a binary stream is a
corrupted stream, and `replay | consumer` is the main way this command is meant
to be used. Flushing only after a message that actually waited keeps a paced
stream on time without turning a fast replay into one syscall per message.

**The fuzz seed is logged.** The deterministic driver writes `(seed, iteration)`
to a trace file before each mutation and is always built, so a crash replays
from two numbers. A libFuzzer target exists as well; CI runs both. This closes
the "log the seed" gap that the previous project left open.

## Layout

```
include/itchstream/   messages.hpp (wire sizes, structs), endian.hpp,
                      decoder.hpp, feed.hpp (MappedFile, walk), stats.hpp,
                      dump.hpp, replay.hpp, symbol_filter.hpp, sink.hpp
src/                  decoder, feed, stats, dump, replay, symbol filter,
                      machine/build description, CLI
tests/                decode + feed/framing + dump + replay tests, seeded fuzz
                      driver, libFuzzer target
tools/                gen_synthetic.py, make_slice.py
scripts/              bench.sh, make_slice.sh, machine_info.sh
docs/                 architecture.md
data/                 README and the 7 KB synthetic seed; real data is never committed
```

`CHANGELOG.md` records what each milestone added and how it was verified;
`CONTRIBUTING.md` has the build, test, fuzz and benchmark commands.

## License

MIT. See [LICENSE](LICENSE).
