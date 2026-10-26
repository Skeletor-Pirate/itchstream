#pragma once

// M2: isolate one symbol's message stream.
//
// Filtering needs a stock locate, not a symbol compare, because six of the ten
// decoded types do not carry the symbol; `symbol_filter.hpp` owns that rule. A
// dump is the symbol's complete stream, not the decoded subset, which is what
// makes it replayable.
//
// In binary mode the wire bytes are copied verbatim behind a rebuilt 2-byte
// length prefix (see `write_framed`), so nothing in the message is re-encoded.
// In text mode every field is rendered on one line, without allocating per
// message.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "feed.hpp"
#include "sink.hpp"

namespace itchstream {

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

// Walk [data, data + size), writing one symbol's messages to `sink`.
[[nodiscard]] DumpResult dump_symbol(const std::byte* data, std::size_t size,
                                     const DumpOptions& options, MessageSink& sink);

}  // namespace itchstream
