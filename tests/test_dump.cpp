// M2 dump tests: filtering by symbol, byte-exact output, text rendering.
//
// The interesting cases are not "does it find AAPL": they are the ordering
// case (an order before its directory entry), the locate-only case (a message
// that carries no symbol), and the guarantee that the binary output is a valid
// stream rather than a plausible one.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "itchstream/dump.hpp"
#include "itchstream/feed.hpp"
#include "itchstream/symbol_filter.hpp"
#include "test_bytes.hpp"

using namespace itchstream;
using itchstream_test::add_order;
using itchstream_test::Bytes;
using itchstream_test::executed;
using itchstream_test::frame;
using itchstream_test::noii;
using itchstream_test::stock_directory;
using itchstream_test::trade;
using itchstream_test::trading_action;

namespace {

// Timestamps chosen so the text form is readable: 09:30:00.000021223.
constexpr std::uint64_t kTs = 34'200'000'021'223;
constexpr std::uint64_t kTsPlus1ms = kTs + 1'000'000;

class StringSink final : public MessageSink {
public:
    [[nodiscard]] bool write(const void* data, std::size_t size) override {
        if (writes_ >= limit) return false;
        ++writes_;
        bytes.append(static_cast<const char*>(data), size);
        return true;
    }

    std::string bytes;
    std::size_t limit = static_cast<std::size_t>(-1);
    std::size_t writes_ = 0;
};

struct Dumped {
    DumpResult result;
    std::string bytes;
    std::string text;
};

Dumped dump(const Bytes& stream, std::string_view symbol, bool text = false) {
    Dumped out;
    StringSink sink;
    DumpOptions options;
    options.symbol = std::string(symbol);
    options.text = text;
    out.result = dump_symbol(stream.data(), stream.size(), options, sink);
    if (text) {
        out.text = sink.bytes;
    } else {
        out.bytes = sink.bytes;
    }
    return out;
}

}  // namespace

TEST_CASE("symbols are validated and normalised the way the exchange prints them",
          "[dump][FR-5]") {
    CHECK(is_valid_symbol("AAPL"));
    CHECK(is_valid_symbol("BRK.A"));
    CHECK(is_valid_symbol("RDS-A"));
    CHECK(is_valid_symbol("A$"));
    CHECK(is_valid_symbol("Z"));

    CHECK_FALSE(is_valid_symbol(""));
    CHECK_FALSE(is_valid_symbol("TOOLONG12"));  // nine characters
    CHECK_FALSE(is_valid_symbol("A B"));       // a space is padding, not a symbol
    CHECK_FALSE(is_valid_symbol("A\tB"));
    CHECK_FALSE(is_valid_symbol("A/B"));

    CHECK(normalize_symbol("aapl") == "AAPL");
    CHECK(normalize_symbol("Brk.A") == "BRK.A");
    CHECK(normalize_symbol("AAPL") == "AAPL");
}

