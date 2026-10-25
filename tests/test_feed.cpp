// Feed, framing and malformed-input tests (FR-1, FR-4, FR-5 groundwork, FR-10).
//
// The malformed cases matter more than the happy path: they are what stops a
// truncated download from becoming an out-of-bounds read.

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "itchstream/decoder.hpp"
#include "itchstream/feed.hpp"
#include "itchstream/stats.hpp"
#include "test_bytes.hpp"

using namespace itchstream;
using itchstream_test::Bytes;
using itchstream_test::frame;

namespace {

constexpr std::uint64_t kFirstTimestamp = 1000;
constexpr std::uint64_t kLastTimestamp = 1012;
constexpr std::uint64_t kMessageCount = 13;

Bytes add_order(std::uint64_t ts, std::uint64_t ref, char side, std::uint32_t shares,
                std::string_view symbol, std::uint32_t price) {
    return Bytes{}
        .u8('A')
        .u16(1)
        .u16(2)
        .ts(ts)
        .u64(ref)
        .ch(side)
        .u32(shares)
        .chars(symbol, 8)
        .u32(price);  // 1 + 2 + 2 + 6 + 8 + 1 + 4 + 8 + 4 = 36
}

// A small but complete file: every decoded type once, one skipped type, and
// non-decreasing timestamps from kFirstTimestamp to kLastTimestamp.
Bytes sample_stream() {
    Bytes out;

    out.append(frame(Bytes{}.u8('S').u16(0).u16(0).ts(1000).ch('O')));

    out.append(frame(Bytes{}
                         .u8('R')
                         .u16(1)
                         .u16(0)
                         .ts(1001)
                         .chars("AAPL", 8)
                         .ch('Q')
                         .ch('N')
                         .u32(100)
                         .ch('N')
                         .ch('C')
                         .chars("  ", 2)
                         .ch('P')
                         .ch('N')
                         .ch('N')
                         .ch('1')
                         .ch('N')
                         .u32(0)
                         .ch('N')));

    out.append(frame(add_order(1002, 10, 'B', 300, "AAPL", 1234500)));
    out.append(frame(add_order(1003, 11, 'S', 200, "AAPL", 1234600)));

    out.append(frame(Bytes{}
                         .u8('F')
                         .u16(1)
                         .u16(0)
                         .ts(1004)
                         .u64(12)
                         .ch('B')
                         .u32(100)
                         .chars("AAPL", 8)
                         .u32(1234400)
                         .chars("NSDQ", 4)));

    out.append(frame(Bytes{}.u8('E').u16(1).u16(0).ts(1005).u64(10).u32(100).u64(1)));
    out.append(frame(Bytes{}.u8('C').u16(1).u16(0).ts(1006).u64(11).u32(50).u64(2).ch('Y').u32(1234500)));
    out.append(frame(Bytes{}.u8('X').u16(1).u16(0).ts(1007).u64(11).u32(25)));
    out.append(frame(Bytes{}.u8('D').u16(1).u16(0).ts(1008).u64(12)));
    out.append(frame(Bytes{}.u8('U').u16(1).u16(0).ts(1009).u64(10).u64(13).u32(200).u32(1234400)));

    out.append(frame(Bytes{}
                         .u8('P')
                         .u16(1)
                         .u16(0)
                         .ts(1010)
                         .u64(13)
                         .ch('B')
                         .u32(100)
                         .chars("AAPL", 8)
                         .u32(1234500)
                         .u64(3)));

    // 'H' (Stock Trading Action, 25 bytes) is not decoded by this version: the
    // walker must skip it by length and keep going.
    out.append(frame(Bytes{}
                         .u8('H')
                         .u16(1)
                         .u16(0)
                         .ts(1011)
                         .chars("AAPL", 8)
                         .ch('T')
                         .ch(' ')
                         .chars("    ", 4)));

    out.append(frame(Bytes{}.u8('S').u16(0).u16(0).ts(1012).ch('C')));
    return out;
}

class TempFile {
public:
    TempFile(std::string_view name, const Bytes& bytes) {
        path_ = std::filesystem::temp_directory_path() /
                (std::string("itchstream_test_") + std::string(name) + ".itch");
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (bytes.size() > 0) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        out.close();
        REQUIRE(out.good());
    }

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

WalkResult walk_all(const Bytes& bytes) {
    return walk(bytes.data(), bytes.size(), [](const RawMessage&) { return WalkAction::Continue; });
}

}  // namespace

TEST_CASE("FR-1: walk hands over every message in file order", "[feed][FR-1]") {
    const Bytes bytes = sample_stream();

    std::vector<MsgType> seen;
    std::size_t first_offset = 0;
    bool saw_skipped_type = false;

    const WalkResult result = walk(bytes.data(), bytes.size(), [&](const RawMessage& message) {
        if (seen.empty()) first_offset = static_cast<std::size_t>(message.payload - bytes.data());
        if (!is_decoded(message.type)) saw_skipped_type = true;
        seen.push_back(message.type);
        return WalkAction::Continue;
    });

    REQUIRE(result.ok());
    CHECK(result.messages == kMessageCount);
    CHECK(result.messages == seen.size());
    CHECK(result.bytes_consumed == bytes.size());
    CHECK(first_offset == 2);  // past the first 2-byte length prefix
    CHECK(seen.front() == MsgType::SystemEvent);
    CHECK(seen.back() == MsgType::SystemEvent);
    CHECK(saw_skipped_type);
}

TEST_CASE("the visitor can stop the walk without it being an error", "[feed]") {
    const Bytes bytes = sample_stream();

    const WalkResult result = walk(bytes.data(), bytes.size(),
                                   [](const RawMessage&) { return WalkAction::Stop; });

    CHECK(result.ok());
    CHECK(result.stopped_early);
    CHECK(result.messages == 0);
    CHECK(result.bytes_consumed == 0);
}

TEST_CASE("FR-10: a truncated tail is an error, not an out-of-bounds read",
          "[feed][FR-10]") {
    // Length prefix claims 36 bytes, only 10 follow.
    const Bytes bytes = Bytes{}.u16(36).u8('A').zeros(9);

    const WalkResult result = walk_all(bytes);

    CHECK(result.error == WalkError::TruncatedMessage);
    CHECK(result.error_offset == 0);
    CHECK(result.observed_length == 36);
    CHECK(result.messages == 0);
    CHECK(describe(result).find("offset 0") != std::string::npos);
}

TEST_CASE("FR-10: a stray final byte is a truncated header", "[feed][FR-10]") {
    Bytes bytes = sample_stream();
    bytes.u8(0x7F);

    const WalkResult result = walk_all(bytes);

    CHECK(result.error == WalkError::TruncatedHeader);
    CHECK(result.error_offset == bytes.size() - 1);
    CHECK(result.messages == kMessageCount);
}

TEST_CASE("FR-10: a zero length prefix is an error", "[feed][FR-10]") {
    const Bytes bytes = Bytes{}.u16(0).u8('S').zeros(20);

    const WalkResult result = walk_all(bytes);

    CHECK(result.error == WalkError::ZeroLength);
    CHECK(result.error_offset == 0);
}

TEST_CASE("FR-10: a length that disagrees with the spec is an error", "[feed][FR-10]") {
    // A 30-byte message whose type byte says Add Order (36 bytes). Trusting the
    // prefix here would desynchronise every following message.
    Bytes bytes;
    bytes.u16(30).u8('A').zeros(29);

    const WalkResult result = walk_all(bytes);

    CHECK(result.error == WalkError::LengthMismatch);
    CHECK(result.error_type == MsgType::AddOrder);
    CHECK(result.expected_length == 36);
    CHECK(result.observed_length == 30);
    CHECK(describe(result).find("expected 36") != std::string::npos);
}

TEST_CASE("FR-10: an implausible length prefix is rejected before it is skipped",
          "[feed][FR-10]") {
    // 65 bytes exceeds the largest ITCH 5.0 message. The body is present, so
    // this is not truncation: it is corruption that must not be skipped.
    Bytes bytes;
    bytes.u16(65).u8('Z').zeros(64);

    const WalkResult result = walk_all(bytes);

    CHECK(result.error == WalkError::LengthTooLarge);
    CHECK(result.observed_length == 65);
}

TEST_CASE("an empty input is zero messages and no error", "[feed]") {
    const WalkResult result = walk(nullptr, 0, [](const RawMessage&) { return WalkAction::Continue; });

    CHECK(result.ok());
    CHECK(result.messages == 0);
    CHECK(result.bytes_consumed == 0);
}

TEST_CASE("FR-10: every prefix of a valid stream walks cleanly or errors, never both",
          "[feed][FR-10]") {
    const Bytes full = sample_stream();

    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        Bytes prefix = full;
        prefix.resize(cut);

        const WalkResult result = walk_all(prefix);

        // Either the prefix ends exactly on a message boundary, or the walk
        // stops at the last complete message with an error at a real offset.
        if (result.ok()) {
            CHECK(result.bytes_consumed == cut);
        } else {
            CHECK(result.bytes_consumed < cut);
            CHECK(result.error_offset < cut);
            CHECK(result.error != WalkError::None);
        }
        CHECK(result.messages <= kMessageCount);
    }
}

