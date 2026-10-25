#pragma once

// Reading the file and walking its messages.
//
// The file is mapped once, read front to back, and never copied: `RawMessage`
// is a (type, length, pointer) view into the mapping. Mapped pages are the
// kernel's page cache, so a second run over the same file does not touch disk,
// which is the "warm" condition every published number is measured under.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "endian.hpp"
#include "messages.hpp"

namespace itchstream {

enum class FeedError : std::uint8_t {
    None,
    OpenFailed,   // no such file, permission denied
    SizeFailed,   // fstat / GetFileSizeEx failed
    MapFailed,    // mmap / CreateFileMapping failed
};

[[nodiscard]] const char* to_string(FeedError error) noexcept;

// A read-only mapping of a whole file.
//
// On Linux this is mmap(MAP_PRIVATE, PROT_READ); on Windows it is
// CreateFileMapping/MapViewOfFile. The Windows side exists so the project
// builds and tests on the machine it is written on. It is not tuned, and no
// published benchmark should come from it.
class MappedFile {
public:
    MappedFile() noexcept = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Returns false and fills `error` on failure. Empty files open successfully
    // with size() == 0 and data() == nullptr.
    bool open(const std::string& path, std::string& error);
    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return is_open_; }
    [[nodiscard]] const std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    void* base_ = nullptr;  // mapping base address
    std::size_t size_ = 0;
    const std::byte* data_ = nullptr;
    bool is_open_ = false;
    std::string path_;

#if defined(_WIN32)
    void* file_handle_ = nullptr;     // HANDLE
    void* mapping_handle_ = nullptr;  // HANDLE
#endif
};

// One message, as a view into the mapping. No copy, no decode.
struct RawMessage {
    MsgType type;
    std::uint16_t length;      // value of the 2-byte length prefix
    const std::byte* payload;  // first byte is the type byte

    // Every ITCH 5.0 message begins type(1) locate(2) tracking(2) timestamp(6),
    // so the timestamp starts at offset 5 for every type, decoded or skipped.
    [[nodiscard]] std::optional<std::uint64_t> timestamp_if_present() const noexcept {
        if (length < 11) return std::nullopt;
        return load_be48(payload + 5);
    }
};

enum class WalkAction : std::uint8_t { Continue, Stop };

enum class WalkError : std::uint8_t {
    None,
    TruncatedHeader,   // fewer than 2 bytes remain: no room for a length prefix
    TruncatedMessage,  // length prefix promises more bytes than the file holds
    ZeroLength,        // length prefix is zero
    LengthTooLarge,    // length prefix beyond any legitimate ITCH 5.0 message
    LengthMismatch,    // known type whose length disagrees with the specification
};

[[nodiscard]] const char* to_string(WalkError error) noexcept;

struct WalkResult {
    std::uint64_t messages = 0;       // messages handed to the visitor, in full
    std::uint64_t bytes_consumed = 0; // offset just past the last complete message
    WalkError error = WalkError::None;
    std::uint64_t error_offset = 0;   // file offset of the offending length prefix
    MsgType error_type = MsgType{0};
    std::uint16_t expected_length = 0;
    std::uint16_t observed_length = 0;
    bool stopped_early = false;

    [[nodiscard]] bool ok() const noexcept { return error == WalkError::None; }
};

// Human-readable, one line, including offsets. Used by the CLI and tests.
[[nodiscard]] std::string describe(const WalkResult& result);

// Walk every length-prefixed message in [data, data + size).
//
// A message is only handed to the visitor once its length prefix has been
// checked against the bytes remaining and, for a decoded type, against the
// specification. The visitor can stop early by returning WalkAction::Stop,
// which is not an error (used by tests and by "first N messages" cutoffs).
//
// Untrusted input is the point: a truncated tail, a zero length or a length
// that disagrees with the spec ends the walk with an error instead of walking
// off the end of the mapping.
template <class Visitor>
[[nodiscard]] WalkResult walk(const std::byte* data, std::size_t size, Visitor&& visitor) {
    WalkResult result{};
    std::size_t offset = 0;

    while (offset < size) {
        const std::size_t remaining = size - offset;
        if (remaining < 2) {
            result.error = WalkError::TruncatedHeader;
            result.error_offset = offset;
            return result;
        }

        const std::uint16_t length = load_be16(data + offset);

        if (length == 0) {
            result.error = WalkError::ZeroLength;
            result.error_offset = offset;
            return result;
        }

        const std::size_t body_available = remaining - 2;
        if (length > body_available) {
            result.error = WalkError::TruncatedMessage;
            result.error_offset = offset;
            result.observed_length = length;
            return result;
        }

        if (length > kMaxPlausibleMessageLength) {
            result.error = WalkError::LengthTooLarge;
            result.error_offset = offset;
            result.observed_length = length;
            return result;
        }

        const std::byte* body = data + offset + 2;
        const MsgType type = to_msg_type(static_cast<std::uint8_t>(body[0]));
        const std::uint16_t expected = wire_size(type);

        if (expected != 0 && expected != length) {
            result.error = WalkError::LengthMismatch;
            result.error_offset = offset;
            result.error_type = type;
            result.expected_length = expected;
            result.observed_length = length;
            return result;
        }

        if (visitor(RawMessage{type, length, body}) == WalkAction::Stop) {
            result.stopped_early = true;
            return result;
        }

        ++result.messages;
        offset += 2 + length;
        result.bytes_consumed = offset;
    }

    return result;
}

}  // namespace itchstream
