// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drop-in d3d12.dll proxy.  All exports forwarded to the real DLL via
// GetProcAddress; D3D12CreateDevice returns a WrappedDevice.
//
// NOTE: do NOT link against d3d12.lib.  We ARE d3d12.dll; a self-referential
// import entry in our own IAT causes Windows loader instability.  All real-DLL
// calls go through GetRealProc<> (GetProcAddress on the system32 copy).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d12.h>
#include <string>
#include <cstdio>

#include "version.h"
#include "log.h"
#include "config.h"
#include "wrapped_device.h"
#include "stereo_engine.h"

// ---------------------------------------------------------------------------
// Real d3d12.dll handle – used by wrapped_device.cpp for RS (de)serialisation
// ---------------------------------------------------------------------------
HMODULE g_hRealD3D12 = nullptr;

// ---------------------------------------------------------------------------
// InfoQueue: drain all pending D3D12 validation messages into our log.
// Called from D3D12CreateDevice and optionally from wrapped hooks.
// ---------------------------------------------------------------------------
void DrainInfoQueue(ID3D12Device* pDevice)
{
    if (!pDevice || !Cfg::g.drainInfoQueue) return;

    ID3D12InfoQueue* pIQ = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pIQ))) || !pIQ) return;

    UINT64 n = pIQ->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        if (FAILED(pIQ->GetMessageA(i, nullptr, &len)) || len == 0) continue;

        auto* buf = static_cast<D3D12_MESSAGE*>(HeapAlloc(GetProcessHeap(), 0, len));
        if (!buf) continue;
        if (SUCCEEDED(pIQ->GetMessageA(i, buf, &len))) {
            // Map D3D12 severity to our log level
            LogLevel lv = LogLevel::Debug;
            if (buf->Severity == D3D12_MESSAGE_SEVERITY_ERROR)        lv = LogLevel::Error;
            else if (buf->Severity == D3D12_MESSAGE_SEVERITY_WARNING) lv = LogLevel::Warn;
            else if (buf->Severity == D3D12_MESSAGE_SEVERITY_INFO)    lv = LogLevel::Info;
            Log::Write(lv, "[D3D12] %s", buf->pDescription ? buf->pDescription : "(null)");
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    pIQ->ClearStoredMessages();
    pIQ->Release();
}

// ---------------------------------------------------------------------------
// Helper: resolve directory of this DLL
// ---------------------------------------------------------------------------
static std::wstring GetMyDir()
{
    wchar_t buf[MAX_PATH] = {};
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetMyDir), &hSelf);
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

        // --- Determine INI path (narrow conversion – ASCII paths only) ---
        auto myDir  = GetMyDir();
        std::wstring iniPathW = myDir + L"3DV12.ini";
        std::string  iniPath(iniPathW.begin(), iniPathW.end());

        // --- Load config ---
        if (!Cfg::Load(iniPath, Cfg::g))
            Cfg::WriteDefaults(iniPath);

        // --- Start logging ---
        std::string logPath = Cfg::g.logFile;
        if (logPath.find(':') == std::string::npos) {
            std::string myDirA(myDir.begin(), myDir.end());
            logPath = myDirA + logPath;
        }
        Log::Init(logPath, Log::ParseLevel(Cfg::g.logLevel));
        LOG_INFO("DllMain: " PROJECT_FULL " loading (version int %d).", VERSION);
        LOG_INFO("INI: %s", iniPath.c_str());
        LOG_DEBUG("DllMain: stereoEnabled=%d logLevel=%s dumpShaders=%d "
                  "enableDebugLayer=%d drainInfoQueue=%d",
                  (int)Cfg::g.stereoEnabled, Cfg::g.logLevel.c_str(),
                  (int)Cfg::g.dumpShaders,
                  (int)Cfg::g.enableDebugLayer, (int)Cfg::g.drainInfoQueue);

        // --- Load real d3d12.dll explicitly from system32 ---
        // We MUST NOT link against d3d12.lib (we ARE d3d12.dll).
        wchar_t sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring realPath = std::wstring(sysDir) + L"\\d3d12.dll";
        LOG_DEBUG("DllMain: loading real DLL from %ls", realPath.c_str());
        g_hRealD3D12 = LoadLibraryW(realPath.c_str());
        if (!g_hRealD3D12) {
            LOG_ERROR("DllMain: FATAL – could not load real d3d12.dll from %ls (error %u)!",
                      realPath.c_str(), GetLastError());
            return FALSE;
        }
        LOG_INFO("DllMain: loaded real d3d12.dll from %ls", realPath.c_str());
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        StereoEngine::Shutdown();
        LOG_INFO("DllMain: unloading.");
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
    if (!g_hRealD3D12) { LOG_ERROR("GetRealProc(%s): g_hRealD3D12 is null!", name); return nullptr; }
    auto* p = reinterpret_cast<T>(GetProcAddress(g_hRealD3D12, name));
    if (!p) LOG_WARN("GetRealProc: '%s' not found in real d3d12.dll", name);
    return p;
}

