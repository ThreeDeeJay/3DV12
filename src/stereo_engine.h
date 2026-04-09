#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thin wrapper around NVAPI stereo functions used for D3D12.
//
// NOTE: This file uses NVAPI type definitions inline so that the project does
// not require the full NVAPI SDK to compile.  If you have the NVAPI SDK, add
// the include path to CMakeLists.txt and define HAVE_NVAPI_SDK; then the real
// nvapi.h types will be used instead of our forward declarations.
//
// Runtime NVAPI loading
// ─────────────────────
// nvapi64.dll / nvapi.dll is loaded at runtime so the wrapper works on
// non-NVIDIA systems (stereo features simply remain inactive).

#include <cstdint>
#include <d3d12.h>

// ---------------------------------------------------------------------------
// Minimal NVAPI forward declarations (skipped when using the real SDK)
// ---------------------------------------------------------------------------
#ifndef HAVE_NVAPI_SDK

typedef uint32_t NvU32;
typedef int      NvAPI_Status;
#define NVAPI_OK 0

// Stereo handle opaque type
struct _NVDX_ObjectHandle;
typedef _NVDX_ObjectHandle* NVDX_ObjectHandle;
using StereoHandle = void*;  // NvStereoHandle is void*

// Eye enum
enum NV_STEREO_ACTIVE_EYE {
    NVAPI_STEREO_EYE_LEFT  = 1,
    NVAPI_STEREO_EYE_RIGHT = 2,
    NVAPI_STEREO_EYE_MONO  = 3,
};

#else
#include <nvapi.h>
using StereoHandle = StereoHandle;
#endif // HAVE_NVAPI_SDK

// ---------------------------------------------------------------------------
// Stereo parameters snapshot – polled each frame
// ---------------------------------------------------------------------------
struct StereoParams {
    float separation;    // eye separation (0.0 – 100.0, NVAPI units)
    float convergence;   // convergence depth
    float eyeSign;       // -0.5f (left) or +0.5f (right)
    float padding;       // reserved
};

// ---------------------------------------------------------------------------
namespace StereoEngine {

// Initialise NVAPI and create a stereo handle from the D3D12 device.
// Returns false on non-NVIDIA hardware or when stereo is disabled in driver.
bool Init(ID3D12Device* pDevice);

// Release the stereo handle and unload NVAPI.
void Shutdown();

// Returns true if NVAPI stereo is enabled and activated.
bool IsActive();

// Query current separation and convergence from NVAPI.
// Fills out a StereoParams structure for the given eye.
// separationScale / convergenceScale come from the INI config.
StereoParams QueryParams(NV_STEREO_ACTIVE_EYE eye,
                         float separationScale  = 1.0f,
                         float convergenceScale = 1.0f);

// Activate stereo for the given eye (called per-eye before rendering).
bool SetEye(NV_STEREO_ACTIVE_EYE eye);

} // namespace StereoEngine
