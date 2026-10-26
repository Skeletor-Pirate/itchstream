#pragma once

// M2: isolate one symbol's message stream.
//
// Filtering needs a stock locate, not a symbol compare, because six of the ten
// decoded types do not carry the symbol. The locate sits at byte offset 1 in
// every ITCH 5.0 message, so once the directory entry has been seen the dump
// can copy every message for that locate — including the types this version
// does not decode. A dump is the symbol's complete stream, not the decoded
// subset, which is what makes it replayable.
//
// The four types that do carry a symbol (R, A, F, P) are also matched directly.
// That covers the ordering case where an order for the symbol appears before
// its Stock Directory entry, which is rare but not impossible in a sliced file
// that begins mid-day.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "feed.hpp"

namespace itchstream {

// Where a dump writes. An interface rather than a FILE* so tests can capture
// the bytes and assert on them exactly, and so the CLI owns the file handle.
class DumpSink {
public:
    DumpSink() = default;
    virtual ~DumpSink() = default;

    DumpSink(const DumpSink&) = delete;
    DumpSink& operator=(const DumpSink&) = delete;

    // Returns false on a write failure. The dump stops and reports it.
    [[nodiscard]] virtual bool write(const void* data, std::size_t size) = 0;
};

struct DumpOptions {
    std::string symbol;  // 1..8 characters; matched case-insensitively
    bool text = false;   // false: a valid ITCH stream, true: one line per message
};

struct DumpResult {
    WalkResult walk{};
    std::array<std::uint64_t, 256> by_type{};  // messages written, per type
    std::uint64_t messages = 0;
    std::uint64_t bytes_written = 0;
    std::optional<std::uint16_t> locate;  // learned from R, or from A/F/P
    bool symbol_found = false;            // some message carried this symbol
    bool write_failed = false;
    std::string error;  // set when write_failed
};

// True for a symbol the exchange could print: 1..8 characters of letters,
// digits and the punctuation ITCH uses ('.', '-', '$'), no spaces.
[[nodiscard]] bool is_valid_symbol(std::string_view symbol) noexcept;

// Uppercase A-Z. Exchange symbols are uppercase by convention, so a user typing
// "aapl" must get the same dump as one typing "AAPL".
[[nodiscard]] std::string normalize_symbol(std::string_view symbol);

// Walk [data, data + size), writing one symbol's messages to `sink`.
//
// In binary mode the wire bytes are copied verbatim behind a rebuilt 2-byte
// length prefix, so the output is a valid, replayable ITCH file and nothing in
// the message is re-encoded. In text mode every field is rendered on one line,
// still without allocating per message.
[[nodiscard]] DumpResult dump_symbol(const std::byte* data, std::size_t size,
                                     const DumpOptions& options, DumpSink& sink);

}  // namespace itchstream
