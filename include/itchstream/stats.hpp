#pragma once

// Stats mode (FR-4): counts per type, first/last timestamp, bytes, wall time.
//
// This is also the M1 throughput benchmark. The visitor does the minimum work
// per message and, unless `length_only` is set, decodes every known type so the
// number covers real parsing rather than only the length walk.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "feed.hpp"

namespace itchstream {

struct TypeTally {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;  // body bytes, excluding the 2-byte length prefix
};

struct Stats {
    std::uint64_t messages = 0;
    std::uint64_t bytes_consumed = 0;
    std::uint64_t map_bytes = 0;
    std::array<TypeTally, 256> by_type{};
    std::uint64_t decoded = 0;
    std::uint64_t decode_failures = 0;
    std::uint64_t skipped = 0;
    bool have_timestamp = false;
    std::uint64_t first_timestamp = 0;
    std::uint64_t last_timestamp = 0;
    bool timestamps_monotonic = true;  // exchange timestamps must never go backwards
    WalkResult walk{};
};

struct StatsOptions {
    // Decode every known type and count failures. Off means "walk and count
    // only", which is the cheapest useful parse benchmark and the number to
    // quote when isolating broker-feed cost from payload cost.
    bool decode_payloads = true;
};

[[nodiscard]] Stats collect(const std::byte* data, std::size_t size, const StatsOptions& options);

// HH:MM:SS.nnnnnnnnn from nanoseconds since midnight.
[[nodiscard]] std::string format_timestamp(std::uint64_t ns_since_midnight);

// 1234567 -> "1,234,567"
[[nodiscard]] std::string format_count(std::uint64_t value);

// Host description for publishing next to a benchmark: CPU, cores, RAM, OS.
[[nodiscard]] std::string machine_summary();

// Compiler, build type and flags actually used for this binary.
[[nodiscard]] std::string build_summary();

}  // namespace itchstream