TEST_CASE("a binary dump is the symbol's messages, byte for byte", "[dump][FR-5]") {
    const Bytes aapl_directory = stock_directory(kTs, 1, "AAPL");
    const Bytes msft_directory = stock_directory(kTs, 2, "MSFT");
    const Bytes aapl_add = add_order(kTs, 1, 1001, 'B', 100, "AAPL", 1'234'500);
    const Bytes msft_add = add_order(kTs, 2, 2001, 'S', 200, "MSFT", 98'7600);
    const Bytes aapl_noii = noii(kTs, 1);
    const Bytes aapl_executed = executed(kTs, 1, 1001, 50, 7);
    const Bytes aapl_trade = trade(kTs, 1, 1001, 'B', 50, "AAPL", 1'234'500, 8);
    const Bytes aapl_action = trading_action(kTs, 1, "AAPL");

    Bytes stream;
    stream.append(frame(Bytes{}.u8('S').u16(0).u16(0).ts(kTs).ch('O')));
    stream.append(frame(aapl_directory));
    stream.append(frame(msft_directory));
    stream.append(frame(aapl_add));
    stream.append(frame(msft_add));
    stream.append(frame(aapl_noii));
    stream.append(frame(aapl_executed));
    stream.append(frame(aapl_trade));
    stream.append(frame(aapl_action));

    const Dumped out = dump(stream, "AAPL");
    REQUIRE(out.result.walk.ok());
    CHECK(out.result.symbol_found);
    CHECK(out.result.locate == std::uint16_t{1});
    CHECK(out.result.messages == 6);

    // Exactly the AAPL messages, in file order, each still framed. The 'S' with
    // locate 0, both MSFT messages, and nothing else.
    Bytes expected;
    expected.append(frame(aapl_directory));
    expected.append(frame(aapl_add));
    expected.append(frame(aapl_noii));
    expected.append(frame(aapl_executed));
    expected.append(frame(aapl_trade));
    expected.append(frame(aapl_action));

    REQUIRE(out.bytes.size() == expected.size());
    CHECK(std::string_view(out.bytes) ==
          std::string_view(reinterpret_cast<const char*>(expected.data()), expected.size()));
    CHECK(out.result.bytes_written == out.bytes.size());

    // And the output is a valid stream, not merely a plausible one.
    const WalkResult reparse = walk(reinterpret_cast<const std::byte*>(out.bytes.data()),
                                    out.bytes.size(), [](const RawMessage&) {
                                        return WalkAction::Continue;
                                    });
    CHECK(reparse.ok());
    CHECK(reparse.messages == 6);
}

TEST_CASE("per-type counts in the result match the messages written", "[dump][FR-5]") {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 3, "SPY")));
    stream.append(frame(add_order(kTs, 3, 11, 'B', 100, "SPY", 1)));
    stream.append(frame(add_order(kTs, 3, 12, 'S', 100, "SPY", 2)));
    stream.append(frame(executed(kTs, 3, 11, 100, 1)));
    stream.append(frame(noii(kTs, 3)));

    const Dumped out = dump(stream, "SPY");
    REQUIRE(out.result.walk.ok());
    CHECK(out.result.by_type[static_cast<std::uint8_t>('R')] == 1);
    CHECK(out.result.by_type[static_cast<std::uint8_t>('A')] == 2);
    CHECK(out.result.by_type[static_cast<std::uint8_t>('E')] == 1);
    CHECK(out.result.by_type[static_cast<std::uint8_t>('I')] == 1);

    std::uint64_t total = 0;
    for (const std::uint64_t count : out.result.by_type) total += count;
    CHECK(total == out.result.messages);
}

TEST_CASE("an order before its directory entry is still matched by symbol", "[dump][FR-5]") {
    // A slice that starts mid-day can begin with activity for a symbol whose
    // Stock Directory message has already passed.
    Bytes stream;
    stream.append(frame(add_order(kTs, 9, 500, 'B', 100, "AAPL", 1)));
    stream.append(frame(stock_directory(kTs, 9, "AAPL")));
    stream.append(frame(executed(kTs, 9, 500, 100, 1)));

    const Dumped out = dump(stream, "AAPL");
    REQUIRE(out.result.walk.ok());
    CHECK(out.result.messages == 3);  // add, directory, execute
    CHECK(out.result.locate == std::uint16_t{9});
}

TEST_CASE("a locate-only message before the directory entry cannot be matched", "[dump][FR-5]") {
    // Documented limitation: 'E' carries no symbol, so before the directory
    // entry names the locate there is nothing to compare.
    Bytes stream;
    stream.append(frame(executed(kTs, 9, 500, 100, 1)));
    stream.append(frame(stock_directory(kTs, 9, "AAPL")));

    const Dumped out = dump(stream, "AAPL");
    REQUIRE(out.result.walk.ok());
    CHECK(out.result.messages == 1);
    CHECK(out.result.by_type[static_cast<std::uint8_t>('E')] == 0);
}

TEST_CASE("a symbol lookup is case-insensitive and the locate is learnt once", "[dump][FR-5]") {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 4, "TSLA")));
    stream.append(frame(add_order(kTs, 4, 1, 'B', 100, "TSLA", 1)));

    const Dumped upper = dump(stream, "TSLA");
    const Dumped lower = dump(stream, "tsla");
    CHECK(upper.bytes == lower.bytes);
    CHECK(upper.result.messages == 2);
}

