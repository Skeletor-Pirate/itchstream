#pragma once

// Decoders: wire bytes -> plain structs.
//
// Each decoder walks the message with a sequential Reader, so the field order
// in the code is the field order in the specification. `decode_payload` is the
// only entry point a caller needs; it validates the length before touching a
// byte, which is what makes it safe to point at untrusted input.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "messages.hpp"

namespace itchstream {

using Payload = std::variant<SystemEvent,
                             StockDirectory,
                             AddOrder,
                             AddOrderMpid,
                             OrderExecuted,
                             OrderExecutedWithPrice,
                             OrderCancel,
                             OrderDelete,
                             OrderReplace,
                             Trade>;

// Per-type decoders. Precondition: `p` points at `wire_size(type)` readable
// bytes. `decode_payload` is the checked wrapper; these are exposed so tests can
// assert on exact fields per message type.
[[nodiscard]] SystemEvent decode_system_event(const std::byte* p) noexcept;
[[nodiscard]] StockDirectory decode_stock_directory(const std::byte* p) noexcept;
[[nodiscard]] AddOrder decode_add_order(const std::byte* p) noexcept;
[[nodiscard]] AddOrderMpid decode_add_order_mpid(const std::byte* p) noexcept;
[[nodiscard]] OrderExecuted decode_order_executed(const std::byte* p) noexcept;
[[nodiscard]] OrderExecutedWithPrice decode_order_executed_with_price(const std::byte* p) noexcept;
[[nodiscard]] OrderCancel decode_order_cancel(const std::byte* p) noexcept;
[[nodiscard]] OrderDelete decode_order_delete(const std::byte* p) noexcept;
[[nodiscard]] OrderReplace decode_order_replace(const std::byte* p) noexcept;
[[nodiscard]] Trade decode_trade(const std::byte* p) noexcept;

// Decode one message.
//
// Returns nullopt when the type is skipped by this version or when `length`
// does not equal the wire size for that type. Both are "nothing to decode",
// not errors: the walker reports a length disagreement as a hard error before
// a caller gets here.
[[nodiscard]] std::optional<Payload> decode_payload(MsgType type, const std::byte* p,
                                                   std::uint16_t length) noexcept;

}  // namespace itchstream
