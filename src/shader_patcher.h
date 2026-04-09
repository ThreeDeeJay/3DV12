#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ShaderPatcher sits above DXBCPatcher and DXBCBlob.
// It is called from the wrapped ID3D12Device::CreateGraphicsPipelineState /
// CreateComputePipelineState to analyse and patch every shader stage.
//
// Deferred-pass heuristics
// ─────────────────────────
// A pixel shader is classified as a "deferred lighting/post-process" pass when:
//   • It binds ≥ 3 shader-resource views (multiple G-buffer samples)
//   • It does NOT write SV_Depth (not a depth-only / shadow pass)
//   • It does NOT have SV_Position as the only input
//
// A compute shader is classified as "post-process / AO / shadows" when:
//   • It has ≥ 1 UAV output
//   • It samples ≥ 2 textures
//
// All vertex shaders are patched (position correction applies universally).
// Passes can be individually enabled/disabled from the INI.

#include <cstdint>
#include <vector>
#include <string>
#include <d3d12.h>
#include <d3dcompiler.h>

// ---------------------------------------------------------------------------
// Shader classification result
// ---------------------------------------------------------------------------
enum class ShaderRole : uint8_t {
    Unknown            = 0,
    // Vertex stages
    VS_Generic         = 1,   // generic VS – always inject position correction
    VS_ShadowMap       = 2,   // depth-only VS – inject position correction (shadow maps still need it)
    // Pixel stages
    PS_Opaque          = 3,   // standard forward opaque
    PS_Deferred        = 4,   // G-buffer sampling / deferred lighting / AO
    PS_Transparent     = 5,
    PS_ShadowMap       = 6,   // depth write only – inject position correction via VS pairing
    PS_PostProcess     = 7,   // screen-space effects (SSAO, SSR, bloom, etc.)
    // Compute
    CS_Generic         = 8,
    CS_Deferred        = 9,   // AO, shadow filtering, deferred lighting
    CS_PostProcess     = 10,
};

struct ShaderInfo {
    ShaderRole  role        = ShaderRole::Unknown;
    uint64_t    hash        = 0;  // FNV-1a 64-bit of raw bytecode
    uint32_t    numCBs      = 0;
    uint32_t    numSRVs     = 0;
    uint32_t    numUAVs     = 0;
    uint32_t    numSamplers = 0;
    bool        writesSVDepth = false;
    bool        isDXIL      = false;
    bool        isSupported = false; // false → skip patching
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace ShaderPatcher {

// Analyse a raw DXBC/DXIL bytecode blob.
ShaderInfo Analyse(const void* pBytecode, size_t byteLen);

// Patch a shader blob for stereoscopy.
// Returns the patched blob. Returns empty vector if patching is not applicable.
std::vector<uint8_t> Patch(const void* pBytecode, size_t byteLen,
                           const ShaderInfo& info, uint32_t cbSlot);

// Optionally dump bytecode to disk (controlled by Cfg::g.dumpShaders).
void DumpIfEnabled(const void* pBytecode, size_t byteLen,
                   const ShaderInfo& info, const char* suffix);

// Patch all stages of a Graphics PSO descriptor.
// Modified stages are rewritten into psoDesc (pBytecode pointers and sizes are
// updated to point into the patchedBufs storage which must outlive the PSO creation).
struct PatchedPSO {
    // Storage for patched bytecode; these back the D3D12_SHADER_BYTECODE structs.
    std::vector<uint8_t> patchedVS, patchedPS;
};
bool PatchGraphicsPSO(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                      PatchedPSO& storage, uint32_t cbSlot);

struct PatchedCSPSO {
    std::vector<uint8_t> patchedCS;
};
bool PatchComputePSO(D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                     PatchedCSPSO& storage, uint32_t cbSlot);

} // namespace ShaderPatcher
