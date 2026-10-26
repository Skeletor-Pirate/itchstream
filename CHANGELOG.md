# Changelog

Every entry says what changed and what was verified. Throughput numbers belong
to the machine and flags published in the [README](README.md); this file does not
restate them.

## 0.3.0 — 2026-10-26 (M3)

### Added

- `replay <file> [--speed F] [--limit N] [--symbol SYM] [--out FILE]`: re-emits
  the feed's exact bytes on deadlines computed from the exchange timestamps.
  Pacing is absolute (`wall_start + (ts - first_ts) / speed`), so lateness stays
  local to one message; non-monotonic timestamps are clamped; the sink is
  flushed only after a message that actually waited.
- `SymbolFilter` and `MessageSink` extracted from the dump path, so
  `dump --symbol` and `replay --symbol` share one definition of symbol
  membership, and `write_framed` is the single definition of a re-emitted
  message. `DumpSink` became `MessageSink` and gained `flush()`.
- The report moves to stderr when the stream owns stdout, so
  `replay file | consumer` is clean.
- `--out` naming the input file is refused with exit `3`: the input is mapped
  for the whole run.
- CI: replay byte-identity, `replay`/`dump` equality for one symbol, `--limit`,
  and the input-overwrite refusal.

### Verified

- 54 test cases / 2,021 assertions, clean under ASan + UBSan; a fresh clone
  builds, tests and replays.
- The 314 MB slice replayed at 1,000,000x is byte-identical to the source;
  `replay --symbol AAPL` equals `dump --symbol AAPL` (293,281 bytes).
- Pacing: 270.0575 s of feed at 1x took 270.0576 s. The AAPL day (22,923 s) at
  100,000x landed 0.01–0.07 ms late over 9,120 messages.

## 0.2.0 — 2026-10-26 (M2)

### Added

- `dump <file> --symbol SYM --out FILE [--format itch|text]`: a symbol's complete
  message stream, binary by default. The length prefix is rebuilt and the body
  copied verbatim, so skipped types travel with the symbol and the output
  re-parses.
- Filtering by stock locate (learned from `R`), with `R`, `A`, `F` and `P` also
  matched by their embedded symbol for slices that begin mid-day.
- `--json` summaries; exit `6` for a symbol that is absent (output left empty),
  exit `7` for a write failure; stats-only options rejected on `dump` and vice
  versa; a libFuzzer path over both output formats.
- CI: dump SPY from a 200k synthetic file and re-parse it.

### Verified

- AAPL on the real slice: locate 14, 9,120 messages, 293,281 bytes; re-parsing
  reports the same 9,120 messages, 8,980 decoded, 140 skipped, 0 failures, and an
  independent Python locate scan agrees on every message.
- 43 test cases / 1,947 assertions under ASan + UBSan.

## 0.1.0 — 2026-10-26 (M0 + M1)

### Added

- Memory-mapped feed walker with length validation before dispatch:
  truncation, zero lengths, oversized prefixes and spec/length disagreements are
  errors with offsets, never out-of-bounds reads.
- Decoders for `S`, `R`, `A`, `F`, `E`, `C`, `X`, `D`, `U`, `P`, each with a
  wire-size `static_assert` and per-field tests; every other type skipped and
  counted by length.
- `stats` mode with `--repeat`, `--length-only`, `--json`, `--quiet`,
  `--no-machine`, and a machine/build block printed next to every number.
- Synthetic generator, boundary-aware slicer, benchmark script, seeded fuzz
  driver and libFuzzer target, CI matrix (sanitizers, release + determinism,
  fuzz, Windows), and the 7 KB synthetic seed.

### Verified

- 314,572,770 bytes / 9,907,683 real messages: 8,931,223 decoded, 976,460
  skipped, 0 decode failures, 0 length mismatches.
- Decode median 152.278 ms (65.06 M msg/s, 2065.8 MB/s), length-only 77.873 ms
  (127.23 M msg/s, 4039.6 MB/s), pinned core, Release, AC power; the same binary
  on battery measured 36 M msg/s, which is why power state is published.
