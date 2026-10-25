#include "itchstream/decoder.hpp"

#include <cassert>

#include "itchstream/endian.hpp"

namespace itchstream {
namespace {

// A decoder that forgets a field, or reads one twice, still compiles: the
// bytes simply shift. Asserting the reader consumed exactly the specification's
// width turns that class of mistake into a debug-build abort, and the unit tests
// catch it in Release.
void check_consumed(const Reader& reader, std::size_t expected) noexcept {
    assert(reader.offset() == expected);
    (void)reader;
    (void)expected;
}

}  // namespace

SystemEvent decode_system_event(const std::byte* p) noexcept {
    Reader r{p};
    SystemEvent m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.event_code = r.ch();
    check_consumed(r, SystemEvent::kWireSize);
    return m;
}

StockDirectory decode_stock_directory(const std::byte* p) noexcept {
    Reader r{p};
    StockDirectory m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.stock = r.chars<8>();
    m.market_category = r.ch();
    m.financial_status_indicator = r.ch();
    m.round_lot_size = r.u32();
    m.round_lots_only = r.ch();
    m.issue_classification = r.ch();
    m.issue_subtype = r.chars<2>();
    m.authenticity = r.ch();
    m.short_sale_threshold_indicator = r.ch();
    m.ipo_indicator = r.ch();
    m.luld_reference_price_tier = r.ch();
    m.etp_flag = r.ch();
    m.etp_leverage_factor = r.u32();
    m.inverse_indicator = r.ch();
    check_consumed(r, StockDirectory::kWireSize);
    return m;
}

AddOrder decode_add_order(const std::byte* p) noexcept {
    Reader r{p};
    AddOrder m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.order_reference_number = r.u64();
    m.buy_sell_indicator = r.ch();
    m.shares = r.u32();
    m.stock = r.chars<8>();
    m.price = r.u32();
    check_consumed(r, AddOrder::kWireSize);
    return m;
}

AddOrderMpid decode_add_order_mpid(const std::byte* p) noexcept {
    Reader r{p};
    AddOrderMpid m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.order_reference_number = r.u64();
    m.buy_sell_indicator = r.ch();
    m.shares = r.u32();
    m.stock = r.chars<8>();
    m.price = r.u32();
    m.attribution = r.chars<4>();
    check_consumed(r, AddOrderMpid::kWireSize);
    return m;
}

OrderExecuted decode_order_executed(const std::byte* p) noexcept {
    Reader r{p};
    OrderExecuted m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.order_reference_number = r.u64();
    m.executed_shares = r.u32();
    m.match_number = r.u64();
    check_consumed(r, OrderExecuted::kWireSize);
    return m;
}

OrderExecutedWithPrice decode_order_executed_with_price(const std::byte* p) noexcept {
    Reader r{p};
    OrderExecutedWithPrice m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.order_reference_number = r.u64();
    m.executed_shares = r.u32();
    m.match_number = r.u64();
    m.printable = r.ch();
    m.execution_price = r.u32();
    check_consumed(r, OrderExecutedWithPrice::kWireSize);
    return m;
}

OrderCancel decode_order_cancel(const std::byte* p) noexcept {
    Reader r{p};
    OrderCancel m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.order_reference_number = r.u64();
    m.canceled_shares = r.u32();
    check_consumed(r, OrderCancel::kWireSize);
    return m;
}

OrderDelete decode_order_delete(const std::byte* p) noexcept {
    Reader r{p};
    OrderDelete m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.order_reference_number = r.u64();
    check_consumed(r, OrderDelete::kWireSize);
    return m;
}

OrderReplace decode_order_replace(const std::byte* p) noexcept {
    Reader r{p};
    OrderReplace m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.original_order_reference_number = r.u64();
    m.new_order_reference_number = r.u64();
    m.shares = r.u32();
    m.price = r.u32();
    check_consumed(r, OrderReplace::kWireSize);
    return m;
}

Trade decode_trade(const std::byte* p) noexcept {
    Reader r{p};
    Trade m{};
    m.type = r.u8();
    m.stock_locate = r.u16();
    m.tracking_number = r.u16();
    m.timestamp = r.timestamp();
    m.order_reference_number = r.u64();
    m.buy_sell_indicator = r.ch();
    m.shares = r.u32();
    m.stock = r.chars<8>();
    m.price = r.u32();
    m.match_number = r.u64();
    check_consumed(r, Trade::kWireSize);
    return m;
}

std::optional<Payload> decode_payload(MsgType type, const std::byte* p,
                                      std::uint16_t length) noexcept {
    // Length is checked here, not by the caller: this is the entry point that
    // may be pointed at untrusted bytes (fuzz target, golden-reference dump).
    const std::uint16_t expected = wire_size(type);
    if (expected == 0 || expected != length) {
        return std::nullopt;
    }

    switch (type) {
        case MsgType::SystemEvent: return Payload{decode_system_event(p)};
        case MsgType::StockDirectory: return Payload{decode_stock_directory(p)};
        case MsgType::AddOrder: return Payload{decode_add_order(p)};
        case MsgType::AddOrderMpid: return Payload{decode_add_order_mpid(p)};
        case MsgType::OrderExecuted: return Payload{decode_order_executed(p)};
        case MsgType::OrderExecutedWithPrice: return Payload{decode_order_executed_with_price(p)};
        case MsgType::OrderCancel: return Payload{decode_order_cancel(p)};
        case MsgType::OrderDelete: return Payload{decode_order_delete(p)};
        case MsgType::OrderReplace: return Payload{decode_order_replace(p)};
        case MsgType::Trade: return Payload{decode_trade(p)};
    }
    return std::nullopt;
}

}  // namespace itchstream