// ---------------------------------------------------------------------------
// D3D12CreateDevice – primary intercept
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI D3D12CreateDevice(
    IUnknown*         pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID            riid,
    void**            ppDevice)
{
    LOG_DEBUG("D3D12CreateDevice: entry pAdapter=%p featureLevel=0x%X ppDevice=%p",
              (void*)pAdapter, (unsigned)MinimumFeatureLevel, (void*)ppDevice);

    using PFN = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateDevice");
    if (!pfn) { LOG_ERROR("D3D12CreateDevice: real function not found!"); return E_NOTIMPL; }

    // Optionally enable the D3D12 debug/validation layer
    if (Cfg::g.enableDebugLayer) {
        LOG_INFO("D3D12CreateDevice: enabling debug layer.");
        using PFN_GDI = HRESULT(WINAPI*)(REFIID, void**);
        auto pfnGDI = GetRealProc<PFN_GDI>("D3D12GetDebugInterface");
        if (pfnGDI) {
            ID3D12Debug* pDbg = nullptr;
            if (SUCCEEDED(pfnGDI(IID_PPV_ARGS(&pDbg))) && pDbg) {
                pDbg->EnableDebugLayer();
                pDbg->Release();
                LOG_INFO("D3D12CreateDevice: debug layer enabled.");
            } else {
                LOG_WARN("D3D12CreateDevice: D3D12GetDebugInterface failed – "
                         "debug DLLs may not be installed.");
            }
        }
    }

    // Create the real base device (always IID_ID3D12Device so we can wrap it)
    LOG_DEBUG("D3D12CreateDevice: calling real D3D12CreateDevice...");
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = pfn(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    if (FAILED(hr) || !pRealDevice) {
        LOG_ERROR("D3D12CreateDevice: real device creation FAILED hr=0x%08X", (unsigned)hr);
        return hr;
    }
    LOG_INFO("D3D12CreateDevice: real device created (%p) hr=0x%08X", (void*)pRealDevice, (unsigned)hr);

    // Drain any validation messages produced during device creation
    DrainInfoQueue(pRealDevice);

    // Initialise NVAPI stereo from the real device
    if (Cfg::g.stereoEnabled) {
        LOG_DEBUG("D3D12CreateDevice: initialising NVAPI stereo...");
        StereoEngine::Init(pRealDevice);
    }

    // Wrap the real device; caller gets our wrapper via QI below
    LOG_DEBUG("D3D12CreateDevice: creating WrappedDevice...");
    auto* pWrapped = new WrappedDevice(pRealDevice);
    pRealDevice->Release(); // WrappedDevice holds the only remaining ref

    // Return the interface the caller requested.
    // Our QI returns 'this' for IID_ID3D12Device and forwards to the real
    // device for Device1+.  Either way, all Device0 vtable slots (which
    // contain our intercepts) are the same in every derived version.
    hr = pWrapped->QueryInterface(riid, ppDevice);
    pWrapped->Release();

    if (SUCCEEDED(hr))
        LOG_INFO("D3D12CreateDevice: returning wrapped device (%p) hr=0x%08X",
                 ppDevice ? *ppDevice : nullptr, (unsigned)hr);
    else
        LOG_ERROR("D3D12CreateDevice: QueryInterface for requested riid FAILED hr=0x%08X", (unsigned)hr);

    return hr;
}

// ---------------------------------------------------------------------------
// Forwarded exports
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug)
{
    LOG_TRACE("D3D12GetDebugInterface");
    using PFN = HRESULT(WINAPI*)(REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12GetDebugInterface");
    return pfn ? pfn(riid, ppvDebug) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID pRootSignatureDeserializerInterface, void** ppRootSignatureDeserializer)
{
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateRootSignatureDeserializer");
    return pfn ? pfn(pSrcData, SrcDataSizeInBytes,
                     pRootSignatureDeserializerInterface, ppRootSignatureDeserializer) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
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
    UINT NumFeatures, const IID* pIIDs,
    void* pConfigurationStructs, UINT* pConfigurationStructSizes)
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