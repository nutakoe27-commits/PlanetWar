// pw_core — логирование.
//
// Лог намеренно НЕ является частью состояния симуляции: он ничего не решает,
// его можно выключить целиком, и его отсутствие не меняет ни одного расчёта.
// Поэтому здесь допустимы и время, и потоковый вывод — то, что внутри pw_sim
// запрещено.
#pragma once

#include <cstdarg>
#include <cstdint>

namespace pw {

enum class LogLevel : uint8_t { Trace = 0, Debug, Info, Warn, Error, Off };

/// Порог: сообщения ниже уровня не форматируются вообще.
void setLogLevel(LogLevel level);
LogLevel logLevel();

/// Подмена приёмника — сервер пишет в journald, редактор в панель, тесты в буфер.
using LogSink = void (*)(LogLevel level, const char* category, const char* message);
void setLogSink(LogSink sink);

void logMessage(LogLevel level, const char* category, const char* fmt, ...);

/// Счётчик сообщений уровня — тесты проверяют, что предупреждений не было.
uint64_t logCount(LogLevel level);
void resetLogCounts();

}  // namespace pw

#define PW_LOG_TRACE(cat, ...) ::pw::logMessage(::pw::LogLevel::Trace, cat, __VA_ARGS__)
#define PW_LOG_DEBUG(cat, ...) ::pw::logMessage(::pw::LogLevel::Debug, cat, __VA_ARGS__)
#define PW_LOG_INFO(cat, ...)  ::pw::logMessage(::pw::LogLevel::Info,  cat, __VA_ARGS__)
#define PW_LOG_WARN(cat, ...)  ::pw::logMessage(::pw::LogLevel::Warn,  cat, __VA_ARGS__)
#define PW_LOG_ERROR(cat, ...) ::pw::logMessage(::pw::LogLevel::Error, cat, __VA_ARGS__)
