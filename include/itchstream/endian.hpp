#pragma once

// Big-endian loads for NASDAQ TotalView-ITCH 5.0.
//
// Every multi-byte field on the wire is big-endian. Decoding is therefore a
// plain load followed by a byte swap: on a little-endian host `load_be32` is a
// `mov` plus a `bswap`, no branch, no copy of the message.
//
// Nothing here casts a pointer into the mapped file to a struct. An unaligned
// struct read is undefined behaviour and UBSan flags it; `memcpy` of a known
// width is not, and the compiler turns it into a single load anyway.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchstream {

// The loads below swap unconditionally, which is only correct on a
// little-endian host. Porting to a big-endian target means removing the swap,
// so fail loudly here instead of producing silently byte-reversed prices.
static_assert(std::endian::native == std::endian::little,
              "ItchStream assumes a little-endian host: the loads in this header swap unconditionally");

namespace detail {

#if defined(_MSC_VER) && !defined(__clang__)
inline std::uint16_t bswap16(std::uint16_t v) noexcept { return _byteswap_ushort(v); }
inline std::uint32_t bswap32(std::uint32_t v) noexcept { return _byteswap_ulong(v); }
inline std::uint64_t bswap64(std::uint64_t v) noexcept { return _byteswap_uint64(v); }
#else
inline std::uint16_t bswap16(std::uint16_t v) noexcept { return __builtin_bswap16(v); }
inline std::uint32_t bswap32(std::uint32_t v) noexcept { return __builtin_bswap32(v); }
inline std::uint64_t bswap64(std::uint64_t v) noexcept { return __builtin_bswap64(v); }
#endif

}  // namespace detail

[[nodiscard]] inline std::uint16_t load_be16(const std::byte* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return detail::bswap16(v);
}

[[nodiscard]] inline std::uint32_t load_be32(const std::byte* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return detail::bswap32(v);
}

[[nodiscard]] inline std::uint64_t load_be64(const std::byte* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return detail::bswap64(v);
}

// ITCH timestamps are 6 bytes: nanoseconds since midnight. Reading 8 bytes and
// shifting is two instructions more than reading 6, and reading 8 would run off
// the end of the final message in a file, so read exactly 6 into a zeroed
// 64-bit register and swap that.
[[nodiscard]] inline std::uint64_t load_be48(const std::byte* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 6);
    return detail::bswap64(v) >> 16;
}

// Sequential reader used by the decoder.
//
// Field order in each decode function then mirrors field order in the spec, so
// an offset mistake is not expressible: there are no offsets to write down.
// Each call consumes exactly the width of the field it returns.
class Reader {
public:
    explicit Reader(const std::byte* p) noexcept : start_(p), p_(p) {}

    [[nodiscard]] std::uint8_t u8() noexcept {
        const std::uint8_t v = static_cast<std::uint8_t>(*p_);
        p_ += 1;
        return v;
    }

    [[nodiscard]] std::uint16_t u16() noexcept {
        const std::uint16_t v = load_be16(p_);
        p_ += 2;
        return v;
    }

    [[nodiscard]] std::uint32_t u32() noexcept {
        const std::uint32_t v = load_be32(p_);
        p_ += 4;
        return v;
    }

    [[nodiscard]] std::uint64_t u64() noexcept {
        const std::uint64_t v = load_be64(p_);
        p_ += 8;
        return v;
    }

    // 6-byte nanosecond timestamp.
    [[nodiscard]] std::uint64_t timestamp() noexcept {
        const std::uint64_t v = load_be48(p_);
        p_ += 6;
        return v;
    }

    template <std::size_t N>
    [[nodiscard]] std::array<char, N> chars() noexcept {
        std::array<char, N> out{};
        std::memcpy(out.data(), p_, N);
        p_ += N;
        return out;
    }

    [[nodiscard]] char ch() noexcept { return static_cast<char>(u8()); }

    // Bytes consumed so far. Tests assert this equals the message's wire size,
    // which is how a missing or duplicated field in a decode function is caught.
    [[nodiscard]] std::size_t offset() const noexcept {
        return static_cast<std::size_t>(p_ - start_);
    }

private:
    const std::byte* start_;
    const std::byte* p_;
};

}  // namespace itchstream
