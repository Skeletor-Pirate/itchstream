// Decoder unit tests: one hand-built byte array per message type, every field
// checked, plus the compile-time size assertions.
//
// The byte arrays are built in specification order by hand, not by the code
// under test, so a wrong offset or a wrong width shows up as a failed field
// rather than as a plausible-looking number.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "itchstream/decoder.hpp"
#include "itchstream/endian.hpp"
#include "itchstream/messages.hpp"
#include "test_bytes.hpp"

using namespace itchstream;
using itchstream_test::Bytes;
using itchstream_test::sym8;

namespace {

std::uint64_t payload_timestamp(const Payload& payload) {
    return std::visit([](const auto& message) { return static_cast<std::uint64_t>(message.timestamp); },
                      payload);
}

}  // namespace

TEST_CASE("wire sizes match the specification and the in-memory structs", "[sizes][FR-2]") {
    // The specification's byte counts, written out by hand from the ITCH 5.0
    // document so the table cannot drift with the code.
    CHECK(wire_size(MsgType::SystemEvent) == 12);
    CHECK(wire_size(MsgType::StockDirectory) == 39);
    CHECK(wire_size(MsgType::AddOrder) == 36);
    CHECK(wire_size(MsgType::AddOrderMpid) == 40);
    CHECK(wire_size(MsgType::OrderExecuted) == 31);
    CHECK(wire_size(MsgType::OrderExecutedWithPrice) == 36);
    CHECK(wire_size(MsgType::OrderCancel) == 23);
    CHECK(wire_size(MsgType::OrderDelete) == 19);
    CHECK(wire_size(MsgType::OrderReplace) == 35);
    CHECK(wire_size(MsgType::Trade) == 44);

    // Packed structs hold the 6-byte timestamp in a uint64_t, so every struct
    // is exactly two bytes larger than its wire form. This is the runtime twin
    // of the static_asserts in messages.hpp.
    CHECK(sizeof(SystemEvent) == SystemEvent::kWireSize + kTimestampPadding);
    CHECK(sizeof(StockDirectory) == StockDirectory::kWireSize + kTimestampPadding);
    CHECK(sizeof(AddOrder) == AddOrder::kWireSize + kTimestampPadding);
    CHECK(sizeof(AddOrderMpid) == AddOrderMpid::kWireSize + kTimestampPadding);
    CHECK(sizeof(OrderExecuted) == OrderExecuted::kWireSize + kTimestampPadding);
    CHECK(sizeof(OrderExecutedWithPrice) == OrderExecutedWithPrice::kWireSize + kTimestampPadding);
    CHECK(sizeof(OrderCancel) == OrderCancel::kWireSize + kTimestampPadding);
    CHECK(sizeof(OrderDelete) == OrderDelete::kWireSize + kTimestampPadding);
    CHECK(sizeof(OrderReplace) == OrderReplace::kWireSize + kTimestampPadding);
    CHECK(sizeof(Trade) == Trade::kWireSize + kTimestampPadding);

    // A type this version does not decode reports 0, which is how the walker
    // knows to skip it by length instead of checking it.
    CHECK(wire_size(to_msg_type('H')) == 0);
    CHECK_FALSE(is_decoded(to_msg_type('I')));
}

TEST_CASE("FR-3: System Event decodes every field", "[decode][FR-2]") {
    const std::uint64_t ts = 0x0A0B0C0D0E0F;
    const Bytes bytes = Bytes{}.u8('S').u16(0x0102).u16(0x0304).ts(ts).u8('Q');

    REQUIRE(bytes.size() == SystemEvent::kWireSize);

    const SystemEvent msg = decode_system_event(bytes.data());
    CHECK(msg.type == 'S');
    CHECK(msg.stock_locate == 0x0102);      // not 0x0201: big-endian
    CHECK(msg.tracking_number == 0x0304);
    CHECK(msg.timestamp == ts);
    CHECK(msg.event_code == 'Q');
}

