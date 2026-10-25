#pragma once

// Test-only byte builder: writes big-endian fields in specification order, so a
// test reads like the message table in the ITCH 5.0 document.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace itchstream_test {

class Bytes {
public:
    Bytes& u8(std::uint8_t value) {
        data_.push_back(std::byte{value});
        return *this;
    }

    Bytes& ch(char value) { return u8(static_cast<std::uint8_t>(value)); }

    Bytes& u16(std::uint16_t value) {
        push_be(value, 2);
        return *this;
    }

    Bytes& u32(std::uint32_t value) {
        push_be(value, 4);
        return *this;
    }

    Bytes& u64(std::uint64_t value) {
        push_be(value, 8);
        return *this;
    }

    // 6-byte nanosecond timestamp.
    Bytes& ts(std::uint64_t value) {
        push_be(value, 6);
        return *this;
    }

    // Fixed-width text field, right-padded with spaces like the exchange does.
    Bytes& chars(std::string_view text, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            const char c = i < text.size() ? text[i] : ' ';
            data_.push_back(std::byte{static_cast<unsigned char>(c)});
        }
        return *this;
    }

    Bytes& zeros(std::size_t count) {
        data_.insert(data_.end(), count, std::byte{0});
        return *this;
    }

    Bytes& append(const Bytes& other) {
        data_.insert(data_.end(), other.data_.begin(), other.data_.end());
        return *this;
    }

    void resize(std::size_t count) { data_.resize(count); }

    [[nodiscard]] const std::byte* data() const { return data_.data(); }
    [[nodiscard]] std::size_t size() const { return data_.size(); }

private:
    void push_be(std::uint64_t value, int width) {
        for (int i = width - 1; i >= 0; --i) {
            data_.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF)});
        }
    }

    std::vector<std::byte> data_;
};

// Prefix a message body with its 2-byte length, as the NASDAQ sample files do.
inline Bytes frame(const Bytes& body) {
    Bytes out;
    out.u16(static_cast<std::uint16_t>(body.size()));
    out.append(body);
    return out;
}

inline std::array<char, 8> sym8(std::string_view text) {
    std::array<char, 8> out{};
    out.fill(' ');
    for (std::size_t i = 0; i < text.size() && i < out.size(); ++i) out[i] = text[i];
    return out;
}

}  // namespace itchstream_test
