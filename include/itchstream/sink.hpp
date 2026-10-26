#pragma once

// Where re-emitted messages go, and the one definition of byte-exact copying.
//
// The interface exists so tests can capture bytes instead of touching the file
// system, and so the CLI owns the file handle (and its error reporting). It is
// deliberately one virtual call per written message: matched messages are the
// few percent of a feed that a dump or a symbol replay emits, and the walk that
// finds them stays allocation-free.

#include <cstddef>
#include <cstdint>

#include "feed.hpp"

namespace itchstream {

class MessageSink {
public:
    MessageSink() = default;
    virtual ~MessageSink() = default;

    MessageSink(const MessageSink&) = delete;
    MessageSink& operator=(const MessageSink&) = delete;

    // Returns false on a write failure. The caller stops and reports it.
    [[nodiscard]] virtual bool write(const void* data, std::size_t size) = 0;

    // Called when the consumer needs to see what was written so far, which is
    // what pacing means. Default: nothing to do for a sink that keeps memory.
    [[nodiscard]] virtual bool flush() { return true; }
};

// Writes the validated 2-byte length prefix and then the message body verbatim,
// so the output is the input's exact message bytes and never a re-encoding.
[[nodiscard]] inline bool write_framed(MessageSink& sink, const RawMessage& message) {
    const std::byte header[2] = {std::byte{static_cast<std::uint8_t>(message.length >> 8)},
                                 std::byte{static_cast<std::uint8_t>(message.length & 0xFF)}};
    return sink.write(header, sizeof header) && sink.write(message.payload, message.length);
}

}  // namespace itchstream
