#include "itchstream/messages.hpp"

namespace itchstream {
namespace {

// A type byte in a corrupt file can be anything from 0x00 to 0xFF. Dumping a
// raw control byte into the stats table wrecks the terminal, so non-printable
// bytes become '?' and printable ASCII stays itself.
struct TypeGlyphs {
    std::array<std::array<char, 2>, 256> cells{};

    constexpr TypeGlyphs() noexcept {
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const bool printable = i >= 0x21 && i <= 0x7e;
            cells[i][0] = printable ? static_cast<char>(i) : '?';
            cells[i][1] = '\0';
        }
    }
};

inline constexpr TypeGlyphs kGlyphs{};

}  // namespace

std::string_view printable_type(MsgType type) noexcept {
    return std::string_view(kGlyphs.cells[static_cast<std::uint8_t>(type)].data(), 1);
}

std::string_view symbol_view(const std::array<char, 8>& stock) noexcept {
    std::size_t length = stock.size();
    while (length > 0 && (stock[length - 1] == ' ' || stock[length - 1] == '\0')) {
        --length;
    }
    return std::string_view(stock.data(), length);
}

}  // namespace itchstream
