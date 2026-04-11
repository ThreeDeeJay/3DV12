// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drop-in d3d12.dll proxy.  All exports forwarded to the real DLL via
// GetProcAddress; D3D12CreateDevice returns a WrappedDevice.
//
// CRITICAL: do NOT link d3d12.lib.  We ARE d3d12.dll.  All real-DLL calls
// go through GetRealProc<> (runtime GetProcAddress on the system32 copy).
//
// Every symbol the real d3d12.dll exports must appear in d3d12.def AND be
// forwarded here.  DXGI and other system components call:
//   GetProcAddress(GetModuleHandle("d3d12.dll"), "OpenAdapter12")
// etc. at startup, before the app calls D3D12CreateDevice.  A missing export
// yields NULL and can crash the system DLL.

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
// Real d3d12.dll handle – shared with wrapped_device.cpp
// ---------------------------------------------------------------------------
HMODULE g_hRealD3D12 = nullptr;

// ---------------------------------------------------------------------------
// GetRealProc – resolve a function from the real d3d12.dll
// ---------------------------------------------------------------------------
template<typename T>
static T GetRealProc(const char* name)
{
    if (!g_hRealD3D12) {
        LOG_ERROR("GetRealProc(%s): real DLL not loaded!", name);
        return nullptr;
    }
    return reinterpret_cast<T>(GetProcAddress(g_hRealD3D12, name));
}

// ---------------------------------------------------------------------------
// InfoQueue drain – pipe D3D12 validation messages into our log
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
            LogLevel lv = LogLevel::Debug;
            if      (buf->Severity == D3D12_MESSAGE_SEVERITY_ERROR)   lv = LogLevel::Error;
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
// Helper: directory of this DLL
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
// DLL entry point
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);

        auto myDir   = GetMyDir();
        std::wstring iniPathW = myDir + L"3DV12.ini";
        std::string  iniPath(iniPathW.begin(), iniPathW.end());

        if (!Cfg::Load(iniPath, Cfg::g))
            Cfg::WriteDefaults(iniPath);

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

        wchar_t sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring realPath = std::wstring(sysDir) + L"\\d3d12.dll";
        LOG_DEBUG("DllMain: loading real DLL from %ls", realPath.c_str());

        g_hRealD3D12 = LoadLibraryW(realPath.c_str());
        if (!g_hRealD3D12) {
            LOG_ERROR("DllMain: FATAL – could not load %ls (error %u)!",
                      realPath.c_str(), GetLastError());
            return FALSE;
        }
        LOG_INFO("DllMain: loaded real d3d12.dll (handle=%p)", (void*)g_hRealD3D12);
        LOG_DEBUG("DllMain: init complete, ready to intercept.");
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

// ===========================================================================
// PRIMARY INTERCEPT – D3D12CreateDevice
// ===========================================================================
extern "C" HRESULT WINAPI D3D12CreateDevice(
    IUnknown*         pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID            riid,
    void**            ppDevice)
{
    // OutputDebugString fires even if log is somehow unavailable
    OutputDebugStringA("[3DV12] D3D12CreateDevice entered\n");
    LOG_DEBUG("D3D12CreateDevice: pAdapter=%p featureLevel=0x%X",
              (void*)pAdapter, (unsigned)MinimumFeatureLevel);

    using PFN = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateDevice");
    if (!pfn) {
        LOG_ERROR("D3D12CreateDevice: real function not found!");
        return E_NOTIMPL;
    }

    // Optional: enable D3D12 validation layer before device creation
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
                LOG_WARN("D3D12CreateDevice: GetDebugInterface failed – "
                         "debug DLLs may not be installed.");
            }
        }
    }

    LOG_DEBUG("D3D12CreateDevice: calling real CreateDevice...");
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = pfn(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    if (FAILED(hr) || !pRealDevice) {
        LOG_ERROR("D3D12CreateDevice: real device creation FAILED hr=0x%08X", (unsigned)hr);
        return hr;
    }
    LOG_INFO("D3D12CreateDevice: real device OK (%p) hr=0x%08X",
             (void*)pRealDevice, (unsigned)hr);

    DrainInfoQueue(pRealDevice);

    if (Cfg::g.stereoEnabled) {
        LOG_DEBUG("D3D12CreateDevice: initialising stereo engine...");
        StereoEngine::Init(pRealDevice);
    }

    LOG_DEBUG("D3D12CreateDevice: constructing WrappedDevice...");
    auto* pWrapped = new WrappedDevice(pRealDevice);
    pRealDevice->Release();

    hr = pWrapped->QueryInterface(riid, ppDevice);
    pWrapped->Release();

    if (SUCCEEDED(hr))
        LOG_INFO("D3D12CreateDevice: done, wrapped=%p hr=0x%08X",
                 ppDevice ? *ppDevice : nullptr, (unsigned)hr);
    else
        LOG_ERROR("D3D12CreateDevice: QI for caller riid FAILED hr=0x%08X", (unsigned)hr);

    return hr;
}

