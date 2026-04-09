// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This DLL is a drop-in d3d12.dll proxy.
// It exports every function that the real d3d12.dll exports.
// All calls are forwarded to the real DLL, except D3D12CreateDevice which
// returns a WrappedDevice.
//
// Deployment:
//   Place 3DV12.dll renamed to d3d12.dll next to the game executable.
//   The real d3d12.dll must be accessible (system32 is fine; Windows loader
//   will find it after we've already been loaded from the app directory).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <string>
#include <filesystem>
#include <cstdio>

#include "version.h"
#include "log.h"
#include "config.h"
#include "wrapped_device.h"
#include "stereo_engine.h"

// ---------------------------------------------------------------------------
// Real d3d12.dll handle – used by wrapped_device.cpp for RS serialisation
// ---------------------------------------------------------------------------
HMODULE g_hRealD3D12 = nullptr;

// ---------------------------------------------------------------------------
// Helper: resolve directory of this DLL
// ---------------------------------------------------------------------------
static std::wstring GetMyDir()
{
    wchar_t buf[MAX_PATH] = {};
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&GetMyDir, &hSelf);
    GetModuleFileNameW(hSelf, buf, MAX_PATH);
    std::wstring p(buf);
    auto slash = p.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? p.substr(0, slash + 1) : L".\\";
}

// ---------------------------------------------------------------------------
// DLL entry
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);

        // --- Determine INI path ---
        auto myDir  = GetMyDir();
        std::wstring iniPathW = myDir + L"3DV12.ini";
        std::string  iniPath(iniPathW.begin(), iniPathW.end());

        // --- Load config (creates defaults if missing) ---
        if (!Cfg::Load(iniPath, Cfg::g))
            Cfg::WriteDefaults(iniPath);

        // --- Start logging ---
        std::string logPath = Cfg::g.logFile;
        if (logPath.find(':') == std::string::npos) {
            // Relative – put next to the DLL
            std::string myDirA(myDir.begin(), myDir.end());
            logPath = myDirA + logPath;
        }
        Log::Init(logPath, Log::ParseLevel(Cfg::g.logLevel));
        LOG_INFO("DllMain: " PROJECT_FULL " loading (version int %d).", VERSION);
        LOG_INFO("INI: %s", iniPath.c_str());

        // --- Load real d3d12.dll from system32 ---
        wchar_t sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring realPath = std::wstring(sysDir) + L"\\d3d12.dll";
        g_hRealD3D12 = LoadLibraryW(realPath.c_str());
        if (!g_hRealD3D12) {
            LOG_ERROR("DllMain: Failed to load real d3d12.dll from %ls!", realPath.c_str());
            return FALSE;
        }
        LOG_INFO("DllMain: Loaded real d3d12.dll from %ls", realPath.c_str());
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        StereoEngine::Shutdown();
        LOG_INFO("DllMain: Unloading.");
        Log::Shutdown();
        if (g_hRealD3D12) {
            FreeLibrary(g_hRealD3D12);
            g_hRealD3D12 = nullptr;
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Proxy helpers
// ---------------------------------------------------------------------------
template<typename T>
static T GetRealProc(const char* name)
{
    if (!g_hRealD3D12) return nullptr;
    return reinterpret_cast<T>(GetProcAddress(g_hRealD3D12, name));
}

// ---------------------------------------------------------------------------
// D3D12CreateDevice – the primary intercept point
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI D3D12CreateDevice(
    IUnknown*         pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID            riid,
    void**            ppDevice)
{
    using PFN = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateDevice");
    if (!pfn) return E_NOTIMPL;

    // Create the real device first
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = pfn(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    if (FAILED(hr) || !pRealDevice) return hr;

    LOG_INFO("D3D12CreateDevice: real device created (%p).", (void*)pRealDevice);

    // Initialise NVAPI stereo from the real device
    if (Cfg::g.stereoEnabled)
        StereoEngine::Init(pRealDevice);

    // Wrap the real device
    auto* pWrapped = new WrappedDevice(pRealDevice);
    pRealDevice->Release(); // WrappedDevice took its own ref

    // QI to the requested interface (all versions supported via our vtable)
    hr = pWrapped->QueryInterface(riid, ppDevice);
    pWrapped->Release();

    LOG_INFO("D3D12CreateDevice: returning wrapped device.");
    return hr;
}

// ---------------------------------------------------------------------------
// Forwarded exports – all other d3d12.dll entry points
// ---------------------------------------------------------------------------

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug)
{
    using PFN = HRESULT(WINAPI*)(REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12GetDebugInterface");
    return pfn ? pfn(riid, ppvDebug) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes, REFIID pRootSignatureDeserializerInterface, void** ppRootSignatureDeserializer)
{
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateRootSignatureDeserializer");
    return pfn ? pfn(pSrcData, SrcDataSizeInBytes, pRootSignatureDeserializerInterface, ppRootSignatureDeserializer) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature, D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob)
{
    using PFN = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto pfn  = GetRealProc<PFN>("D3D12SerializeRootSignature");
    return pfn ? pfn(pRootSignature, Version, ppBlob, ppErrorBlob) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob)
{
    using PFN = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    auto pfn  = GetRealProc<PFN>("D3D12SerializeVersionedRootSignature");
    return pfn ? pfn(pRootSignature, ppBlob, ppErrorBlob) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface, void** ppRootSignatureDeserializer)
{
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateVersionedRootSignatureDeserializer");
    return pfn ? pfn(pSrcData, SrcDataSizeInBytes,
                     pRootSignatureDeserializerInterface, ppRootSignatureDeserializer) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT NumFeatures, const IID* pIIDs, void* pConfigurationStructs, UINT* pConfigurationStructSizes)
{
    using PFN = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
    auto pfn  = GetRealProc<PFN>("D3D12EnableExperimentalFeatures");
    return pfn ? pfn(NumFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void** ppvDebug)
{
    using PFN = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12GetInterface");
    return pfn ? pfn(rclsid, riid, ppvDebug) : E_NOTIMPL;
}
