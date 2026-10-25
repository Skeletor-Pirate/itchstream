#include "itchstream/stats.hpp"

#include <cstdio>

#include "itchstream/decoder.hpp"

namespace itchstream {

Stats collect(const std::byte* data, std::size_t size, const StatsOptions& options) {
    Stats stats{};
    stats.map_bytes = size;

    const WalkResult walk_result = walk(data, size, [&](const RawMessage& message) {
        TypeTally& tally = stats.by_type[static_cast<std::uint8_t>(message.type)];
        ++tally.messages;
        tally.bytes += message.length;

        if (const std::optional<std::uint64_t> timestamp = message.timestamp_if_present()) {
            if (!stats.have_timestamp) {
                stats.have_timestamp = true;
                stats.first_timestamp = *timestamp;
            } else if (*timestamp < stats.last_timestamp) {
                // The exchange guarantees non-decreasing timestamps within a
                // file. A regression means the walk is out of step, so surface
                // it rather than publishing stats from a desynchronised parse.
                stats.timestamps_monotonic = false;
            }
            stats.last_timestamp = *timestamp;
        }

        if (!is_decoded(message.type)) {
            ++stats.skipped;
        } else if (options.decode_payloads) {
            if (decode_payload(message.type, message.payload, message.length)) {
                ++stats.decoded;
            } else {
                ++stats.decode_failures;
            }
        }

        return WalkAction::Continue;
    });

    stats.walk = walk_result;
    stats.messages = walk_result.messages;
    stats.bytes_consumed = walk_result.bytes_consumed;
    return stats;
}

std::string format_timestamp(std::uint64_t ns_since_midnight) {
    const std::uint64_t total_seconds = ns_since_midnight / 1'000'000'000ULL;
    const std::uint64_t nanos = ns_since_midnight % 1'000'000'000ULL;
    const std::uint64_t hours = total_seconds / 3600;
    const std::uint64_t minutes = (total_seconds / 60) % 60;
    const std::uint64_t seconds = total_seconds % 60;

    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%02llu:%02llu:%02llu.%09llu",
                  static_cast<unsigned long long>(hours), static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(seconds), static_cast<unsigned long long>(nanos));
    return std::string(buffer);
}

std::string format_count(std::uint64_t value) {
    const std::string digits = std::to_string(value);
    const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;

    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    out.append(digits, 0, lead);
    for (std::size_t i = lead; i < digits.size(); i += 3) {
        out.push_back(',');
        out.append(digits, i, 3);
    }
    return out;
}

}  // namespace itchstream
