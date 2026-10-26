// M3 replay tests: re-emission must be byte-exact, and the pacing must be
// driven by the feed's own timestamps rather than by the wall clock.
//
// The timing checks are deliberately loose — a loaded CI box can wake a sleep
// tens of milliseconds late — but they are not vacuous: they compare a paced run
// against a target it can only miss by ignoring the timestamps or the speed
// factor, which is exactly the failure the tests exist to catch.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "itchstream/dump.hpp"
#include "itchstream/feed.hpp"
#include "itchstream/replay.hpp"
#include "test_bytes.hpp"

using namespace itchstream;
using itchstream_test::add_order;
using itchstream_test::Bytes;
using itchstream_test::executed;
using itchstream_test::frame;
using itchstream_test::stock_directory;
using itchstream_test::trade;

namespace {

// 09:30:00.000021223, the same instant the dump tests use.
constexpr std::uint64_t kTs = 34'200'000'021'223;

class CaptureSink final : public MessageSink {
public:
    [[nodiscard]] bool write(const void* data, std::size_t size) override {
        if (writes_ >= write_limit) return false;
        ++writes_;
        bytes.append(static_cast<const char*>(data), size);
        return true;
    }

    [[nodiscard]] bool flush() override {
        ++flushes;
        return flush_ok;
    }

