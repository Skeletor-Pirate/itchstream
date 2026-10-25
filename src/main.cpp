#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "itchstream/feed.hpp"
#include "itchstream/stats.hpp"

namespace {

constexpr const char* kVersion = "0.1.0-m1";

// Exit codes are part of the contract: scripts and CI branch on them.
constexpr int kExitOk = 0;
constexpr int kExitOpenFailed = 1;
constexpr int kExitBadData = 2;
constexpr int kExitUsage = 3;
constexpr int kExitNotImplemented = 4;
constexpr int kExitNondeterministic = 5;

const char* kUsage =
    "usage:\n"
    "  itchstream stats <file> [options]\n"
    "  itchstream dump <file> --symbol SYM --out FILE     (M2, not implemented)\n"
    "  itchstream replay <file> [options]                 (M3, not implemented)\n"
    "  itchstream --help | --version\n"
    "\n"
    "stats options:\n"
    "  --repeat N     N measured runs after one discarded warm-up run (default 1)\n"
    "  --length-only  walk and count only; do not decode payloads\n"
    "  --json         machine-readable output\n"
    "  --quiet        one line per run, no tables\n"
    "  --no-machine   omit the machine and build header\n"
    "\n"
    "exit codes: 0 ok, 1 cannot open file, 2 malformed data, 3 bad usage,\n"
    "            4 not implemented yet, 5 message count differs between runs\n";

struct Options {
    std::string command;
    std::string file;
    std::uint64_t repeat = 1;
    bool decode = true;
    bool json = false;
    bool quiet = false;
    bool machine = true;
};

void print_usage(std::ostream& out) {
    out << kUsage;
}

int usage_error(const std::string& message) {
    std::cerr << "itchstream: " << message << "\n\n";
    print_usage(std::cerr);
    return kExitUsage;
}

double median_of(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n % 2 == 1) return values[n / 2];
    return 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

// A 10 KB file finishes in microseconds; "0.0000 s" reads like a bug.
std::string format_seconds(double seconds) {
    char buffer[40];
    if (seconds < 1e-3) {
        std::snprintf(buffer, sizeof buffer, "%.1f us", seconds * 1e6);
    } else if (seconds < 1.0) {
        std::snprintf(buffer, sizeof buffer, "%.3f ms", seconds * 1e3);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.4f s", seconds);
    }
    return buffer;
}

void print_stats_report(const Options& options, const itchstream::MappedFile& file,
                        const itchstream::Stats& stats, const std::vector<double>& seconds) {
    const double median = median_of(seconds);
    const double fastest = seconds.empty() ? 0.0 : *std::min_element(seconds.begin(), seconds.end());
    const double slowest = seconds.empty() ? 0.0 : *std::max_element(seconds.begin(), seconds.end());
    const double rate = median > 0.0 ? static_cast<double>(stats.messages) / median / 1e6 : 0.0;
    const double bytes_rate = median > 0.0 ? static_cast<double>(stats.bytes_consumed) / median / 1e6 : 0.0;

    std::printf("file      %s\n", file.path().c_str());
    std::printf("mapped    %s bytes\n", itchstream::format_count(file.size()).c_str());
    std::printf("messages  %s%s\n", itchstream::format_count(stats.messages).c_str(),
                stats.walk.stopped_early ? " (visitor stopped early)" : "");
    std::printf("bytes     %s walked%s\n", itchstream::format_count(stats.bytes_consumed).c_str(),
                stats.walk.error == itchstream::WalkError::None ? " (whole file)" : "");

    if (stats.have_timestamp) {
        std::printf("first ts  %s\n", itchstream::format_timestamp(stats.first_timestamp).c_str());
        std::printf("last ts   %s\n", itchstream::format_timestamp(stats.last_timestamp).c_str());
    } else {
        std::printf("first ts  (no message carried a timestamp)\n");
    }

    std::printf("decode    %s\n",
                options.decode ? (itchstream::format_count(stats.decoded) + " payloads, " +
                                  itchstream::format_count(stats.decode_failures) + " failures").c_str()
                               : "skipped (--length-only)");
    std::printf("skipped   %s messages of types this version does not decode\n",
                itchstream::format_count(stats.skipped).c_str());

    std::printf("\ntype  %-30s %14s %8s %14s\n", "message", "count", "share", "bytes");
    for (const itchstream::TypeInfo& info : itchstream::kTypeTable) {
        const itchstream::TypeTally& tally =
            stats.by_type[static_cast<std::uint8_t>(info.type)];
        if (tally.messages == 0) continue;
        const double share = stats.messages > 0
                                 ? 100.0 * static_cast<double>(tally.messages) / static_cast<double>(stats.messages)
                                 : 0.0;
        std::printf("  %s  %-30s %14s %7.2f%% %14s\n",
                    std::string(itchstream::printable_type(info.type)).c_str(), info.name,
                    itchstream::format_count(tally.messages).c_str(), share,
                    itchstream::format_count(tally.bytes).c_str());
    }
    for (std::size_t byte = 0; byte < 256; ++byte) {
        const auto type = itchstream::to_msg_type(static_cast<std::uint8_t>(byte));
        if (itchstream::is_decoded(type)) continue;
        const itchstream::TypeTally& tally = stats.by_type[byte];
        if (tally.messages == 0) continue;
        const double share = stats.messages > 0
                                 ? 100.0 * static_cast<double>(tally.messages) / static_cast<double>(stats.messages)
                                 : 0.0;
        std::printf("  %s  %-30s %14s %7.2f%% %14s\n",
                    std::string(itchstream::printable_type(type)).c_str(), "(skipped type)",
                    itchstream::format_count(tally.messages).c_str(), share,
                    itchstream::format_count(tally.bytes).c_str());
    }

    const double spread_percent = median > 0.0 ? 100.0 * (slowest - fastest) / median : 0.0;

    std::printf("\ntiming    median %s over %zu run%s (min %s, max %s, spread %.1f%%)\n",
                format_seconds(median).c_str(), seconds.size(), seconds.size() == 1 ? "" : "s",
                format_seconds(fastest).c_str(), format_seconds(slowest).c_str(), spread_percent);
    std::printf("rate      %.2f M messages/s, %.1f MB/s\n", rate, bytes_rate);

    if (!stats.timestamps_monotonic) {
        std::printf("WARNING   timestamps went backwards at least once: the walk is out of step with the file\n");
    }
    if (!stats.walk.ok()) {
        std::printf("ERROR     %s\n", itchstream::describe(stats.walk).c_str());
    }
}

// A Windows path dropped into JSON unescaped turns "data\day.itch" into a tab
// escape and invalid JSON. Escape the path (and anything else free-form).
std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char raw : input) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void print_stats_json(const Options& options, const itchstream::MappedFile& file,
                      const itchstream::Stats& stats, const std::vector<double>& seconds) {
    const double median = median_of(seconds);
    const double fastest = seconds.empty() ? 0.0 : *std::min_element(seconds.begin(), seconds.end());
    const double slowest = seconds.empty() ? 0.0 : *std::max_element(seconds.begin(), seconds.end());
    const double rate = median > 0.0 ? static_cast<double>(stats.messages) / median : 0.0;
    const std::string escaped_path = json_escape(file.path());

    std::printf("{\"file\":\"%s\",\"bytes\":%llu,\"messages\":%llu,\"decoded\":%llu,"
                "\"decode_failures\":%llu,\"skipped\":%llu,\"decode_payloads\":%s,"
                "\"first_timestamp_ns\":%llu,\"last_timestamp_ns\":%llu,"
                "\"timestamps_monotonic\":%s,\"walk_ok\":%s,\"walk_error\":\"%s\","
                "\"runs\":%zu,\"median_seconds\":%.6f,\"min_seconds\":%.6f,\"max_seconds\":%.6f,"
                "\"messages_per_second\":%.0f,\"by_type\":{",
                escaped_path.c_str(), static_cast<unsigned long long>(file.size()),
                static_cast<unsigned long long>(stats.messages),
                static_cast<unsigned long long>(stats.decoded),
                static_cast<unsigned long long>(stats.decode_failures),
                static_cast<unsigned long long>(stats.skipped),
                options.decode ? "true" : "false",
                static_cast<unsigned long long>(stats.have_timestamp ? stats.first_timestamp : 0),
                static_cast<unsigned long long>(stats.have_timestamp ? stats.last_timestamp : 0),
                stats.timestamps_monotonic ? "true" : "false", stats.walk.ok() ? "true" : "false",
                itchstream::to_string(stats.walk.error), seconds.size(), median, fastest, slowest, rate);

    bool first = true;
    for (std::size_t byte = 0; byte < 256; ++byte) {
        const itchstream::TypeTally& tally = stats.by_type[byte];
        if (tally.messages == 0) continue;
        const auto type = itchstream::to_msg_type(static_cast<std::uint8_t>(byte));
        std::printf("%s\"%s\":{\"messages\":%llu,\"bytes\":%llu}", first ? "" : ",",
                    std::string(itchstream::printable_type(type)).c_str(),
                    static_cast<unsigned long long>(tally.messages),
                    static_cast<unsigned long long>(tally.bytes));
        first = false;
    }
    std::printf("}}\n");
}

