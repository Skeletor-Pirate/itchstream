#pragma once

// Decoded representation of the ITCH 5.0 messages ItchStream cares about.
//
// Two rules shape this file:
//
//  1. The structs are packed, so `sizeof` is the sum of the field widths and a
//     static_assert can pin each one to the byte count in the specification.
//     They are value objects filled field by field, never overlaid on the
//     mapped file: no `reinterpret_cast` of wire bytes into these types.
//
//  2. The timestamp is stored as a uint64_t holding nanoseconds since
//     midnight, but it occupies 6 bytes on the wire. Every struct is therefore
//     exactly `kWireSize + 2` bytes in memory. The static_assert under each
//     struct states that relationship explicitly, so adding, dropping or
//     widening a field breaks the build rather than the book.
//
// Prices stay integers with 4 implied decimals (`Price4`), as they arrive.
// No floating point enters the decode path or the book.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace itchstream {

using OrderRef = std::uint64_t;   // 8 bytes on the wire
using Price4 = std::uint32_t;     // 4 implied decimals: 1234567 == 123.4567
using Shares = std::uint32_t;
using Timestamp = std::uint64_t;  // nanoseconds since midnight, 6 bytes on the wire

inline constexpr std::size_t kTimestampWireBytes = 6;
inline constexpr std::size_t kTimestampMemBytes = sizeof(Timestamp);
inline constexpr std::size_t kTimestampPadding = kTimestampMemBytes - kTimestampWireBytes;

// TotalView-ITCH 5.0 message types. The underlying type is uint8_t because the
// file contains type bytes with no enumerator (H, V, W, K, J, L, Y, Q, B, I,
// N, ...); those are carried through as unknown values and skipped by length.
enum class MsgType : std::uint8_t {
    SystemEvent = 'S',
    StockDirectory = 'R',
    AddOrder = 'A',
    AddOrderMpid = 'F',
    OrderExecuted = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
    Trade = 'P',
};

[[nodiscard]] constexpr MsgType to_msg_type(std::uint8_t b) noexcept {
    return static_cast<MsgType>(b);
}

// Largest length prefix seen in a legitimate file is 50 (NOII, type 'I', which
// this version skips). Anything bigger is corruption, and treating it as a
// skip would silently desynchronise the rest of the walk.
inline constexpr std::uint16_t kMaxPlausibleMessageLength = 64;

#pragma pack(push, 1)

// S - System Event. 12 bytes.
struct SystemEvent {
    static constexpr std::size_t kWireSize = 12;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    char event_code;  // O start of messages, S system hours, Q market hours,
                      // M market close, E system hours end, C end of messages
};
static_assert(sizeof(SystemEvent) == SystemEvent::kWireSize + kTimestampPadding,
              "System Event must be 12 bytes on the wire");

// R - Stock Directory. 39 bytes.
struct StockDirectory {
    static constexpr std::size_t kWireSize = 39;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    std::array<char, 8> stock;
    char market_category;
    char financial_status_indicator;
    std::uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    std::array<char, 2> issue_subtype;
    char authenticity;
    char short_sale_threshold_indicator;
    char ipo_indicator;
    char luld_reference_price_tier;
    char etp_flag;
    std::uint32_t etp_leverage_factor;
    char inverse_indicator;
};
static_assert(sizeof(StockDirectory) == StockDirectory::kWireSize + kTimestampPadding,
              "Stock Directory must be 39 bytes on the wire");

// A - Add Order (no MPID attribution). 36 bytes.
struct AddOrder {
    static constexpr std::size_t kWireSize = 36;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef order_reference_number;
    char buy_sell_indicator;  // 'B' bid, 'S' ask
    Shares shares;
    std::array<char, 8> stock;
    Price4 price;
};
static_assert(sizeof(AddOrder) == AddOrder::kWireSize + kTimestampPadding,
              "Add Order must be 36 bytes on the wire");

// F - Add Order with MPID attribution. 40 bytes.
struct AddOrderMpid {
    static constexpr std::size_t kWireSize = 40;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef order_reference_number;
    char buy_sell_indicator;
    Shares shares;
    std::array<char, 8> stock;
    Price4 price;
    std::array<char, 4> attribution;
};
static_assert(sizeof(AddOrderMpid) == AddOrderMpid::kWireSize + kTimestampPadding,
              "Add Order with MPID must be 40 bytes on the wire");

// E - Order Executed. 31 bytes.
struct OrderExecuted {
    static constexpr std::size_t kWireSize = 31;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef order_reference_number;
    Shares executed_shares;
    std::uint64_t match_number;
};
static_assert(sizeof(OrderExecuted) == OrderExecuted::kWireSize + kTimestampPadding,
              "Order Executed must be 31 bytes on the wire");

// C - Order Executed with Price. 36 bytes.
struct OrderExecutedWithPrice {
    static constexpr std::size_t kWireSize = 36;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef order_reference_number;
    Shares executed_shares;
    std::uint64_t match_number;
    char printable;  // 'Y'/'N'
    Price4 execution_price;
};
static_assert(sizeof(OrderExecutedWithPrice) == OrderExecutedWithPrice::kWireSize + kTimestampPadding,
              "Order Executed with Price must be 36 bytes on the wire");

// X - Order Cancel (partial). 23 bytes.
struct OrderCancel {
    static constexpr std::size_t kWireSize = 23;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef order_reference_number;
    Shares canceled_shares;
};
static_assert(sizeof(OrderCancel) == OrderCancel::kWireSize + kTimestampPadding,
              "Order Cancel must be 23 bytes on the wire");

