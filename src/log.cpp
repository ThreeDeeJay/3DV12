// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "log.h"
#include "version.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>

static FILE*      g_File  = nullptr;
static LogLevel   g_Level = LogLevel::Info;
static std::mutex g_Mtx;

static const char* LevelTag(LogLevel l)
{
    switch (l) {
        case LogLevel::Error: return "ERR";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Info:  return "INF";
        case LogLevel::Debug: return "DBG";
        case LogLevel::Trace: return "TRC";
        default:              return "???";
    }
}

namespace Log {

void Init(const std::string& filePath, LogLevel level)
{
    std::lock_guard<std::mutex> lk(g_Mtx);
    g_Level = level;

    if (!filePath.empty())
        fopen_s(&g_File, filePath.c_str(), "a");
    if (!g_File) g_File = stderr;

    fprintf(g_File,
            "========================================\n"
            PROJECT_FULL " – version int %d\n"
            "========================================\n",
            VERSION);
    fflush(g_File);
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_Mtx);
    if (g_File && g_File != stderr) {
        fprintf(g_File, "[INF] Log shutdown.\n");
        fclose(g_File);
    }
    g_File = nullptr;
}

void Write(LogLevel level, const char* fmt, ...)
{
    if (level > g_Level) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std::lock_guard<std::mutex> lk(g_Mtx);
    if (!g_File) return;
    fprintf(g_File, "[%s] %s\n", LevelTag(level), msg);
    fflush(g_File);
}

LogLevel ParseLevel(const std::string& s)
{
    if (s == "Off"   || s == "0") return LogLevel::Off;
    if (s == "Error" || s == "1") return LogLevel::Error;
    if (s == "Warn"  || s == "2") return LogLevel::Warn;
    if (s == "Debug" || s == "4") return LogLevel::Debug;
    if (s == "Trace" || s == "5") return LogLevel::Trace;
    return LogLevel::Info;
}

} // namespace Log
