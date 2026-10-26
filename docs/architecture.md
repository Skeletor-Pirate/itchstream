# Architecture

ItchStream is a single-pass, single-threaded reader for NASDAQ TotalView-ITCH
5.0 files, plus two ways to write messages back out. It is a parser and a
measurement harness, not a trading system: no live feed, no strategy, no
order book, no GUI.

## The pipeline

```
              mmap (read-only)
 file ─────────────────────────► walk() ──► RawMessage{type, length, payload}
                                             │
                               ┌─────────────┴──────────────┐
                               │                            │
                      is_decoded(type)?              SymbolFilter
                               │                            │
                        decode_payload()               matches()?
                               │                            │
                          Payload (variant)          the message's bytes
                               │                            │
                        Stats counters            MessageSink (dump, replay)
```

One pass over the mapping. Nothing is queued; nothing is copied except the bytes
a sink is asked to write.

## Modules

| Module | Owns | Invariant |
| --- | --- | --- |
| `feed.hpp` / `feed.cpp` | `MappedFile`, the length-prefixed walk | a message reaches a visitor only after its length is checked against the file and the specification |
| `messages.hpp` | wire sizes, offsets, packed decoded structs | every decoded struct is asserted to be `kWireSize + 2` bytes |
| `endian.hpp` | big-endian loads | fields are read with `memcpy`, never by casting mapped bytes |
| `decoder.hpp` / `decoder.cpp` | `Payload` variant and the ten decoders | prices stay 4-implied-decimal integers |
| `stats.hpp` / `stats.cpp` | per-type tallies and report formatting | counts are derived from bytes only |
| `symbol_filter.hpp` / `symbol_filter.cpp` | one symbol against a stream | the locate at offset 1 is the only key that covers every type |
| `sink.hpp` | `MessageSink`, `write_framed` | re-emission is the input's bytes with the prefix rebuilt |
| `dump.hpp` / `dump.cpp` | a symbol stream as ITCH bytes or text | binary output is a valid ITCH stream |
| `replay.hpp` / `replay.cpp` | paced re-emission | speed changes timing, never a byte |
| `machine.cpp` | host and build description | a published number carries its machine and flags |
| `src/main.cpp` | CLI, exit codes, file handles | the stream owns stdout; the report moves to stderr |

## Boundaries worth knowing

**`RawMessage` is a view, not a copy.** It points into the mapping and is valid
only for the duration of the visitor call. A visitor that keeps bytes must copy
them, which is exactly what `dump` and `replay` do.

**The walker validates before dispatch.** A known type whose length disagrees
with the specification is a hard error; an unknown type is skipped by its own
prefix, which must be 1..64 bytes. Nothing below the walker has to worry about
walking off the end of the mapping.

**`Payload` is a `std::variant` of decoded structs**, filled on the stack. There
is no allocation per message on any path.

**One filter, two commands.** `dump --symbol` and `replay --symbol` share
`SymbolFilter`, so they cannot disagree about what belongs to a symbol.

**One writer, two commands.** `write_framed` rebuilds the length prefix and
copies the body verbatim; there is exactly one definition of "this message's
bytes", and both output commands go through it.

**Pacing is not decoding.** `replay` paces on `steady_clock` deadlines derived
from the exchange timestamps. The timestamp is data; it is never used as a
clock, and a non-monotonic one is clamped instead of wrapped.

## Adding a message type

1. Add the struct to `messages.hpp` with its `kWireSize` and the
   `static_assert(sizeof(T) == T::kWireSize + 2)`.
2. Add the `decode_*` function, the `Payload` alternative, and the dispatch case.
3. Add a field test per field in `test_decode.cpp`, plus the wire-size check.
4. Nothing else: if the type is not decoded it is still skipped by its length,
   and if it carries a locate it is still filterable and replayable.

If the type carries a symbol of its own, `symbol_filter.cpp` can compare it
before the locate is known; otherwise the locate path already covers it.

## Adding a command

`src/main.cpp` has one `run_<command>(const Options&)` per command. Options are
validated before dispatch, the file is opened and mapped once, the library does
the work, the CLI owns the output handle and translates the result into an exit
code. The library never touches `stdout`, `stderr`, or a path.
