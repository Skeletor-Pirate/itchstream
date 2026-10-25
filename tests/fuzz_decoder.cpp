// libFuzzer target for the decoder and the message walker.
//
// Built only with Clang (-DITCHSTREAM_FUZZER=ON). Run it under ASan + UBSan:
//
//   ./build/itchstream_fuzz -max_total_time=600 -jobs=4
//
// A crash writes a reproducer to ./crash-*. Feed that file back in with
// `./build/itchstream_fuzz crash-<hash>` to replay it.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "itchstream/decoder.hpp"
#include "itchstream/feed.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;

    const std::byte* bytes = reinterpret_cast<const std::byte*>(data);

    // Path 1: treat the input as a length-prefixed message stream, which is
    // what the real tool does. The walker owns all bounds checking.
    itchstream::walk(bytes, size, [](const itchstream::RawMessage& message) {
        (void)message.timestamp_if_present();
        if (itchstream::is_decoded(message.type)) {
            const std::optional<itchstream::Payload> payload =
                itchstream::decode_payload(message.type, message.payload, message.length);
            // The walker validated the length, so a decoded type must decode.
            assert(payload.has_value());
            (void)payload;
        }
        return itchstream::WalkAction::Continue;
    });

    // Path 2: treat byte 0 as the type and the rest as a single body, with the
    // length taken from the input size. This reaches decode_payload with lengths
    // that the walker would have rejected.
    const auto type = itchstream::to_msg_type(data[0]);
    const auto length = static_cast<std::uint16_t>(std::min<std::size_t>(size - 1, 0xFFFF));
    (void)itchstream::decode_payload(type, bytes + 1, length);

    return 0;
}