TEST_CASE("the same bytes walk to the same result twice", "[feed][FR-11]") {
    const Bytes bytes = sample_stream();

    const WalkResult first = walk_all(bytes);
    const WalkResult second = walk_all(bytes);

    CHECK(first.messages == second.messages);
    CHECK(first.bytes_consumed == second.bytes_consumed);
    CHECK(first.error == second.error);
    CHECK(first.error_offset == second.error_offset);
}

TEST_CASE("FR-1: MappedFile maps a file read-only", "[feed][FR-1]") {
    const Bytes bytes = sample_stream();
    const TempFile file("map", bytes);

    MappedFile mapped;
    std::string error;
    REQUIRE(mapped.open(file.path(), error));
    CHECK(error.empty());
    CHECK(mapped.is_open());
    CHECK(mapped.size() == bytes.size());
    REQUIRE(mapped.data() != nullptr);
    CHECK(std::memcmp(mapped.data(), bytes.data(), bytes.size()) == 0);
    CHECK(mapped.path() == file.path());

    // The mapping is walkable directly, which is the whole point: no copy.
    CHECK(walk_all(bytes).messages == kMessageCount);
}

TEST_CASE("FR-1: a missing file reports an error instead of crashing", "[feed][FR-1]") {
    MappedFile mapped;
    std::string error;

    CHECK_FALSE(mapped.open("itchstream-does-not-exist.itch", error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(mapped.is_open());
    CHECK(mapped.data() == nullptr);
    CHECK(mapped.size() == 0);
}

TEST_CASE("FR-1: an empty file opens with size zero", "[feed][FR-1]") {
    const Bytes empty;
    const TempFile file("empty", empty);

    MappedFile mapped;
    std::string error;
    REQUIRE(mapped.open(file.path(), error));
    CHECK(mapped.is_open());
    CHECK(mapped.size() == 0);
    CHECK(mapped.data() == nullptr);
    CHECK(walk_all(empty).messages == 0);
}

TEST_CASE("FR-4: stats count per type, timestamps and skipped messages", "[stats][FR-4]") {
    const Bytes bytes = sample_stream();

    const Stats stats = collect(bytes.data(), bytes.size(), StatsOptions{});

    CHECK(stats.messages == kMessageCount);
    CHECK(stats.bytes_consumed == bytes.size());
    CHECK(stats.map_bytes == bytes.size());
    CHECK(stats.walk.ok());

    CHECK(stats.decoded == 12);
    CHECK(stats.decode_failures == 0);
    CHECK(stats.skipped == 1);

    CHECK(stats.by_type[static_cast<std::size_t>('S')].messages == 2);
    CHECK(stats.by_type[static_cast<std::size_t>('R')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('A')].messages == 2);
    CHECK(stats.by_type[static_cast<std::size_t>('F')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('E')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('C')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('X')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('D')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('U')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('P')].messages == 1);
    CHECK(stats.by_type[static_cast<std::size_t>('H')].messages == 1);

    CHECK(stats.have_timestamp);
    CHECK(stats.first_timestamp == kFirstTimestamp);
    CHECK(stats.last_timestamp == kLastTimestamp);
    CHECK(stats.timestamps_monotonic);
}

TEST_CASE("stats can skip payload decoding", "[stats][FR-4]") {
    const Bytes bytes = sample_stream();

    StatsOptions options;
    options.decode_payloads = false;
    const Stats stats = collect(bytes.data(), bytes.size(), options);

    CHECK(stats.messages == kMessageCount);
    CHECK(stats.decoded == 0);
    CHECK(stats.skipped == 1);
    CHECK(stats.timestamps_monotonic);
}

TEST_CASE("stats notice timestamps that go backwards", "[stats]") {
    Bytes bytes;
    bytes.append(frame(Bytes{}.u8('S').u16(0).u16(0).ts(5000).ch('O')));
    bytes.append(frame(Bytes{}.u8('S').u16(0).u16(0).ts(4000).ch('C')));

    const Stats stats = collect(bytes.data(), bytes.size(), StatsOptions{});

    CHECK(stats.messages == 2);
    CHECK_FALSE(stats.timestamps_monotonic);
}

TEST_CASE("timestamps and counts format for the report", "[stats]") {
    CHECK(format_timestamp(0) == "00:00:00.000000000");
    CHECK(format_timestamp(34200000000000ULL) == "09:30:00.000000000");
    CHECK(format_timestamp(34200000123456ULL) == "09:30:00.000123456");
    CHECK(format_timestamp(86399999999999ULL) == "23:59:59.999999999");

    CHECK(format_count(0) == "0");
    CHECK(format_count(7) == "7");
    CHECK(format_count(999) == "999");
    CHECK(format_count(1000) == "1,000");
    CHECK(format_count(1234567) == "1,234,567");
    CHECK(format_count(1000000000ULL) == "1,000,000,000");
}
