#pragma once

// M3: re-emit a feed at its own pace.
//
// Pacing is absolute, not incremental. The deadline for a message is
//
//     wall_start + (timestamp - first_timestamp) / speed
//
// so a late wake-up costs that one message and nothing after it, instead of the
// permanent drift an incremental "sleep for the gap since the last message"
// accumulates. Wall clock source is steady_clock, never the feed's own idea of
// time: the exchange timestamp is data, not a clock.
//
// Bytes are re-emitted exactly as they arrived, through the same `write_framed`
// that `dump` uses, so what a consumer sees is the input stream, only delayed.
// Speed changes timing; it cannot change a byte.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "feed.hpp"
#include "sink.hpp"

namespace itchstream {

struct ReplayOptions {
    double speed = 1.0;                 // 1.0 is the feed's own pace, 10.0 is ten times faster
    std::uint64_t limit = 0;            // 0 emits everything
    std::optional<std::string> symbol;  // when set, only this symbol
};

struct ReplayResult {
    WalkResult walk{};
    std::uint64_t emitted = 0;  // messages written
    std::uint64_t bytes_written = 0;
    std::uint64_t first_timestamp = 0;
    std::uint64_t last_timestamp = 0;
    bool have_timestamp = false;
    double feed_seconds = 0.0;    // last - first, in seconds
    double target_seconds = 0.0;  // feed_seconds / speed: the wall time the pacing aimed at
    std::optional<std::uint16_t> locate;
    bool symbol_found = false;
    bool write_failed = false;
    std::string error;  // set when write_failed
};

// Walk [data, data + size), writing messages to `sink` at their own pace.
// Returns when the file ends, `limit` is reached, or a write fails.
[[nodiscard]] ReplayResult replay(const std::byte* data, std::size_t size,
                                 const ReplayOptions& options, MessageSink& sink);

}  // namespace itchstream