TEST_CASE("FR-3: Stock Directory decodes every field", "[decode][FR-2]") {
    const std::uint64_t ts = 0x00000102030405;
    const Bytes bytes = Bytes{}
                            .u8('R')
                            .u16(7)
                            .u16(8)
                            .ts(ts)
                            .chars("AAPL", 8)
                            .ch('Q')    // market category
                            .ch('N')    // financial status
                            .u32(100)   // round lot size
                            .ch('N')    // round lots only
                            .ch('C')    // issue classification
                            .chars("  ", 2)  // issue sub-type
                            .ch('P')    // authenticity
                            .ch('N')    // short sale threshold
                            .ch('N')    // IPO flag
                            .ch('1')    // LULD reference price tier
                            .ch('N')    // ETP flag
                            .u32(0)     // ETP leverage factor
                            .ch('N');   // inverse indicator

    REQUIRE(bytes.size() == StockDirectory::kWireSize);

    const StockDirectory msg = decode_stock_directory(bytes.data());
    CHECK(msg.type == 'R');
    CHECK(msg.stock_locate == 7);
    CHECK(msg.tracking_number == 8);
    CHECK(msg.timestamp == ts);
    CHECK(msg.stock == sym8("AAPL"));
    CHECK(symbol_view(msg.stock) == "AAPL");
    CHECK(msg.market_category == 'Q');
    CHECK(msg.financial_status_indicator == 'N');
    CHECK(msg.round_lot_size == 100);
    CHECK(msg.round_lots_only == 'N');
    CHECK(msg.issue_classification == 'C');
    CHECK(msg.issue_subtype == std::array<char, 2>{' ', ' '});
    CHECK(msg.authenticity == 'P');
    CHECK(msg.short_sale_threshold_indicator == 'N');
    CHECK(msg.ipo_indicator == 'N');
    CHECK(msg.luld_reference_price_tier == '1');
    CHECK(msg.etp_flag == 'N');
    CHECK(msg.etp_leverage_factor == 0);
    CHECK(msg.inverse_indicator == 'N');
}

TEST_CASE("FR-2: Add Order decodes every field", "[decode][FR-2]") {
    const Bytes bytes = Bytes{}
                            .u8('A')
                            .u16(3)
                            .u16(4)
                            .ts(0x0000FFFFFFFFFFFF)
                            .u64(0x1122334455667788ULL)
                            .ch('B')
                            .u32(300)
                            .chars("MSFT", 8)
                            .u32(123456);  // 12.3456

    REQUIRE(bytes.size() == AddOrder::kWireSize);

    const AddOrder msg = decode_add_order(bytes.data());
    CHECK(msg.type == 'A');
    CHECK(msg.stock_locate == 3);
    CHECK(msg.tracking_number == 4);
    CHECK(msg.timestamp == 0x0000FFFFFFFFFFFFULL);  // high bit set, still 6 bytes
    CHECK(msg.order_reference_number == 0x1122334455667788ULL);
    CHECK(msg.buy_sell_indicator == 'B');
    CHECK(msg.shares == 300);
    CHECK(msg.stock == sym8("MSFT"));
    CHECK(msg.price == 123456);
}

TEST_CASE("FR-2: Add Order with MPID decodes the attribution", "[decode][FR-2]") {
    const Bytes bytes = Bytes{}
                            .u8('F')
                            .u16(3)
                            .u16(4)
                            .ts(1)
                            .u64(0x1122334455667788ULL)
                            .ch('S')
                            .u32(300)
                            .chars("MSFT", 8)
                            .u32(123456)
                            .chars("NSDQ", 4);

    REQUIRE(bytes.size() == AddOrderMpid::kWireSize);

    const AddOrderMpid msg = decode_add_order_mpid(bytes.data());
    CHECK(msg.type == 'F');
    CHECK(msg.buy_sell_indicator == 'S');
    CHECK(msg.shares == 300);
    CHECK(msg.stock == sym8("MSFT"));
    CHECK(msg.price == 123456);
    CHECK(msg.attribution == std::array<char, 4>{'N', 'S', 'D', 'Q'});
}

TEST_CASE("FR-2: Order Executed reduces a resting order", "[decode][FR-2]") {
    const Bytes bytes = Bytes{}
                            .u8('E')
                            .u16(3)
                            .u16(4)
                            .ts(2)
                            .u64(0x1122334455667788ULL)
                            .u32(100)
                            .u64(0x0102030405060708ULL);

    REQUIRE(bytes.size() == OrderExecuted::kWireSize);

    const OrderExecuted msg = decode_order_executed(bytes.data());
    CHECK(msg.type == 'E');
    CHECK(msg.order_reference_number == 0x1122334455667788ULL);
    CHECK(msg.executed_shares == 100);
    CHECK(msg.match_number == 0x0102030405060708ULL);
}

