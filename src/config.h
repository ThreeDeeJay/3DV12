#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <cstdint>

// ---------------------------------------------------------------------------
// All runtime-configurable settings, populated from 3DV12.ini
// ---------------------------------------------------------------------------
struct Config {
    // [General]
    std::string logFile        = "3DV12.log";
    std::string logLevel       = "Info";

    // [Stereo]
    bool  stereoEnabled        = true;
    float separationScale      = 1.0f;   // multiplier on NVAPI separation
    float convergenceScale     = 1.0f;   // multiplier on NVAPI convergence
    bool  autoDetect           = true;   // heuristic deferred-pass detection
    bool  patchVertexShaders   = true;
    bool  patchPixelShaders    = true;
    bool  patchComputeShaders  = true;
    // HLSL register for stereo constant buffer (b<N>, space13)
    int   stereoConstantBuffer = 13;
    // Root signature: append stereo param slot at this index (auto = -1)
    int   stereoRootParamIndex = -1;

    // [Debug]
    bool        dumpShaders    = false;
    std::string dumpDir        = "ShaderDump";
    bool        logShaderHash  = true;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace Cfg {

// Load INI from the given path. Falls back to defaults on missing keys.
// Returns false only if the file cannot be opened (non-fatal).
bool Load(const std::string& iniPath, Config& out);

// Write a default INI to disk.
bool WriteDefaults(const std::string& iniPath);

// Global instance set by Load()
extern Config g;

} // namespace Cfg
