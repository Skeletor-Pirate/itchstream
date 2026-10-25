// Deterministic mutation driver.
//
// libFuzzer is the better fuzzer, but it needs Clang, and the point of this
// target is to run everywhere (Windows, MSVC, g++, CI, ASan) with a failure
// that can be replayed exactly. It prints its seed up front and writes the
// current (seed, iteration) to a 16-byte trace file before every iteration, so
// a crash under ASan leaves behind the two numbers needed to reproduce it:
//
//   ./itchstream_fuzz_driver --iterations 1000000 --seed 7
//   # crash: read trace, then
//   ./itchstream_fuzz_driver --seed 7 --start-iteration 34812 --iterations 1
//
// This is the "log the fuzz seed" habit from FastBook's open items, applied
// here from the start.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "itchstream/decoder.hpp"
#include "itchstream/feed.hpp"

namespace {

struct Rng {
    std::uint64_t state;

    std::uint64_t next() {
        // splitmix64: fast, deterministic, no library dependency.
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    std::uint32_t below(std::uint32_t bound) {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
    }
};

const itchstream::TypeInfo& random_decoded_type(Rng& rng) {
    return itchstream::kTypeTable[rng.below(static_cast<std::uint32_t>(itchstream::kDecodedTypeCount))];
}

void append_u16_be(std::vector<std::byte>& buffer, std::uint16_t value) {
    buffer.push_back(std::byte{static_cast<std::uint8_t>(value >> 8)});
    buffer.push_back(std::byte{static_cast<std::uint8_t>(value & 0xFF)});
}

// Mode 0: unconstrained bytes.
// Mode 1: framed messages with random type bytes and random lengths.
// Mode 2: framed messages of known types with the correct length, so the
//         decoders see arbitrary field values.
// Mode 3: mostly valid stream with a few bytes corrupted.
void build_case(Rng& rng, std::vector<std::byte>& buffer, std::uint64_t iteration) {
    buffer.clear();

    switch (iteration % 4) {
        case 0: {
            const std::size_t size = rng.below(2048);
            for (std::size_t i = 0; i < size; ++i) {
                buffer.push_back(std::byte{static_cast<std::uint8_t>(rng.next() & 0xFF)});
            }
            break;
        }
        case 1: {
            const std::uint32_t messages = 1 + rng.below(8);
            for (std::uint32_t i = 0; i < messages; ++i) {
                const std::uint16_t length = static_cast<std::uint16_t>(rng.below(80));
                append_u16_be(buffer, length);
                for (std::uint16_t b = 0; b < length; ++b) {
                    buffer.push_back(std::byte{static_cast<std::uint8_t>(rng.next() & 0xFF)});
                }
            }
            break;
        }
        case 2: {
            const std::uint32_t messages = 1 + rng.below(8);
            for (std::uint32_t i = 0; i < messages; ++i) {
                const itchstream::TypeInfo& info = random_decoded_type(rng);
                append_u16_be(buffer, info.wire_size);
                for (std::uint16_t b = 0; b < info.wire_size; ++b) {
                    buffer.push_back(std::byte{static_cast<std::uint8_t>(rng.next() & 0xFF)});
                }
            }
            break;
        }
        default: {
            const std::uint32_t messages = 1 + rng.below(16);
            for (std::uint32_t i = 0; i < messages; ++i) {
                const itchstream::TypeInfo& info = random_decoded_type(rng);
                append_u16_be(buffer, info.wire_size);
                for (std::uint16_t b = 0; b < info.wire_size; ++b) {
                    buffer.push_back(std::byte{static_cast<std::uint8_t>(rng.next() & 0xFF)});
                }
            }
            // Corrupt one byte in the middle: length prefix, type byte or body.
            if (!buffer.empty()) {
                const std::size_t index = rng.below(static_cast<std::uint32_t>(buffer.size()));
                buffer[index] = std::byte{static_cast<std::uint8_t>(rng.next() & 0xFF)};
            }
            break;
        }
    }
}

void write_trace(std::ofstream& trace, std::uint64_t seed, std::uint64_t iteration) {
    if (!trace) return;
    trace.seekp(0);
    trace.write(reinterpret_cast<const char*>(&seed), sizeof seed);
    trace.write(reinterpret_cast<const char*>(&iteration), sizeof iteration);
    trace.flush();
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t iterations = 100000;
    std::uint64_t seed = 1;
    std::uint64_t start_iteration = 0;
    std::string trace_path = "itchstream_fuzz_trace.bin";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;

        if (arg == "--iterations" && has_value) {
            iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--seed" && has_value) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--start-iteration" && has_value) {
            start_iteration = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--trace" && has_value) {
            trace_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "usage: itchstream_fuzz_driver [--iterations N] [--seed S]\n"
                "                             [--start-iteration I] [--trace FILE]\n");
            return 0;
        } else {
            std::fprintf(stderr, "itchstream_fuzz_driver: unknown argument '%s'\n", arg.c_str());
            return 2;
        }
    }