// D - Order Delete. 19 bytes.
struct OrderDelete {
    static constexpr std::size_t kWireSize = 19;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef order_reference_number;
};
static_assert(sizeof(OrderDelete) == OrderDelete::kWireSize + kTimestampPadding,
              "Order Delete must be 19 bytes on the wire");

// U - Order Replace. 35 bytes. The replacement is a new order reference number
// and loses queue priority: the book must treat it as delete-then-add-at-back.
struct OrderReplace {
    static constexpr std::size_t kWireSize = 35;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef original_order_reference_number;
    OrderRef new_order_reference_number;
    Shares shares;
    Price4 price;
};
static_assert(sizeof(OrderReplace) == OrderReplace::kWireSize + kTimestampPadding,
              "Order Replace must be 35 bytes on the wire");

// P - Trade (non-cross). 44 bytes. Statistics only, never modifies the book.
struct Trade {
    static constexpr std::size_t kWireSize = 44;
    std::uint8_t type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    Timestamp timestamp;
    OrderRef order_reference_number;
    char buy_sell_indicator;
    Shares shares;
    std::array<char, 8> stock;
    Price4 price;
    std::uint64_t match_number;
};
static_assert(sizeof(Trade) == Trade::kWireSize + kTimestampPadding,
              "Trade must be 44 bytes on the wire");

#pragma pack(pop)

// The decoder memcpy's these into std::variant, which requires trivial copies.
static_assert(std::is_trivially_copyable_v<SystemEvent>);
static_assert(std::is_trivially_copyable_v<StockDirectory>);
static_assert(std::is_trivially_copyable_v<AddOrder>);
static_assert(std::is_trivially_copyable_v<AddOrderMpid>);
static_assert(std::is_trivially_copyable_v<OrderExecuted>);
static_assert(std::is_trivially_copyable_v<OrderExecutedWithPrice>);
static_assert(std::is_trivially_copyable_v<OrderCancel>);
static_assert(std::is_trivially_copyable_v<OrderDelete>);
static_assert(std::is_trivially_copyable_v<OrderReplace>);
static_assert(std::is_trivially_copyable_v<Trade>);

struct TypeInfo {
    MsgType type;
    std::uint16_t wire_size;
    const char* name;
};

// Single source of truth for "which types do we decode, and how long are they".
inline constexpr TypeInfo kTypeTable[] = {
    {MsgType::SystemEvent, SystemEvent::kWireSize, "System Event"},
    {MsgType::StockDirectory, StockDirectory::kWireSize, "Stock Directory"},
    {MsgType::AddOrder, AddOrder::kWireSize, "Add Order"},
    {MsgType::AddOrderMpid, AddOrderMpid::kWireSize, "Add Order (MPID)"},
    {MsgType::OrderExecuted, OrderExecuted::kWireSize, "Order Executed"},
    {MsgType::OrderExecutedWithPrice, OrderExecutedWithPrice::kWireSize, "Order Executed with Price"},
    {MsgType::OrderCancel, OrderCancel::kWireSize, "Order Cancel"},
    {MsgType::OrderDelete, OrderDelete::kWireSize, "Order Delete"},
    {MsgType::OrderReplace, OrderReplace::kWireSize, "Order Replace"},
    {MsgType::Trade, Trade::kWireSize, "Trade"},
};

inline constexpr std::size_t kDecodedTypeCount = sizeof(kTypeTable) / sizeof(kTypeTable[0]);

// Expected wire size for a decoded type, or 0 for a type this version skips.
// A switch rather than a table lookup: it is on the per-message path, and the
// compiler turns it into a small jump table.
[[nodiscard]] constexpr std::uint16_t wire_size(MsgType type) noexcept {
    switch (type) {
        case MsgType::SystemEvent: return SystemEvent::kWireSize;
        case MsgType::StockDirectory: return StockDirectory::kWireSize;
        case MsgType::AddOrder: return AddOrder::kWireSize;
        case MsgType::AddOrderMpid: return AddOrderMpid::kWireSize;
        case MsgType::OrderExecuted: return OrderExecuted::kWireSize;
        case MsgType::OrderExecutedWithPrice: return OrderExecutedWithPrice::kWireSize;
        case MsgType::OrderCancel: return OrderCancel::kWireSize;
        case MsgType::OrderDelete: return OrderDelete::kWireSize;
        case MsgType::OrderReplace: return OrderReplace::kWireSize;
        case MsgType::Trade: return Trade::kWireSize;
    }
    return 0;
}

[[nodiscard]] constexpr bool is_decoded(MsgType type) noexcept {
    return wire_size(type) != 0;
}

// Long name for decoded types, nullptr for skipped ones.
[[nodiscard]] constexpr const char* type_name(MsgType type) noexcept {
    for (const TypeInfo& info : kTypeTable) {
        if (info.type == type) return info.name;
    }
    return nullptr;
}

// Printable form of any type byte, including skipped ones: 0x00 and other
// control bytes would wreck a terminal table, so they become '?' or '\xNN'.
[[nodiscard]] std::string_view printable_type(MsgType type) noexcept;

// A fixed-width text field as the exchange pads it: trailing spaces and NULs
// are padding, not part of the value. The view points at the caller's bytes.
[[nodiscard]] inline std::string_view trim_padding(const char* data, std::size_t size) noexcept {
    while (size > 0 && (data[size - 1] == ' ' || data[size - 1] == '\0')) --size;
    return std::string_view(data, size);
}

// Symbols are 8 bytes, right-padded with spaces.
[[nodiscard]] std::string_view symbol_view(const std::array<char, 8>& stock) noexcept;

}  // namespace itchstream