    std::string bytes;
    std::size_t write_limit = static_cast<std::size_t>(-1);  // writes, not messages
    std::size_t writes_ = 0;
    std::size_t flushes = 0;
    bool flush_ok = true;
};

// Two symbols, interleaved, so a symbol replay has something to exclude.
Bytes sample_stream() {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 14, "AAPL")));
    stream.append(frame(stock_directory(kTs, 15, "MSFT")));
    stream.append(frame(add_order(kTs + 1'000'000, 14, 1001, 'B', 200, "AAPL", 1'2345)));
    stream.append(frame(add_order(kTs + 2'000'000, 15, 2001, 'S', 100, "MSFT", 3'2100)));
    stream.append(frame(executed(kTs + 3'000'000, 14, 1001, 200, 7)));
    stream.append(frame(trade(kTs + 4'000'000, 15, 2001, 'S', 100, "MSFT", 3'2100, 8)));
    stream.append(frame(executed(kTs + 5'000'000, 14, 1001, 0, 9)));
    return stream;
}

ReplayResult run(const Bytes& stream, ReplayOptions options, CaptureSink& sink) {
    return replay(stream.data(), stream.size(), options, sink);
}

double wall_seconds_of(const std::function<void()>& body) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto finish = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(finish - start).count();
}

}  // namespace

TEST_CASE("replay re-emits the feed byte for byte at any speed", "[replay][FR-11]") {
    const Bytes stream = sample_stream();

    // 1x and 1000x must produce the same bytes: speed changes timing, never data.
    for (const double speed : {1.0, 1000.0}) {
        CaptureSink sink;
        ReplayOptions options;
        options.speed = speed;
        const ReplayResult result = run(stream, options, sink);

        CHECK(result.walk.ok());
        CHECK_FALSE(result.write_failed);
        CHECK(result.emitted == 7);
        CHECK(result.bytes_written == stream.size());
        CHECK(sink.bytes.size() == stream.size());
        CHECK(std::string(sink.bytes) ==
              std::string(reinterpret_cast<const char*>(stream.data()), stream.size()));
        CHECK(result.feed_seconds > 0.0049);
        CHECK(result.feed_seconds < 0.0051);
    }
}

TEST_CASE("replay --limit stops after the requested number of messages", "[replay]") {
    const Bytes stream = sample_stream();

    CaptureSink sink;
    ReplayOptions options;
    options.speed = 1000.0;
    options.limit = 2;
    const ReplayResult result = run(stream, options, sink);

    CHECK(result.walk.ok());
    CHECK(result.walk.stopped_early);
    CHECK(result.emitted == 2);
    CHECK(result.bytes_written == 2 + 39 + 2 + 39);  // two stock directory messages
    CHECK(result.feed_seconds == 0.0);  // both carry the same timestamp
}

TEST_CASE("replay --symbol emits exactly what dump would", "[replay][FR-5]") {
    const Bytes stream = sample_stream();

    CaptureSink dump_sink;
    DumpOptions dump_options;
    dump_options.symbol = "AAPL";
    const DumpResult dumped = dump_symbol(stream.data(), stream.size(), dump_options, dump_sink);

    CaptureSink replay_sink;
    ReplayOptions options;
    options.speed = 1000.0;
    options.symbol = "AAPL";
    const ReplayResult replayed = run(stream, options, replay_sink);

    CHECK(replayed.walk.ok());
    CHECK(replayed.symbol_found);
    CHECK(replayed.locate.has_value());
    CHECK(*replayed.locate == 14);
    CHECK(replayed.emitted == 4);  // R, A, E, E — not the two MSFT messages
    CHECK(replay_sink.bytes == dump_sink.bytes);
    CHECK(replayed.emitted == dumped.messages);
}

TEST_CASE("replay of a symbol that is not in the file emits nothing", "[replay][FR-5]") {
    const Bytes stream = sample_stream();

    CaptureSink sink;
    ReplayOptions options;
    options.symbol = "NOPE";
    const ReplayResult result = run(stream, options, sink);

    CHECK(result.walk.ok());
    CHECK_FALSE(result.symbol_found);
    CHECK_FALSE(result.locate.has_value());
    CHECK(result.emitted == 0);
    CHECK(sink.bytes.empty());
}

TEST_CASE("pacing follows the feed's timestamps and the speed factor", "[replay]") {
    // Six messages 20 ms apart: a 100 ms feed span.
    Bytes stream;
    for (int i = 0; i < 6; ++i) {
        stream.append(frame(add_order(kTs + static_cast<std::uint64_t>(i) * 20'000'000, 14,
                                      static_cast<std::uint64_t>(100 + i), 'B', 100, "AAPL", 1000)));
    }

    CaptureSink real_time_sink;
    ReplayOptions real_time;
    real_time.speed = 1.0;
    ReplayResult real_time_result;
    const double real_time_wall =
        wall_seconds_of([&] { real_time_result = run(stream, real_time, real_time_sink); });

    CHECK(real_time_result.emitted == 6);
    CHECK(real_time_result.target_seconds > 0.099);
    CHECK(real_time_result.target_seconds < 0.101);
    CHECK(real_time_wall >= 0.09);  // it waited out the feed's own span
    CHECK(real_time_wall < 1.0);    // and did not lose the plot
    CHECK(real_time_sink.flushes >= 5);  // each paced message reached the consumer

    CaptureSink fast_sink;
    ReplayOptions fast;
    fast.speed = 50.0;
    ReplayResult fast_result;
    const double fast_wall = wall_seconds_of([&] { fast_result = run(stream, fast, fast_sink); });

    CHECK(fast_result.emitted == 6);
    CHECK(fast_result.target_seconds > 0.0019);
    CHECK(fast_result.target_seconds < 0.0021);
    CHECK(fast_wall < 0.05);  // half the unpaced span: speed was actually applied
    CHECK(fast_sink.bytes == real_time_sink.bytes);
}

TEST_CASE("a timestamp that goes backwards does not stall the replay", "[replay]") {
    Bytes stream;
    stream.append(frame(add_order(kTs + 10'000'000, 14, 1, 'B', 100, "AAPL", 1000)));
    stream.append(frame(add_order(kTs, 14, 2, 'B', 100, "AAPL", 1000)));
    stream.append(frame(add_order(kTs + 5'000'000, 14, 3, 'B', 100, "AAPL", 1000)));

    CaptureSink sink;
    ReplayOptions options;
    options.speed = 1.0;
    ReplayResult result;
    const double wall = wall_seconds_of([&] { result = run(stream, options, sink); });

    CHECK(result.walk.ok());
    CHECK(result.emitted == 3);
    CHECK(wall < 0.5);  // a wrapped deadline would hang for centuries
    CHECK(result.feed_seconds == 0.0);  // the last timestamp is behind the first
    CHECK(sink.bytes.size() == stream.size());
}

TEST_CASE("messages without a timestamp inherit the previous pace", "[replay]") {
    // An unknown type with a body too short to hold a timestamp. The walker
    // admits it; replay must emit it without inventing a time for it.
    const Bytes short_message = Bytes{}.u8('Z').zeros(3);

    Bytes stream;
    stream.append(frame(add_order(kTs, 14, 1, 'B', 100, "AAPL", 1000)));
    stream.append(frame(short_message));
    stream.append(frame(add_order(kTs + 1'000'000, 14, 2, 'B', 100, "AAPL", 1000)));

    CaptureSink sink;
    ReplayOptions options;
    ReplayResult result;
    const double wall = wall_seconds_of([&] { result = run(stream, options, sink); });

    CHECK(result.walk.ok());
    CHECK(result.emitted == 3);
    CHECK(result.have_timestamp);
    CHECK(result.feed_seconds > 0.0009);
    CHECK(result.feed_seconds < 0.0011);
    CHECK(wall < 0.5);
    CHECK(std::string(sink.bytes) ==
          std::string(reinterpret_cast<const char*>(stream.data()), stream.size()));
}

TEST_CASE("a feed with no timestamps runs unpaced", "[replay]") {
    Bytes stream;
    stream.append(frame(Bytes{}.u8('Z').zeros(3)));
    stream.append(frame(Bytes{}.u8('Y').zeros(5)));

    CaptureSink sink;
    ReplayResult result;
    const double wall = wall_seconds_of([&] { result = run(stream, ReplayOptions{}, sink); });

    CHECK(result.walk.ok());
    CHECK_FALSE(result.have_timestamp);
    CHECK(result.emitted == 2);
    CHECK(result.feed_seconds == 0.0);
    CHECK(result.target_seconds == 0.0);
    CHECK(wall < 0.5);
}

TEST_CASE("a write failure stops the replay and is reported", "[replay]") {
    const Bytes stream = sample_stream();

    CaptureSink sink;
    sink.write_limit = 2;  // one whole message, then the next message's header
    ReplayOptions options;
    options.speed = 1000.0;
    const ReplayResult result = run(stream, options, sink);

    CHECK(result.write_failed);
    CHECK_FALSE(result.error.empty());
    CHECK(result.emitted == 1);
    CHECK(result.bytes_written == 2 + 39);
    CHECK(sink.bytes.size() == 2 + 39);
}

TEST_CASE("a flush failure is a write failure", "[replay]") {
    const Bytes stream = sample_stream();

    CaptureSink sink;
    sink.flush_ok = false;
    const ReplayResult result = run(stream, ReplayOptions{}, sink);

    CHECK(result.write_failed);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("a truncated feed stops the replay with an error", "[replay][FR-10]") {
    Bytes stream = sample_stream();
    stream.u16(40);   // a length prefix ...
    stream.zeros(10); // ... with 30 bytes missing

    CaptureSink sink;
    ReplayOptions options;
    options.speed = 1000.0;
    const ReplayResult result = run(stream, options, sink);

    CHECK_FALSE(result.walk.ok());
    CHECK(result.walk.error == WalkError::TruncatedMessage);
    CHECK(result.emitted == 7);
    CHECK(result.bytes_written == sample_stream().size());
    CHECK_FALSE(result.write_failed);
}
