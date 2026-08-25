#include "pw/core/log.h"

#include <atomic>
#include <cstdio>

namespace pw {
namespace {

std::atomic<LogLevel> gLevel{LogLevel::Info};
std::atomic<LogSink> gSink{nullptr};
std::atomic<uint64_t> gCounts[6] = {};

const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "OFF  ";
    }
}

void defaultSink(LogLevel level, const char* category, const char* message) {
    std::FILE* out = level >= LogLevel::Warn ? stderr : stdout;
    std::fprintf(out, "[%s] %-10s %s\n", levelName(level), category, message);
}

}  // namespace

void setLogLevel(LogLevel level) { gLevel.store(level, std::memory_order_relaxed); }
LogLevel logLevel() { return gLevel.load(std::memory_order_relaxed); }
void setLogSink(LogSink sink) { gSink.store(sink, std::memory_order_relaxed); }

uint64_t logCount(LogLevel level) {
    return gCounts[static_cast<int>(level)].load(std::memory_order_relaxed);
}

void resetLogCounts() {
    for (auto& c : gCounts) c.store(0, std::memory_order_relaxed);
}

void logMessage(LogLevel level, const char* category, const char* fmt, ...) {
    if (level < gLevel.load(std::memory_order_relaxed)) return;
    gCounts[static_cast<int>(level)].fetch_add(1, std::memory_order_relaxed);

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    LogSink sink = gSink.load(std::memory_order_relaxed);
    (sink ? sink : &defaultSink)(level, category, buffer);
}

}  // namespace pw