TEST_CASE("an unknown symbol writes nothing and says so", "[dump][FR-5]") {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 1, "AAPL")));
    stream.append(frame(add_order(kTs, 1, 1, 'B', 100, "AAPL", 1)));

    const Dumped out = dump(stream, "NONE");
    REQUIRE(out.result.walk.ok());
    CHECK_FALSE(out.result.symbol_found);
    CHECK_FALSE(out.result.locate.has_value());
    CHECK(out.result.messages == 0);
    CHECK(out.bytes.empty());
}

TEST_CASE("the text format names every field on one line", "[dump][FR-5]") {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 1, "AAPL")));
    stream.append(frame(add_order(kTs, 1, 1001, 'B', 100, "AAPL", 1'234'500)));
    stream.append(frame(executed(kTsPlus1ms, 1, 1001, 100, 7)));
    stream.append(frame(noii(kTsPlus1ms + 1, 1)));

    const Dumped out = dump(stream, "AAPL", /*text=*/true);
    REQUIRE(out.result.walk.ok());

    const std::string expected =
        "09:30:00.000021223  R     1 symbol=AAPL mkt=Q lot=100\n"
        "09:30:00.000021223  A     1 ref=1001 side=B shares=100 price=123.4500 symbol=AAPL\n"
        "09:30:00.001021223  E     1 ref=1001 shares=100 match=7\n"
        "09:30:00.001021224  I     1 raw=50B\n";
    CHECK(out.text == expected);
}

TEST_CASE("text and binary dumps contain the same messages", "[dump][FR-5]") {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 1, "AAPL")));
    stream.append(frame(add_order(kTs, 1, 1, 'B', 100, "AAPL", 1)));
    stream.append(frame(stock_directory(kTs, 2, "MSFT")));
    stream.append(frame(executed(kTs, 2, 2, 100, 1)));

    const Dumped binary = dump(stream, "AAPL");
    const Dumped text = dump(stream, "AAPL", /*text=*/true);

    CHECK(binary.result.messages == text.result.messages);
    CHECK(binary.result.by_type == text.result.by_type);
    CHECK(text.text.find("MSFT") == std::string::npos);
    CHECK(text.text.find("ref=1 side=B") != std::string::npos);
}

TEST_CASE("a sink failure stops the dump and is reported", "[dump][FR-5]") {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 1, "AAPL")));
    stream.append(frame(add_order(kTs, 1, 1, 'B', 100, "AAPL", 1)));
    stream.append(frame(add_order(kTs, 1, 2, 'S', 100, "AAPL", 2)));

    StringSink sink;
    sink.limit = 3;  // header + body for message one, then the header of two
    DumpOptions options;
    options.symbol = "AAPL";

    const DumpResult result = dump_symbol(stream.data(), stream.size(), options, sink);
    CHECK(result.write_failed);
    CHECK_FALSE(result.error.empty());
    CHECK(result.messages == 1);
    CHECK(result.bytes_written == 2 + StockDirectory::kWireSize);

    // The walker still ran to completion; a failed sink is not bad data.
    CHECK(result.walk.ok());
}

TEST_CASE("a truncated tail leaves a partial dump and a walk error", "[dump][FR-5]") {
    Bytes stream;
    stream.append(frame(stock_directory(kTs, 1, "AAPL")));
    stream.append(frame(add_order(kTs, 1, 1, 'B', 100, "AAPL", 1)));
    const Bytes complete = stream;
    stream.resize(stream.size() - 1);

    const Dumped out = dump(stream, "AAPL");
    CHECK_FALSE(out.result.walk.ok());
    CHECK(out.result.walk.error == WalkError::TruncatedMessage);
    CHECK(out.result.messages == 1);  // the directory entry only
    CHECK(out.bytes.size() == 2 + StockDirectory::kWireSize);

    // The same input twice must produce the same partial dump.
    const Dumped again = dump(stream, "AAPL");
    CHECK(again.bytes == out.bytes);
    CHECK(again.result.messages == out.result.messages);
    CHECK(complete.size() > stream.size());
}