// ===========================================================================
// FORWARDED EXPORTS – all other d3d12.dll entry points
// Every function forwards to the real DLL.  Missing entries here cause
// system DLLs (dxgi, d3d12core, PIX) to get NULL from GetProcAddress and
// crash before the app reaches D3D12CreateDevice.
// ===========================================================================

// ---- Core public API -------------------------------------------------------

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug)
{
    OutputDebugStringA("[3DV12] D3D12GetDebugInterface\n");
    LOG_TRACE("D3D12GetDebugInterface");
    using PFN = HRESULT(WINAPI*)(REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12GetDebugInterface");
    return pfn ? pfn(riid, ppvDebug) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID pRSDI, void** ppRSD)
{
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateRootSignatureDeserializer");
    return pfn ? pfn(pSrcData, SrcDataSizeInBytes, pRSDI, ppRSD) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob)
{
    using PFN = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                  D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto pfn  = GetRealProc<PFN>("D3D12SerializeRootSignature");
    return pfn ? pfn(pRootSignature, Version, ppBlob, ppErrorBlob) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
    ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob)
{
    using PFN = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                  ID3DBlob**, ID3DBlob**);
    auto pfn  = GetRealProc<PFN>("D3D12SerializeVersionedRootSignature");
    return pfn ? pfn(pRootSignature, ppBlob, ppErrorBlob) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrcData, SIZE_T SrcDataSizeInBytes,
    REFIID pRSDI, void** ppRSD)
{
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateVersionedRootSignatureDeserializer");
    return pfn ? pfn(pSrcData, SrcDataSizeInBytes, pRSDI, ppRSD) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT NumFeatures, const IID* pIIDs,
    void* pConfigurationStructs, UINT* pConfigurationStructSizes)
{
    using PFN = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
    auto pfn  = GetRealProc<PFN>("D3D12EnableExperimentalFeatures");
    return pfn ? pfn(NumFeatures, pIIDs, pConfigurationStructs, pConfigurationStructSizes) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void** ppv)
{
    OutputDebugStringA("[3DV12] D3D12GetInterface\n");
    LOG_TRACE("D3D12GetInterface");
    using PFN = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12GetInterface");
    return pfn ? pfn(rclsid, riid, ppv) : E_NOTIMPL;
}

// ---- WDDM / DXGI DDI -------------------------------------------------------
// OpenAdapter12 is called by dxgi.dll during adapter enumeration via
//   GetProcAddress(GetModuleHandle("d3d12.dll"), "OpenAdapter12")
// If it returns NULL, DXGI silently disables D3D12 adapter support.
// If the table entry exists but the function crashes, the process dies before
// any app code runs.  Forward unconditionally to the real DLL.

extern "C" HRESULT WINAPI OpenAdapter12(void* pOpenData)
{
    OutputDebugStringA("[3DV12] OpenAdapter12\n");
    LOG_TRACE("OpenAdapter12");
    using PFN = HRESULT(WINAPI*)(void*);
    auto pfn  = GetRealProc<PFN>("OpenAdapter12");
    return pfn ? pfn(pOpenData) : E_NOTIMPL;
}