TEST_CASE("FR-2: Order Executed with Price decodes printable flag and price", "[decode][FR-2]") {
    const Bytes bytes = Bytes{}
                            .u8('C')
                            .u16(3)
                            .u16(4)
                            .ts(3)
                            .u64(0x1122334455667788ULL)
                            .u32(100)
                            .u64(0x0102030405060708ULL)
                            .ch('Y')
                            .u32(123400);

    REQUIRE(bytes.size() == OrderExecutedWithPrice::kWireSize);

    const OrderExecutedWithPrice msg = decode_order_executed_with_price(bytes.data());
    CHECK(msg.type == 'C');
    CHECK(msg.executed_shares == 100);
    CHECK(msg.match_number == 0x0102030405060708ULL);
    CHECK(msg.printable == 'Y');
    CHECK(msg.execution_price == 123400);
}

TEST_CASE("FR-2: Order Cancel decodes the cancelled share count", "[decode][FR-2]") {
    const Bytes bytes =
        Bytes{}.u8('X').u16(3).u16(4).ts(4).u64(0x1122334455667788ULL).u32(50);

    REQUIRE(bytes.size() == OrderCancel::kWireSize);

    const OrderCancel msg = decode_order_cancel(bytes.data());
    CHECK(msg.type == 'X');
    CHECK(msg.order_reference_number == 0x1122334455667788ULL);
    CHECK(msg.canceled_shares == 50);
}

TEST_CASE("FR-2: Order Delete decodes the order reference", "[decode][FR-2]") {
    const Bytes bytes = Bytes{}.u8('D').u16(3).u16(4).ts(5).u64(0x1122334455667788ULL);

    REQUIRE(bytes.size() == OrderDelete::kWireSize);

    const OrderDelete msg = decode_order_delete(bytes.data());
    CHECK(msg.type == 'D');
    CHECK(msg.order_reference_number == 0x1122334455667788ULL);
    CHECK(msg.timestamp == 5);
}

TEST_CASE("FR-2: Order Replace decodes both references", "[decode][FR-2]") {
    const Bytes bytes = Bytes{}
                            .u8('U')
                            .u16(3)
                            .u16(4)
                            .ts(6)
                            .u64(0xAAAAAAAAAAAAAAAAULL)
                            .u64(0xBBBBBBBBBBBBBBBBULL)
                            .u32(250)
                            .u32(99999);

    REQUIRE(bytes.size() == OrderReplace::kWireSize);

    const OrderReplace msg = decode_order_replace(bytes.data());
    CHECK(msg.type == 'U');
    CHECK(msg.original_order_reference_number == 0xAAAAAAAAAAAAAAAAULL);
    CHECK(msg.new_order_reference_number == 0xBBBBBBBBBBBBBBBBULL);
    CHECK(msg.shares == 250);
    CHECK(msg.price == 99999);
}

TEST_CASE("FR-2: Trade decodes every field", "[decode][FR-2]") {
    const Bytes bytes = Bytes{}
                            .u8('P')
                            .u16(9)
                            .u16(10)
                            .ts(7)
                            .u64(0x1122334455667788ULL)
                            .ch('S')
                            .u32(100)
                            .chars("QQQQ", 8)
                            .u32(1500000)
                            .u64(0xDEADBEEFULL);

    REQUIRE(bytes.size() == Trade::kWireSize);

    const Trade msg = decode_trade(bytes.data());
    CHECK(msg.type == 'P');
    CHECK(msg.stock_locate == 9);
    CHECK(msg.buy_sell_indicator == 'S');
    CHECK(msg.shares == 100);
    CHECK(msg.stock == sym8("QQQQ"));
    CHECK(msg.price == 1500000);
    CHECK(msg.match_number == 0xDEADBEEFULL);
}

