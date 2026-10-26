// Host and build description.
//
// Every published number gets printed next to this, because a throughput figure
// without the machine and the flags is not a measurement, it is a claim.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include "itchstream/stats.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#else
#include <fstream>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace itchstream {
namespace {

#if !defined(_WIN32)

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n')) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string linux_cpu_model() {
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("model name", 0) == 0) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string model = trim(std::string_view(line).substr(colon + 1));
                if (!model.empty()) return model;
            }
        }
    }
    return "unknown CPU";
}

std::string linux_total_ram() {
    std::ifstream file("/proc/meminfo");
    std::string key;
    std::uint64_t kib = 0;
    while (file >> key) {
        if (key == "MemTotal:") {
            file >> kib;
            break;
        }
        file.ignore(4096, '\n');
    }
    if (kib == 0) return "unknown RAM";
    const double gib = static_cast<double>(kib) / (1024.0 * 1024.0);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f GiB", gib);
    return std::string(buffer);
}

std::string linux_kernel() {
    struct utsname info {};
    if (::uname(&info) != 0) return "Linux";
    return std::string("Linux ") + info.release;
}

#endif

}  // namespace

std::string machine_summary() {
    std::string cpu = "unknown CPU";
    std::string ram = "unknown RAM";
    std::string os = "unknown OS";

#if defined(_WIN32)
    if (const char* identifier = std::getenv("PROCESSOR_IDENTIFIER")) {
        cpu = identifier;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (::GlobalMemoryStatusEx(&memory)) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.1f GiB",
                      static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
        ram = buffer;
    }
    os = "Windows";
#else
    cpu = linux_cpu_model();
    ram = linux_total_ram();
    os = linux_kernel();
#endif

    const unsigned cores = std::thread::hardware_concurrency();
    return cpu + " | " + std::to_string(cores) + " logical cores | " + ram + " | " + os;
}

std::string build_summary() {
    std::string out;

#if defined(__clang__)
    out = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    out = std::string("g++ ") + __VERSION__;
#elif defined(_MSC_VER)
    out = "MSVC " + std::to_string(_MSC_VER);
#else
    out = "unknown compiler";
#endif

    out += " | ";
#ifdef ITCHSTREAM_BUILD_TYPE
    out += ITCHSTREAM_BUILD_TYPE;
#else
    out += "unknown build type";
#endif

#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && defined(NDEBUG))
    out += " | optimised";
#else
    out += " | unoptimised (-O0)";
#endif

#ifdef ITCHSTREAM_CXX_FLAGS
    out += " | ";
    out += ITCHSTREAM_CXX_FLAGS;
#endif

#if defined(__SANITIZE_ADDRESS__)
    out += " | asan";
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    out += " | asan";
#endif
#if __has_feature(undefined_behavior_sanitizer)
    out += " | ubsan";
#endif
#endif

    return out;
}

}  // namespace itchstream
