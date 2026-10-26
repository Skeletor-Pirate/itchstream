#pragma once

// One symbol, matched against a feed as it streams past.
//
// ITCH only names the stock on four message types (R, A, F, P). The other six
// decoded types — and every type this version skips — carry only the 16-bit
// stock locate, which sits at byte offset 1 in every ITCH 5.0 message. So the
// filter learns the target's locate from the first message that names it and
// then matches on the locate. That is what lets an order's executions, cancels
// and deletes follow it, and what lets skipped types (I, H, L, Y) travel with
// the symbol in a dump.
//
// Both `dump` and `replay --symbol` use this filter, so there is exactly one
// definition of "belongs to this symbol".

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "feed.hpp"

namespace itchstream {

// The stock locate sits at byte offset 1 in every ITCH 5.0 message.
inline constexpr std::size_t kLocateWireOffset = 1;

// True for a symbol the exchange could print: 1..8 characters of letters,
// digits and the punctuation ITCH uses ('.', '-', '$'), no spaces.
[[nodiscard]] bool is_valid_symbol(std::string_view symbol) noexcept;

// Uppercase A-Z. Exchange symbols are uppercase by convention, so a user typing
// "aapl" must get the same result as one typing "AAPL".
[[nodiscard]] std::string normalize_symbol(std::string_view symbol);

class SymbolFilter {
public:
    explicit SymbolFilter(std::string_view symbol);

    // True when the message belongs to the symbol. Learns the locate from the
    // first matching message and remembers that the symbol was seen at all.
    [[nodiscard]] bool matches(const RawMessage& message) noexcept;

    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] std::optional<std::uint16_t> locate() const noexcept { return locate_; }
    [[nodiscard]] bool found() const noexcept { return found_; }

private:
    std::string symbol_;
    std::optional<std::uint16_t> locate_;
    bool found_ = false;
};

}  // namespace itchstream
