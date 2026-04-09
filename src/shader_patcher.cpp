// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_patcher.h"
#include "dxbc_parser.h"
#include "config.h"
#include "log.h"
#include <Windows.h>
#include <d3dcompiler.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// FNV-1a 64-bit hash
// ---------------------------------------------------------------------------
static uint64_t FNV64(const void* data, size_t len)
{
    const uint8_t* p   = static_cast<const uint8_t*>(data);
    uint64_t       h   = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++)
        h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

// ---------------------------------------------------------------------------
// D3DReflect wrapper (loaded dynamically so the wrapper doesn't hard-require
// d3dcompiler_47.dll on systems where D3D12 apps use DXC only)
// ---------------------------------------------------------------------------
static HRESULT ReflectShader(const void* pBytecode, size_t len, ID3D12ShaderReflection** ppReflect)
{
    // Try d3dcompiler_47 first, then fall back to d3dcompiler.
    static HMODULE hComp = nullptr;
    if (!hComp) hComp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!hComp) hComp = LoadLibraryW(L"d3dcompiler.dll");
    if (!hComp) return E_NOINTERFACE;

    using PFN_D3DReflect = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfnReflect = reinterpret_cast<PFN_D3DReflect>(GetProcAddress(hComp, "D3DReflect"));
    if (!pfnReflect) return E_NOINTERFACE;
    return pfnReflect(pBytecode, len, IID_ID3D12ShaderReflection, (void**)ppReflect);
}

