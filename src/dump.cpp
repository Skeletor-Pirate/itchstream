#include "itchstream/dump.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "itchstream/decoder.hpp"
#include "itchstream/stats.hpp"
#include "itchstream/symbol_filter.hpp"

namespace itchstream {
namespace {

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
    const std::string_view attribution = trim_padding(message.attribution.data(), message.attribution.size());
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
    const std::uint16_t locate = load_be16(message.payload + kLocateWireOffset);
    line.append("  %.*s %5u", static_cast<int>(type.size()), type.data(), locate);
}

bool write_message(const RawMessage& message, bool text, MessageSink& sink, DumpResult& result) {
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
        if (!write_framed(sink, message)) {
            result.error = "the output rejected a write";
            return false;
        }
        result.bytes_written += 2 + message.length;
    }

    ++result.messages;
    ++result.by_type[static_cast<std::uint8_t>(message.type)];
    return true;
}

}  // namespace

DumpResult dump_symbol(const std::byte* data, std::size_t size, const DumpOptions& options,
                       MessageSink& sink) {
    DumpResult result{};
    SymbolFilter filter(options.symbol);

    result.walk = walk(data, size, [&](const RawMessage& message) {
        if (!filter.matches(message)) return WalkAction::Continue;

        if (!write_message(message, options.text, sink, result)) {
            result.write_failed = true;
            return WalkAction::Stop;
        }
        return WalkAction::Continue;
    });

    result.locate = filter.locate();
    result.symbol_found = filter.found();
    return result;
}

}  // namespace itchstream