    // Logged before the first iteration, so even a crash in setup is replayable.
    std::printf("seed=%llu iterations=%llu start_iteration=%llu trace=%s\n",
                static_cast<unsigned long long>(seed),
                static_cast<unsigned long long>(iterations),
                static_cast<unsigned long long>(start_iteration), trace_path.c_str());
    std::fflush(stdout);

    std::ofstream trace(trace_path, std::ios::binary | std::ios::trunc);
    if (!trace) {
        std::fprintf(stderr, "itchstream_fuzz_driver: cannot write trace file '%s'\n",
                     trace_path.c_str());
        return 2;
    }

    Rng rng{seed};
    std::vector<std::byte> buffer;
    std::uint64_t decoded_total = 0;
    std::uint64_t messages_total = 0;

    const std::uint64_t end_iteration = start_iteration + iterations;

    for (std::uint64_t iteration = start_iteration; iteration < end_iteration; ++iteration) {
        write_trace(trace, seed, iteration);
        build_case(rng, buffer, iteration);

        const itchstream::WalkResult result =
            itchstream::walk(buffer.data(), buffer.size(), [&](const itchstream::RawMessage& message) {
                ++messages_total;

                // Invariant: a decoded type whose length the walker accepted
                // must decode. A failure here is a decoder/walker disagreement,
                // reported with the seed so it can be replayed.
                if (itchstream::is_decoded(message.type)) {
                    const std::optional<itchstream::Payload> payload =
                        itchstream::decode_payload(message.type, message.payload, message.length);
                    if (!payload.has_value()) {
                        std::fprintf(stderr,
                                     "INVARIANT FAILURE: seed=%llu iteration=%llu type=%c length=%u "
                                     "did not decode\n",
                                     static_cast<unsigned long long>(seed),
                                     static_cast<unsigned long long>(iteration),
                                     static_cast<char>(static_cast<std::uint8_t>(message.type)),
                                     static_cast<unsigned>(message.length));
                        std::fflush(stderr);
                        std::exit(3);
                    }
                    ++decoded_total;
                }
                (void)message.timestamp_if_present();
                return itchstream::WalkAction::Continue;
            });

        // A clean walk must have consumed the whole buffer; a failed one must
        // have stopped on a real offset inside it.
        if (result.ok()) {
            if (result.bytes_consumed != buffer.size()) {
                std::fprintf(stderr,
                             "INVARIANT FAILURE: seed=%llu iteration=%llu ok walk consumed %llu of "
                             "%zu\n",
                             static_cast<unsigned long long>(seed),
                             static_cast<unsigned long long>(iteration),
                             static_cast<unsigned long long>(result.bytes_consumed), buffer.size());
                return 3;
            }
        } else if (result.error_offset >= buffer.size()) {
            std::fprintf(stderr,
                         "INVARIANT FAILURE: seed=%llu iteration=%llu error offset %llu outside "
                         "buffer of %zu\n",
                         static_cast<unsigned long long>(seed),
                         static_cast<unsigned long long>(iteration),
                         static_cast<unsigned long long>(result.error_offset), buffer.size());
            return 3;
        }

        if (iterations >= 100000 && iteration % 100000 == 0) {
            std::printf("  %llu iterations, %llu messages, %llu decoded\n",
                        static_cast<unsigned long long>(iteration),
                        static_cast<unsigned long long>(messages_total),
                        static_cast<unsigned long long>(decoded_total));
            std::fflush(stdout);
        }
    }

    std::printf("completed %llu iterations (%llu messages, %llu decoded), no crash\n",
                static_cast<unsigned long long>(iterations),
                static_cast<unsigned long long>(messages_total),
                static_cast<unsigned long long>(decoded_total));
    return 0;
}
