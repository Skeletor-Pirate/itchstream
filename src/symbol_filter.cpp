#include "itchstream/symbol_filter.hpp"

#include <cstddef>
#include "itchstream/endian.hpp"
#include "itchstream/messages.hpp"

namespace itchstream {
namespace {

// Wire offsets, built from the wire widths rather than offsetof(): the structs
// hold the 6-byte timestamp in a uint64_t, so every field after it sits two
// bytes later in memory than on the wire.
constexpr std::size_t kSymbolOffsetDirectory = 1 + 2 + 2 + kTimestampWireBytes;  // 11
constexpr std::size_t kSymbolOffsetOrder = kSymbolOffsetDirectory + 8 + 1 + 4;   // 24

static_assert(kSymbolOffsetDirectory + 8 <= StockDirectory::kWireSize);
static_assert(kSymbolOffsetOrder + 8 <= AddOrder::kWireSize);
static_assert(kSymbolOffsetOrder + 8 <= AddOrderMpid::kWireSize);
static_assert(kSymbolOffsetOrder + 8 <= Trade::kWireSize);

// The symbol, for the four types that carry one; empty for every other type.
std::string_view embedded_symbol(const RawMessage& message) noexcept {
    std::size_t offset = 0;
    switch (message.type) {
        case MsgType::StockDirectory:
            offset = kSymbolOffsetDirectory;
            break;
        case MsgType::AddOrder:
        case MsgType::AddOrderMpid:
        case MsgType::Trade:
            offset = kSymbolOffsetOrder;
            break;
        default:
            return {};
    }
    return trim_padding(reinterpret_cast<const char*>(message.payload + offset), 8);
}

}  // namespace

bool is_valid_symbol(std::string_view symbol) noexcept {
    if (symbol.empty() || symbol.size() > 8) return false;
    for (const char raw : symbol) {
        const auto c = static_cast<unsigned char>(raw);
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        const bool punctuation = c == '.' || c == '-' || c == '$';
        if (!letter && !digit && !punctuation) return false;
    }
    return true;
}

std::string normalize_symbol(std::string_view symbol) {
    std::string out(symbol);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

SymbolFilter::SymbolFilter(std::string_view symbol) : symbol_(normalize_symbol(symbol)) {}

bool SymbolFilter::matches(const RawMessage& message) noexcept {
    // Every ITCH 5.0 message has the locate at offset 1, but the walker admits
    // an unknown type with a 1-byte body. Nothing to match there.
    if (message.length < kLocateWireOffset + 2) return false;

    const std::uint16_t message_locate = load_be16(message.payload + kLocateWireOffset);

    // Symbol compare first: it also works before the directory entry has been
    // seen, for the four types that carry the symbol themselves.
    if (const std::string_view symbol = embedded_symbol(message); !symbol.empty()) {
        if (symbol == symbol_) {
            found_ = true;
            if (!locate_.has_value()) locate_ = message_locate;
            return true;
        }
    }

    return locate_.has_value() && message_locate == *locate_;
}

}  // namespace itchstream
