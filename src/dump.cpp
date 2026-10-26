#include "itchstream/dump.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "itchstream/decoder.hpp"
#include "itchstream/stats.hpp"

namespace itchstream {
namespace {

// Wire offsets, built from the wire widths rather than offsetof(): the structs
// hold the 6-byte timestamp in a uint64_t, so every field after it sits two
// bytes later in memory than on the wire.
constexpr std::size_t kLocateOffset = 1;  // every ITCH 5.0 message
constexpr std::size_t kSymbolOffsetDirectory = 1 + 2 + 2 + kTimestampWireBytes;  // 11
constexpr std::size_t kSymbolOffsetOrder = kSymbolOffsetDirectory + 8 + 1 + 4;   // 24

static_assert(kSymbolOffsetDirectory + 8 <= StockDirectory::kWireSize);
static_assert(kSymbolOffsetOrder + 8 <= AddOrder::kWireSize);
static_assert(kSymbolOffsetOrder + 8 <= AddOrderMpid::kWireSize);
static_assert(kSymbolOffsetOrder + 8 <= Trade::kWireSize);

// Text field as the exchange pads it: spaces and NULs on the right are padding,
// not part of the value. The returned view points into the mapping, not a copy.
std::string_view trim_field(const char* data, std::size_t size) noexcept {
    while (size > 0 && (data[size - 1] == ' ' || data[size - 1] == '\0')) --size;
    return std::string_view(data, size);
}

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
    return trim_field(reinterpret_cast<const char*>(message.payload + offset), 8);
}

// One text line, formatted in place. Fixed capacity: the widest line is about
// 120 bytes, and a line that did not fit fails the dump rather than being
// silently cut in half.
class TextLine {
public:
    TextLine() noexcept { buffer_[0] = '\0'; }

    template <class... Args>
    void append(const char* format, Args... args) noexcept {
        if (truncated_) return;
        const std::size_t remaining = sizeof buffer_ - size_;

        if constexpr (sizeof...(Args) == 0) {
            // No arguments to format: copy the text. Passing it to snprintf as a
            // bare format string is what -Wformat-security exists to forbid.
            const std::size_t length = std::strlen(format);
            if (length >= remaining) {
                size_ = sizeof buffer_ - 1;
                truncated_ = true;
                return;
            }
            std::memcpy(buffer_ + size_, format, length);
            size_ += length;
            buffer_[size_] = '\0';
            return;
        } else {
            const int written = std::snprintf(buffer_ + size_, remaining, format, args...);
            if (written < 0) {
                truncated_ = true;
                return;
            }
            const auto length = static_cast<std::size_t>(written);
            if (length >= remaining) {
                size_ = sizeof buffer_ - 1;
                truncated_ = true;
                return;
            }
            size_ += length;
        }
    }

