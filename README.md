# ItchStream

A C++20 reader for NASDAQ TotalView-ITCH 5.0 files. It memory-maps a day of
messages, walks the 2-byte length framing, decodes the ten message types that
carry order-book information, and reports what it found. Single pass, single
thread, no allocation per message.

This is a parser and a measurement harness, not a trading system. There is no
live feed, no strategy, no GUI, no full-market book, and no Windows-specific
tuning. Real ITCH data never enters git; see [`data/README.md`](data/README.md).

## Status

| Milestone | Scope | State |
| --- | --- | --- |
| **M0** | Repo scaffold, mmap feed walker, decoder with `static_assert`ed wire sizes, `stats` mode, synthetic seed file | **done** |
| **M1** | Requirement pass (FR-1..FR-4), first parse benchmark with machine details published | **done** |
| M2 | `dump` — replay one symbol's messages to a file | planned |
| M3 | `replay` — paced re-emission with timestamps | planned |

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

Exit codes are part of the contract: `0` ok, `1` cannot open file, `2` malformed
data, `3` bad usage, `4` not implemented yet, `5` message count differed between
runs of the same file.

## Measured throughput

Input: a 314,572,770-byte message-aligned slice of
`07302019.NASDAQ_ITCH50` — 9,907,683 messages, real NASDAQ data.

| | median | rate |
| --- | --- | --- |
| decode all ten types | 152.278 ms | 65.06 M msg/s, 2065.8 MB/s |
| walk lengths only (`--length-only`) | 77.873 ms | 127.23 M msg/s, 4039.6 MB/s |

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
* Reported spread on this run: 10.7% decode, 21.1% length-only. That is a
  laptop under WSL2, not a benchmark rig. Treat the rate as "tens of millions
  of messages per second" and the comparison between the two rows as the real
  signal: length-only is 2x decode, so the walk is bounded by message dispatch
  and memory traffic, not by field extraction.
* Sanitizer builds are not benchmarked. They are the CI correctness gate and
  they are several times slower by construction.

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
| L | *skipped* MPID Prior Day | 210,669 | 5,477,394 |
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
| FR-10 malformed input fails cleanly, never UB | malformed matrix plus a loop over *every* prefix of a valid stream |
| FR-11 same file in, identical counts out | unit test, `--repeat` agreement check in `bench.sh`, CI determinism job |

`ctest` runs 32 test cases / 1,888 assertions plus a seeded fuzz driver (20,000
mutations from a logged seed). Both are clean under ASan + UBSan in Debug.

The libFuzzer target instruments the library itself, not just the harness, so
coverage actually follows the walker and the decoders. A 60-second local run
(Clang 18, ASan + UBSan, seeded with `data/synthetic_seed.itch`) executed 21.4
million inputs, added 1,041 corpus entries, peaked at 632 MB of RSS, and found
no crash.

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

**The fuzz seed is logged.** The deterministic driver writes `(seed, iteration)`
to a trace file before each mutation and is always built, so a crash replays
from two numbers. A libFuzzer target exists as well; CI runs both. This closes
the "log the seed" gap that the previous project left open.

## Layout

```
include/itchstream/   messages.hpp (wire sizes, structs), endian.hpp,
                      decoder.hpp, feed.hpp (MappedFile, walk), stats.hpp
src/                  decoder, feed, stats, machine/build description, CLI
tests/                decode + feed/framing tests, seeded fuzz driver, libFuzzer target
tools/                gen_synthetic.py, make_slice.py
scripts/              bench.sh, make_slice.sh, machine_info.sh
data/                 README only; real data is never committed
```

## License

MIT. See [LICENSE](LICENSE).
