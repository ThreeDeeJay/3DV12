// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "log.h"
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace Cfg {

Config g;  // global instance

// ---------------------------------------------------------------------------
// Minimal INI parser
// Supports [Section] / Key = Value, # and ; comments, leading/trailing space
// ---------------------------------------------------------------------------
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
ParseINI(const std::string& path)
{
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> result;
    std::ifstream f(path);
    if (!f.is_open()) return result;

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        // Strip BOM
        if (line.size() >= 3 &&
            (uint8_t)line[0] == 0xEF &&
            (uint8_t)line[1] == 0xBB &&
            (uint8_t)line[2] == 0xBF)
            line = line.substr(3);

        // Trim leading space
        auto lt = line.begin();
        while (lt != line.end() && isspace((uint8_t)*lt)) ++lt;
        line = std::string(lt, line.end());

        // Strip comment and trailing space
        auto ci = line.find_first_of(";#");
        if (ci != std::string::npos) line = line.substr(0, ci);
        while (!line.empty() && isspace((uint8_t)line.back())) line.pop_back();

        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty()  && isspace((uint8_t)key.back()))  key.pop_back();
        while (!val.empty()  && isspace((uint8_t)val.front())) val = val.substr(1);
        while (!val.empty()  && isspace((uint8_t)val.back()))  val.pop_back();

        result[section][key] = val;
    }
    return result;
}

static bool ToBool(const std::string& s, bool def)
{
    if (s == "true"  || s == "1" || s == "yes" || s == "on")  return true;
    if (s == "false" || s == "0" || s == "no"  || s == "off") return false;
    return def;
}

static float ToFloat(const std::string& s, float def)
{
    try { return std::stof(s); } catch (...) { return def; }
}

static int ToInt(const std::string& s, int def)
{
    try { return std::stoi(s); } catch (...) { return def; }
}

// Helpers using a map with fallback
#define GET(sec, key, def)  (ini.count(sec) && ini.at(sec).count(key) ? ini.at(sec).at(key) : (def))

// ---------------------------------------------------------------------------
bool Load(const std::string& iniPath, Config& out)
{
    auto ini = ParseINI(iniPath);
    if (ini.empty()) {
        LOG_WARN("Config: '%s' not found or empty – using defaults.", iniPath.c_str());
        return false;
    }

    // [General]
    out.logFile  = GET("General", "LogFile",  "3DV12.log");
    out.logLevel = GET("General", "LogLevel", "Info");

    // [Stereo]
    out.stereoEnabled       = ToBool (GET("Stereo", "Enabled",             "true"), true);
    out.separationScale     = ToFloat(GET("Stereo", "SeparationScale",     "1.0"),  1.0f);
    out.convergenceScale    = ToFloat(GET("Stereo", "ConvergenceScale",    "1.0"),  1.0f);
    out.autoDetect          = ToBool (GET("Stereo", "AutoDetect",          "true"), true);
    out.patchVertexShaders  = ToBool (GET("Stereo", "PatchVertexShaders",  "true"), true);
    out.patchPixelShaders   = ToBool (GET("Stereo", "PatchPixelShaders",   "true"), true);
    out.patchComputeShaders = ToBool (GET("Stereo", "PatchComputeShaders", "true"), true);
    out.stereoConstantBuffer= ToInt  (GET("Stereo", "StereoConstantBuffer","13"),   13);
    out.stereoRootParamIndex= ToInt  (GET("Stereo", "StereoRootParamIndex","-1"),   -1);

    // [Debug]
    out.dumpShaders   = ToBool (GET("Debug", "DumpShaders",   "false"), false);
    out.dumpDir       =         GET("Debug", "DumpDir",        "ShaderDump");
    out.logShaderHash    = ToBool (GET("Debug", "LogShaderHash",  "true"),  true);
    out.enableDebugLayer = ToBool (GET("Debug", "EnableDebugLayer","false"), false);
    out.drainInfoQueue   = ToBool (GET("Debug", "DrainInfoQueue",  "true"),  true);

    LOG_INFO("Config loaded from '%s'.", iniPath.c_str());
    return true;
}

bool WriteDefaults(const std::string& iniPath)
{
    std::ofstream f(iniPath);
    if (!f.is_open()) return false;
    f << R"([General]
; Log file path (relative to DLL location or absolute)
LogFile  = 3DV12.log
; Verbosity: Off | Error | Warn | Info | Debug | Trace
LogLevel = Info

[Stereo]
; Master switch
Enabled             = true
; Scale factors applied on top of NVAPI values
SeparationScale     = 1.0
ConvergenceScale    = 1.0
; Heuristic detection of deferred/post-process passes
AutoDetect          = true
; Which shader stages to patch
PatchVertexShaders  = true
PatchPixelShaders   = true
PatchComputeShaders = true
; cbuffer register used for stereo params (b<N>, space13)
StereoConstantBuffer = 13
; Root parameter index for stereo CBV (-1 = append automatically)
StereoRootParamIndex = -1

[Debug]
; Dump unpatched + patched shader bytecode to DumpDir
DumpShaders  = false
DumpDir      = ShaderDump
LogShaderHash    = true
; Enable D3D12 SDK debug layer (requires debug DLLs installed)
EnableDebugLayer = false
; Drain ID3D12InfoQueue messages into the 3DV12 log
DrainInfoQueue   = true
)";
    return true;
}

} // namespace Cfg