    [[nodiscard]] const char* data() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool truncated() const noexcept { return truncated_; }

private:
    char buffer_[256];
    std::size_t size_ = 0;
    bool truncated_ = false;
};

void append_price(TextLine& line, Price4 price) noexcept {
    line.append(" price=%u.%04u", price / 10'000u, price % 10'000u);
}

void append_side(TextLine& line, char buy_sell_indicator) noexcept {
    line.append(" side=%c", buy_sell_indicator);
}

void render(const SystemEvent& message, TextLine& line) noexcept {
    line.append(" event=%c", message.event_code);
}

void render(const StockDirectory& message, TextLine& line) noexcept {
    const std::string_view symbol = symbol_view(message.stock);
    line.append(" symbol=%.*s mkt=%c lot=%u", static_cast<int>(symbol.size()), symbol.data(),
                message.market_category, message.round_lot_size);
}

void render(const AddOrder& message, TextLine& line) noexcept {
    const std::string_view symbol = symbol_view(message.stock);
    line.append(" ref=%llu", static_cast<unsigned long long>(message.order_reference_number));
    append_side(line, message.buy_sell_indicator);
    line.append(" shares=%u", message.shares);
    append_price(line, message.price);
    line.append(" symbol=%.*s", static_cast<int>(symbol.size()), symbol.data());
}

void render(const AddOrderMpid& message, TextLine& line) noexcept {
    const std::string_view symbol = symbol_view(message.stock);
    const std::string_view attribution = trim_field(message.attribution.data(), message.attribution.size());
    line.append(" ref=%llu", static_cast<unsigned long long>(message.order_reference_number));
    append_side(line, message.buy_sell_indicator);
    line.append(" shares=%u", message.shares);
    append_price(line, message.price);
    line.append(" symbol=%.*s mpid=%.*s", static_cast<int>(symbol.size()), symbol.data(),
                static_cast<int>(attribution.size()), attribution.data());
}

void render(const OrderExecuted& message, TextLine& line) noexcept {
    line.append(" ref=%llu shares=%u match=%llu",
                static_cast<unsigned long long>(message.order_reference_number), message.executed_shares,
                static_cast<unsigned long long>(message.match_number));
}

void render(const OrderExecutedWithPrice& message, TextLine& line) noexcept {
    line.append(" ref=%llu shares=%u match=%llu printable=%c",
                static_cast<unsigned long long>(message.order_reference_number), message.executed_shares,
                static_cast<unsigned long long>(message.match_number), message.printable);
    append_price(line, message.execution_price);
}

void render(const OrderCancel& message, TextLine& line) noexcept {
    line.append(" ref=%llu shares=%u",
                static_cast<unsigned long long>(message.order_reference_number), message.canceled_shares);
}

void render(const OrderDelete& message, TextLine& line) noexcept {
    line.append(" ref=%llu", static_cast<unsigned long long>(message.order_reference_number));
}

void render(const OrderReplace& message, TextLine& line) noexcept {
    line.append(" orig=%llu ref=%llu shares=%u",
                static_cast<unsigned long long>(message.original_order_reference_number),
                static_cast<unsigned long long>(message.new_order_reference_number), message.shares);
    append_price(line, message.price);
}

void render(const Trade& message, TextLine& line) noexcept {
    const std::string_view symbol = symbol_view(message.stock);
    line.append(" ref=%llu", static_cast<unsigned long long>(message.order_reference_number));
    append_side(line, message.buy_sell_indicator);
    line.append(" shares=%u", message.shares);
    append_price(line, message.price);
    line.append(" match=%llu symbol=%.*s", static_cast<unsigned long long>(message.match_number),
                static_cast<int>(symbol.size()), symbol.data());
}

void render_payload(const Payload& payload, TextLine& line) noexcept {
    std::visit([&line](const auto& message) { render(message, line); }, payload);
}

void append_prefix(const RawMessage& message, TextLine& line) noexcept {
    char timestamp[24];
    const std::optional<std::uint64_t> value = message.timestamp_if_present();
    if (value.has_value() && format_timestamp(*value, timestamp, sizeof timestamp) > 0) {
        line.append("%s", timestamp);
    } else {
        line.append("--:--:--.---------");
    }

    const std::string_view type = printable_type(message.type);
    const std::uint16_t locate = load_be16(message.payload + kLocateOffset);
    line.append("  %.*s %5u", static_cast<int>(type.size()), type.data(), locate);
}

bool write_message(const RawMessage& message, bool text, DumpSink& sink, DumpResult& result) {
    if (text) {
        TextLine line;
        append_prefix(message, line);

        const std::optional<Payload> payload =
            is_decoded(message.type)
                ? decode_payload(message.type, message.payload, message.length)
                : std::nullopt;
        if (payload.has_value()) {
            render_payload(*payload, line);
        } else if (is_decoded(message.type)) {
            // The walker rejected a length disagreement already, so this is
            // defensive: say so instead of emitting a half-rendered line.
            line.append(" undecodable=%uB", static_cast<unsigned>(message.length));
        } else {
            line.append(" raw=%uB", static_cast<unsigned>(message.length));
        }
        line.append("\n");

        if (line.truncated()) {
            result.error = "a text line did not fit the format buffer";
            return false;
        }
        if (!sink.write(line.data(), line.size())) {
            result.error = "the output rejected a write";
            return false;
        }
        result.bytes_written += line.size();
    } else {
        // The length prefix is rebuilt from the validated length and the payload
        // is copied verbatim, so the output is the input's exact message bytes.
        const std::byte header[2] = {std::byte{static_cast<std::uint8_t>(message.length >> 8)},
                                     std::byte{static_cast<std::uint8_t>(message.length & 0xFF)}};
        if (!sink.write(header, sizeof header) || !sink.write(message.payload, message.length)) {
            result.error = "the output rejected a write";
            return false;
        }
        result.bytes_written += sizeof header + message.length;
    }

    ++result.messages;
    ++result.by_type[static_cast<std::uint8_t>(message.type)];
    return true;
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

DumpResult dump_symbol(const std::byte* data, std::size_t size, const DumpOptions& options,
                       DumpSink& sink) {
    DumpResult result{};
    const std::string target = normalize_symbol(options.symbol);
    std::optional<std::uint16_t> locate;

    result.walk = walk(data, size, [&](const RawMessage& message) {
        // Every ITCH 5.0 message has the locate at offset 1, but the walker
        // admits an unknown type with a 1-byte body. Nothing to match there.
        if (message.length < kLocateOffset + 2) return WalkAction::Continue;

        const std::uint16_t message_locate = load_be16(message.payload + kLocateOffset);
        bool matched = false;

        // Symbol compare first: it also works before the directory entry has
        // been seen, for the four types that carry the symbol themselves.
        if (const std::string_view symbol = embedded_symbol(message); !symbol.empty()) {
            if (symbol == target) {
                result.symbol_found = true;
                if (!locate.has_value()) locate = message_locate;
                matched = true;
            }
        }

        if (!matched && locate.has_value() && message_locate == *locate) matched = true;
        if (!matched) return WalkAction::Continue;

        if (!write_message(message, options.text, sink, result)) {
            result.write_failed = true;
            return WalkAction::Stop;
        }
        return WalkAction::Continue;
    });

    result.locate = locate;
    return result;
}

}  // namespace itchstream