extern "C" void WINAPI SetAppCompatStringPointer(void* pData, const char* pStr)
{
    OutputDebugStringA("[3DV12] SetAppCompatStringPointer\n");
    LOG_TRACE("SetAppCompatStringPointer");
    using PFN = void(WINAPI*)(void*, const char*);
    auto pfn  = GetRealProc<PFN>("SetAppCompatStringPointer");
    if (pfn) pfn(pData, pStr);
}

// ---- Agility SDK bootstrap -------------------------------------------------
// These are called by the system d3d12.dll when it detects the app exports
// D3D12SDKVersion / D3D12SDKPath (DirectX Agility SDK protocol).
// They must be present so d3d12core.dll can call back into d3d12.dll.

extern "C" HRESULT WINAPI D3D12CoreCreateLayeredDevice(
    const void* pCaps, UINT CapsSize, const void* pLayerDesc,
    UINT LayerDescSize, REFIID riid, void** ppvLayer)
{
    OutputDebugStringA("[3DV12] D3D12CoreCreateLayeredDevice\n");
    LOG_TRACE("D3D12CoreCreateLayeredDevice");
    using PFN = HRESULT(WINAPI*)(const void*, UINT, const void*, UINT, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CoreCreateLayeredDevice");
    return pfn ? pfn(pCaps, CapsSize, pLayerDesc, LayerDescSize, riid, ppvLayer) : E_NOTIMPL;
}

extern "C" SIZE_T WINAPI D3D12CoreGetLayeredDeviceSize(
    const void* pCaps, UINT CapsSize)
{
    OutputDebugStringA("[3DV12] D3D12CoreGetLayeredDeviceSize\n");
    LOG_TRACE("D3D12CoreGetLayeredDeviceSize");
    using PFN = SIZE_T(WINAPI*)(const void*, UINT);
    auto pfn  = GetRealProc<PFN>("D3D12CoreGetLayeredDeviceSize");
    return pfn ? pfn(pCaps, CapsSize) : 0;
}

extern "C" HRESULT WINAPI D3D12CoreRegisterDestroyedObject(void* pObject)
{
    OutputDebugStringA("[3DV12] D3D12CoreRegisterDestroyedObject\n");
    LOG_TRACE("D3D12CoreRegisterDestroyedObject");
    using PFN = HRESULT(WINAPI*)(void*);
    auto pfn  = GetRealProc<PFN>("D3D12CoreRegisterDestroyedObject");
    return pfn ? pfn(pObject) : E_NOTIMPL;
}

// ---- PIX performance events ------------------------------------------------

extern "C" void* WINAPI D3D12PIXEventsReplaceBlock(BOOL getEarliestTime, UINT64* pBlockSize)
{
    using PFN = void*(WINAPI*)(BOOL, UINT64*);
    auto pfn  = GetRealProc<PFN>("D3D12PIXEventsReplaceBlock");
    return pfn ? pfn(getEarliestTime, pBlockSize) : nullptr;
}

extern "C" void* WINAPI D3D12PIXGetThreadInfo()
{
    using PFN = void*(WINAPI*)();
    auto pfn  = GetRealProc<PFN>("D3D12PIXGetThreadInfo");
    return pfn ? pfn() : nullptr;
}

extern "C" void WINAPI D3D12PIXNotifyWakeFromFenceSignal(HANDLE hEvent)
{
    using PFN = void(WINAPI*)(HANDLE);
    auto pfn  = GetRealProc<PFN>("D3D12PIXNotifyWakeFromFenceSignal");
    if (pfn) pfn(hEvent);
}

extern "C" void WINAPI D3D12PIXReportCounter(LPCWSTR pName, float value)
{
    using PFN = void(WINAPI*)(LPCWSTR, float);
    auto pfn  = GetRealProc<PFN>("D3D12PIXReportCounter");
    if (pfn) pfn(pName, value);
}