int run_stats(const Options& options) {
    itchstream::MappedFile file;
    std::string error;
    if (!file.open(options.file, error)) {
        std::cerr << "itchstream: " << error << "\n";
        return kExitOpenFailed;
    }

    if (!options.json && !options.quiet) {
        const std::string machine = itchstream::machine_summary();
        std::printf("itchstream %s\n", kVersion);
        if (options.machine) {
            std::printf("machine   %s\n", machine.c_str());
            std::printf("build     %s\n", itchstream::build_summary().c_str());
        }
    }

    itchstream::StatsOptions stats_options;
    stats_options.decode_payloads = options.decode;

    // PRD method: run each benchmark twice and discard the first, so the file
    // is in the page cache and the numbers describe the parser, not the disk.
    const std::uint64_t warm_up_runs = 1;
    const std::uint64_t total_runs = options.repeat + warm_up_runs;

    std::vector<double> seconds;
    seconds.reserve(static_cast<std::size_t>(options.repeat));

    itchstream::Stats stats{};
    std::uint64_t reference_messages = 0;
    bool nondeterministic = false;

    for (std::uint64_t run = 0; run < total_runs; ++run) {
        const auto start = std::chrono::steady_clock::now();
        itchstream::Stats current = itchstream::collect(file.data(), file.size(), stats_options);
        const auto finish = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(finish - start).count();

        if (run < warm_up_runs) continue;

        if (run == warm_up_runs) {
            reference_messages = current.messages;
        } else if (current.messages != reference_messages) {
            nondeterministic = true;
        }

        if (!options.json && options.quiet) {
            std::printf("run %llu  %s messages  %s  %.2f M msg/s%s\n",
                        static_cast<unsigned long long>(run - warm_up_runs),
                        itchstream::format_count(current.messages).c_str(),
                        format_seconds(elapsed).c_str(),
                        elapsed > 0.0 ? static_cast<double>(current.messages) / elapsed / 1e6 : 0.0,
                        current.walk.ok() ? "" : "  (walk error)");
        }

        seconds.push_back(elapsed);
        stats = std::move(current);
    }

    if (options.json) {
        print_stats_json(options, file, stats, seconds);
    } else if (!options.quiet) {
        print_stats_report(options, file, stats, seconds);
    }

    if (nondeterministic) {
        std::cerr << "itchstream: message count changed between runs over the same bytes\n";
        return kExitNondeterministic;
    }
    if (!stats.walk.ok()) {
        return kExitBadData;
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(std::cout);
        return kExitUsage;
    }

    Options options;
    options.command = argv[1];

    if (options.command == "--help" || options.command == "-h" || options.command == "help") {
        print_usage(std::cout);
        return kExitOk;
    }
    if (options.command == "--version" || options.command == "-v") {
        std::printf("itchstream %s\n", kVersion);
        return kExitOk;
    }

    if (options.command != "stats" && options.command != "dump" && options.command != "replay") {
        return usage_error("unknown command '" + options.command + "'");
    }

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--repeat") {
            if (i + 1 >= argc) return usage_error("--repeat needs a value");
            try {
                options.repeat = std::stoull(argv[++i]);
            } catch (const std::exception&) {
                return usage_error("--repeat needs a whole number");
            }
            if (options.repeat == 0) return usage_error("--repeat must be at least 1");
        } else if (arg == "--length-only") {
            options.decode = false;
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else if (arg == "--no-machine") {
            options.machine = false;
        } else if (!arg.empty() && arg.front() == '-') {
            return usage_error("unknown option '" + std::string(arg) + "'");
        } else if (options.file.empty()) {
            options.file = std::string(arg);
        } else {
            return usage_error("more than one file given");
        }
    }

    if (options.file.empty()) {
        return usage_error("no input file given");
    }

    if (options.command == "dump" || options.command == "replay") {
        std::cerr << "itchstream: '" << options.command
                  << "' is not implemented yet; this build is M1 (decoder + stats only)\n";
        return kExitNotImplemented;
    }

    return run_stats(options);
}
