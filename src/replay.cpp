#include "itchstream/replay.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "itchstream/symbol_filter.hpp"

namespace itchstream {
namespace {

using Clock = std::chrono::steady_clock;

// A day of nanoseconds divided by a very small speed can leave the range of
// int64. Cap the offset at thirty years; no file represents a longer session,
// and the cast stays defined.
constexpr double kMaxOffsetNanoseconds = 1e18;

}  // namespace

ReplayResult replay(const std::byte* data, std::size_t size, const ReplayOptions& options,
                    MessageSink& sink) {
    ReplayResult result{};

    std::optional<SymbolFilter> filter;
    if (options.symbol.has_value()) filter.emplace(*options.symbol);

    // The CLI rejects a non-positive speed; treating it as real time here keeps
    // a direct library call from dividing by zero or running backwards.
    const double speed = options.speed > 0.0 ? options.speed : 1.0;
    const Clock::time_point wall_start = Clock::now();
    std::uint64_t base_timestamp = 0;

    result.walk = walk(data, size, [&](const RawMessage& message) {
        // Checked before the message, not after it, so the message that ends the
        // replay is the one counted as emitted and the walk's own count agrees.
        if (options.limit != 0 && result.emitted >= options.limit) return WalkAction::Stop;
        if (filter.has_value() && !filter->matches(message)) return WalkAction::Continue;

        bool paced = false;
        if (const std::optional<std::uint64_t> timestamp = message.timestamp_if_present()) {
            if (!result.have_timestamp) {
                result.have_timestamp = true;
                result.first_timestamp = *timestamp;
                base_timestamp = *timestamp;
            }
            result.last_timestamp = *timestamp;

            // A timestamp that went backwards must not wrap into a deadline a
            // few centuries out, so clamp rather than trusting monotonicity.
            const std::uint64_t delta = *timestamp > base_timestamp ? *timestamp - base_timestamp : 0;
            const double offset_ns =
                std::min(static_cast<double>(delta) / speed, kMaxOffsetNanoseconds);
            const Clock::time_point deadline =
                wall_start + std::chrono::nanoseconds(static_cast<std::int64_t>(offset_ns));
            const Clock::time_point now = Clock::now();
            if (deadline > now) {
                std::this_thread::sleep_until(deadline);
                paced = true;
            }
        }

        if (!write_framed(sink, message)) {
            result.write_failed = true;
            result.error = "the output rejected a write";
            return WalkAction::Stop;
        }

        ++result.emitted;
        result.bytes_written += 2 + message.length;

        // A consumer following the pace has to see the bytes when they are due,
        // not when stdio fills a block. When nothing waited (a fast replay) the
        // buffering stays on and the run is not syscall-bound.
        if (paced && !sink.flush()) {
            result.write_failed = true;
            result.error = "the output rejected a flush";
            return WalkAction::Stop;
        }

        return WalkAction::Continue;
    });

    if (!sink.flush()) {
        result.write_failed = true;
        if (result.error.empty()) result.error = "the output rejected a flush";
    }

    if (filter.has_value()) {
        result.locate = filter->locate();
        result.symbol_found = filter->found();
    }
    if (result.have_timestamp) {
        const std::uint64_t span =
            result.last_timestamp > result.first_timestamp ? result.last_timestamp - result.first_timestamp
                                                           : 0;
        result.feed_seconds = static_cast<double>(span) / 1e9;
        result.target_seconds = result.feed_seconds / speed;
    }
    return result;
}

}  // namespace itchstream