TEST_CASE("every decoded type carries its timestamp at byte offset 5", "[decode][FR-3]") {
    // The walker reads timestamps straight out of the buffer at a fixed offset
    // for every type, decoded or not. That shortcut is only valid because all
    // ITCH messages start type(1) locate(2) tracking(2) timestamp(6).
    const std::uint64_t ts = 0x0A0B0C0D0E0F;

    const std::vector<Bytes> samples = {
        Bytes{}.u8('S').u16(1).u16(2).ts(ts).ch('Q'),
        Bytes{}.u8('R').u16(1).u16(2).ts(ts).chars("AAPL", 8).ch('Q').ch('N').u32(100).ch('N').ch('C')
            .chars("  ", 2).ch('P').ch('N').ch('N').ch('1').ch('N').u32(0).ch('N'),
        Bytes{}.u8('A').u16(1).u16(2).ts(ts).u64(1).ch('B').u32(10).chars("AAPL", 8).u32(1),
        Bytes{}.u8('F').u16(1).u16(2).ts(ts).u64(1).ch('B').u32(10).chars("AAPL", 8).u32(1)
            .chars("NSDQ", 4),
        Bytes{}.u8('E').u16(1).u16(2).ts(ts).u64(1).u32(10).u64(2),
        Bytes{}.u8('C').u16(1).u16(2).ts(ts).u64(1).u32(10).u64(2).ch('Y').u32(1),
        Bytes{}.u8('X').u16(1).u16(2).ts(ts).u64(1).u32(5),
        Bytes{}.u8('D').u16(1).u16(2).ts(ts).u64(1),
        Bytes{}.u8('U').u16(1).u16(2).ts(ts).u64(1).u64(2).u32(10).u32(1),
        Bytes{}.u8('P').u16(1).u16(2).ts(ts).u64(1).ch('B').u32(10).chars("AAPL", 8).u32(1).u64(2),
    };

    REQUIRE(samples.size() == kDecodedTypeCount);

    for (const Bytes& sample : samples) {
        const MsgType type = to_msg_type(static_cast<std::uint8_t>(sample.data()[0]));
        CHECK(is_decoded(type));
        CHECK(wire_size(type) == sample.size());
        CHECK(load_be48(sample.data() + 5) == ts);

        const std::optional<Payload> payload = decode_payload(type, sample.data(),
                                                             static_cast<std::uint16_t>(sample.size()));
        REQUIRE(payload.has_value());
        CHECK(payload_timestamp(*payload) == ts);
    }
}

TEST_CASE("decode_payload validates the length before touching the payload", "[decode][FR-10]") {
    const Bytes bytes = Bytes{}
                            .u8('A')
                            .u16(3)
                            .u16(4)
                            .ts(1)
                            .u64(0x1122334455667788ULL)
                            .ch('B')
                            .u32(300)
                            .chars("MSFT", 8)
                            .u32(123456);

    // Exact length decodes.
    CHECK(decode_payload(MsgType::AddOrder, bytes.data(), 36).has_value());

    // One byte short or long is refused rather than read past the end.
    CHECK_FALSE(decode_payload(MsgType::AddOrder, bytes.data(), 35).has_value());
    CHECK_FALSE(decode_payload(MsgType::AddOrder, bytes.data(), 37).has_value());

    // Skipped types never decode, whatever their length.
    CHECK_FALSE(decode_payload(to_msg_type('H'), bytes.data(), 25).has_value());
    CHECK_FALSE(decode_payload(to_msg_type('W'), bytes.data(), 12).has_value());
}

TEST_CASE("type names and printable type bytes", "[decode]") {
    CHECK(std::string(printable_type(MsgType::AddOrder)) == "A");
    CHECK(std::string(printable_type(to_msg_type(0x00))) == "?");
    CHECK(std::string(printable_type(to_msg_type(0x9B))) == "?");

    CHECK(std::string(type_name(MsgType::OrderReplace)) == "Order Replace");
    CHECK(type_name(to_msg_type('H')) == nullptr);
}

TEST_CASE("symbol_view trims padding without copying", "[decode]") {
    CHECK(symbol_view(sym8("IBM")) == "IBM");
    CHECK(symbol_view(sym8("ABCDEFGH")) == "ABCDEFGH");
    CHECK(symbol_view(std::array<char, 8>{}) == "");
}