// ---------------------------------------------------------------------------
namespace ShaderPatcher {

ShaderInfo Analyse(const void* pBytecode, size_t byteLen)
{
    ShaderInfo info;
    if (!pBytecode || byteLen < 4) return info;

    // DXBC magic check
    uint32_t magic;
    memcpy(&magic, pBytecode, 4);
    if (magic != kFourCC_DXBC) {
        LOG_WARN("ShaderPatcher::Analyse: unknown magic 0x%08X", magic);
        return info;
    }

    info.hash = FNV64(pBytecode, byteLen);
    if (Cfg::g.logShaderHash)
        LOG_DEBUG("Shader hash: 0x%016llX  size=%zu", (unsigned long long)info.hash, byteLen);

    // Build DXBCBlob
    DXBCBlob blob;
    blob.data.assign((const uint8_t*)pBytecode, (const uint8_t*)pBytecode + byteLen);

    if (!blob.IsValid()) { LOG_WARN("ShaderPatcher: invalid DXBC blob"); return info; }
    info.isDXIL = blob.IsDXIL();

    if (info.isDXIL) {
        // DXIL (SM6+): binary patching via LLVM/DXC is not implemented in this version.
        // Mark as unsupported for DXBC engine; a future DXC-based engine would handle these.
        LOG_DEBUG("ShaderPatcher: DXIL shader detected (hash 0x%016llX) – "
                  "DXIL patching not yet implemented; skipping.", (unsigned long long)info.hash);
        info.isSupported = false;
        return info;
    }

    // Reflect via D3DCompiler to determine binding counts
    ID3D12ShaderReflection* pRefl = nullptr;
    HRESULT hr = ReflectShader(pBytecode, byteLen, &pRefl);
    if (SUCCEEDED(hr) && pRefl) {
        D3D12_SHADER_DESC desc{};
        pRefl->GetDesc(&desc);
        info.numCBs      = desc.ConstantBuffers;
        info.numSRVs     = 0;
        info.numUAVs     = 0;
        info.numSamplers = desc.BoundResources;

        for (UINT i = 0; i < desc.BoundResources; i++) {
            D3D12_SHADER_INPUT_BIND_DESC bd{};
            pRefl->GetResourceBindingDesc(i, &bd);
            switch (bd.Type) {
                case D3D_SIT_TEXTURE:    info.numSRVs++;   break;
                case D3D_SIT_UAV_RWTYPED:
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS:
                                          info.numUAVs++;   break;
                case D3D_SIT_SAMPLER:    info.numSamplers++; break;
                default: break;
            }
        }

        // Check if shader writes SV_Depth
        for (UINT i = 0; i < desc.OutputParameters; i++) {
            D3D12_SIGNATURE_PARAMETER_DESC od{};
            pRefl->GetOutputParameterDesc(i, &od);
            if (od.SystemValueType == D3D_NAME_DEPTH) {
                info.writesSVDepth = true;
                break;
            }
        }
        pRefl->Release();
    }

    // Classify role using heuristics
    uint8_t shType = blob.ShaderType();
    bool isVS = (shType == 0xFE);
    bool isPS = (shType == 0xFF);
    bool isCS = (shType == 0x4B);

    if (isVS) {
        // Shadow-map VS: typically few or no SRV bindings, no samplers
        if (info.numSRVs == 0 && info.numSamplers == 0)
            info.role = ShaderRole::VS_ShadowMap;
        else
            info.role = ShaderRole::VS_Generic;
    } else if (isPS) {
        if (info.numSRVs >= 3 && !info.writesSVDepth)
            info.role = ShaderRole::PS_Deferred;
        else if (info.numSRVs >= 2 && !info.writesSVDepth)
            info.role = ShaderRole::PS_PostProcess;
        else if (info.writesSVDepth && info.numSRVs == 0)
            info.role = ShaderRole::PS_ShadowMap;
        else
            info.role = ShaderRole::PS_Opaque;
    } else if (isCS) {
        if (info.numUAVs > 0 && info.numSRVs >= 2)
            info.role = ShaderRole::CS_Deferred;
        else if (info.numUAVs > 0)
            info.role = ShaderRole::CS_PostProcess;
        else
            info.role = ShaderRole::CS_Generic;
    }

    info.isSupported = (isVS || isPS || isCS);
    return info;
}

// ---------------------------------------------------------------------------
std::vector<uint8_t> Patch(const void* pBytecode, size_t byteLen,
                            const ShaderInfo& info, uint32_t cbSlot)
{
    if (!info.isSupported || info.isDXIL) return {};

    DXBCBlob blob;
    blob.data.assign((const uint8_t*)pBytecode, (const uint8_t*)pBytecode + byteLen);

    bool shouldPatch = false;
    switch (info.role) {
        // Always patch vertex shaders (position correction)
        case ShaderRole::VS_Generic:
        case ShaderRole::VS_ShadowMap:
            shouldPatch = Cfg::g.patchVertexShaders;
            break;
        // Pixel shaders: patch deferred and post-process passes
        case ShaderRole::PS_Deferred:
        case ShaderRole::PS_PostProcess:
            shouldPatch = Cfg::g.patchPixelShaders;
            break;
        // Shadow-map PS: typically depth-only, position correction via VS is enough
        case ShaderRole::PS_ShadowMap:
            shouldPatch = false;
            break;
        case ShaderRole::PS_Opaque:
            shouldPatch = Cfg::g.patchPixelShaders && Cfg::g.autoDetect;
            break;
        // Compute
        case ShaderRole::CS_Deferred:
        case ShaderRole::CS_PostProcess:
            shouldPatch = Cfg::g.patchComputeShaders;
            break;
        default:
            shouldPatch = false;
    }

    if (!shouldPatch) return {};

    LOG_DEBUG("ShaderPatcher::Patch role=%d hash=0x%016llX cb=%u",
              (int)info.role, (unsigned long long)info.hash, cbSlot);

    DXBCBlob patched = DXBCPatcher::PatchBlob(blob, cbSlot);
    if (patched.data.empty()) {
        LOG_WARN("ShaderPatcher: PatchBlob failed for hash 0x%016llX", (unsigned long long)info.hash);
        return {};
    }
    return patched.data;
}

// ---------------------------------------------------------------------------
void DumpIfEnabled(const void* pBytecode, size_t byteLen,
                   const ShaderInfo& info, const char* suffix)
{
    if (!Cfg::g.dumpShaders) return;
    try {
        std::filesystem::create_directories(Cfg::g.dumpDir);
        std::ostringstream name;
        name << Cfg::g.dumpDir << "/"
             << std::hex << std::uppercase << std::setfill('0') << std::setw(16)
             << info.hash << "_" << suffix << ".bin";
        std::ofstream f(name.str(), std::ios::binary);
        if (f.is_open()) f.write((const char*)pBytecode, byteLen);
    } catch (...) {}
}

// ---------------------------------------------------------------------------
bool PatchGraphicsPSO(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                      PatchedPSO& storage, uint32_t cbSlot)
{
    bool anyPatched = false;

    // Vertex Shader
    if (desc.VS.pShaderBytecode && desc.VS.BytecodeLength > 0) {
        auto info = Analyse(desc.VS.pShaderBytecode, desc.VS.BytecodeLength);
        DumpIfEnabled(desc.VS.pShaderBytecode, desc.VS.BytecodeLength, info, "VS_pre");
        auto patched = Patch(desc.VS.pShaderBytecode, desc.VS.BytecodeLength, info, cbSlot);
        if (!patched.empty()) {
            storage.patchedVS = std::move(patched);
            DumpIfEnabled(storage.patchedVS.data(), storage.patchedVS.size(), info, "VS_post");
            desc.VS.pShaderBytecode = storage.patchedVS.data();
            desc.VS.BytecodeLength  = storage.patchedVS.size();
            anyPatched = true;
            LOG_INFO("GraphicsPSO: VS patched (hash=0x%016llX)", (unsigned long long)info.hash);
        }
    }

    // Pixel Shader
    if (desc.PS.pShaderBytecode && desc.PS.BytecodeLength > 0) {
        auto info = Analyse(desc.PS.pShaderBytecode, desc.PS.BytecodeLength);
        DumpIfEnabled(desc.PS.pShaderBytecode, desc.PS.BytecodeLength, info, "PS_pre");
        auto patched = Patch(desc.PS.pShaderBytecode, desc.PS.BytecodeLength, info, cbSlot);
        if (!patched.empty()) {
            storage.patchedPS = std::move(patched);
            DumpIfEnabled(storage.patchedPS.data(), storage.patchedPS.size(), info, "PS_post");
            desc.PS.pShaderBytecode = storage.patchedPS.data();
            desc.PS.BytecodeLength  = storage.patchedPS.size();
            anyPatched = true;
            LOG_INFO("GraphicsPSO: PS patched (hash=0x%016llX)", (unsigned long long)info.hash);
        }
    }

    return anyPatched;
}

bool PatchComputePSO(D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                     PatchedCSPSO& storage, uint32_t cbSlot)
{
    if (!desc.CS.pShaderBytecode || desc.CS.BytecodeLength == 0) return false;
    auto info = Analyse(desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
    DumpIfEnabled(desc.CS.pShaderBytecode, desc.CS.BytecodeLength, info, "CS_pre");
    auto patched = Patch(desc.CS.pShaderBytecode, desc.CS.BytecodeLength, info, cbSlot);
    if (patched.empty()) return false;
    storage.patchedCS = std::move(patched);
    DumpIfEnabled(storage.patchedCS.data(), storage.patchedCS.size(), info, "CS_post");
    desc.CS.pShaderBytecode = storage.patchedCS.data();
    desc.CS.BytecodeLength  = storage.patchedCS.size();
    LOG_INFO("ComputePSO: CS patched (hash=0x%016llX)", (unsigned long long)info.hash);
    return true;
}

} // namespace ShaderPatcher
