#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <cstdint>

// ---------------------------------------------------------------------------
// Log levels (increasing verbosity)
// ---------------------------------------------------------------------------
enum class LogLevel : int {
    Off   = 0,
    Error = 1,
    Warn  = 2,
    Info  = 3,
    Debug = 4,
    Trace = 5,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace Log {

void Init(const std::string& filePath, LogLevel level);
void Shutdown();

void Write(LogLevel level, const char* fmt, ...);

// Convenience wrappers
#define LOG_ERROR(...) Log::Write(LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...)  Log::Write(LogLevel::Warn,  __VA_ARGS__)
#define LOG_INFO(...)  Log::Write(LogLevel::Info,  __VA_ARGS__)
#define LOG_DEBUG(...) Log::Write(LogLevel::Debug, __VA_ARGS__)
#define LOG_TRACE(...) Log::Write(LogLevel::Trace, __VA_ARGS__)

LogLevel ParseLevel(const std::string& s);

} // namespace Log
