// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drop-in d3d12.dll proxy.  All exports forwarded to the real DLL via
// GetProcAddress; D3D12CreateDevice returns a WrappedDevice.
//
// CRITICAL: do NOT link d3d12.lib.  We ARE d3d12.dll.  All real-DLL calls
// go through GetRealProc<> (runtime GetProcAddress on the system32 copy).
//
// Why two DLL handles?
//   On Windows 10 20H2+ the OS split d3d12.dll into a thin shim +
//   d3d12core.dll.  OpenAdapter12, the WDDM DDI, and the Agility SDK
//   bootstrap functions moved into d3d12core.dll.  GetRealProc tries
//   system d3d12.dll first, then d3d12core.dll, so both old and new
//   Windows builds work.

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
// Real DLL handles
//   g_hRealD3D12     – system32\d3d12.dll  (always loaded)
//   g_hRealD3D12Core – system32\d3d12core.dll  (loaded when present)
// Both are exposed so the generated unknown-stubs file can use them.
// ---------------------------------------------------------------------------
HMODULE g_hRealD3D12     = nullptr;
HMODULE g_hRealD3D12Core = nullptr;

// ---------------------------------------------------------------------------
// GetRealProc – resolve a named function, trying both DLL handles
// ---------------------------------------------------------------------------
template<typename T>
static T GetRealProc(const char* name)
{
    FARPROC p = nullptr;
    if (g_hRealD3D12)     p = GetProcAddress(g_hRealD3D12,     name);
    if (!p && g_hRealD3D12Core) p = GetProcAddress(g_hRealD3D12Core, name);
    if (!p) LOG_WARN("GetRealProc: '%s' not found in real DLLs", name);
    return reinterpret_cast<T>(p);
}

// ---------------------------------------------------------------------------
// InfoQueue drain
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

        auto myDir  = GetMyDir();
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

        // --- Load system d3d12.dll ---
        wchar_t sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring sysPath = std::wstring(sysDir) + L"\\";

        std::wstring d3d12Path = sysPath + L"d3d12.dll";
        LOG_DEBUG("DllMain: loading real d3d12.dll from %ls", d3d12Path.c_str());
        g_hRealD3D12 = LoadLibraryW(d3d12Path.c_str());
        if (!g_hRealD3D12) {
            LOG_ERROR("DllMain: FATAL – could not load %ls (error %u)!",
                      d3d12Path.c_str(), GetLastError());
            return FALSE;
        }
        LOG_INFO("DllMain: loaded d3d12.dll    (handle=%p)", (void*)g_hRealD3D12);

        // --- Also load d3d12core.dll (present on Win10 20H2+) ---
        // Functions like OpenAdapter12 moved there on newer Windows.
        // Non-fatal if absent (older Windows has them in d3d12.dll itself).
        std::wstring corePath = sysPath + L"d3d12core.dll";
        g_hRealD3D12Core = LoadLibraryW(corePath.c_str());
        if (g_hRealD3D12Core)
            LOG_INFO("DllMain: loaded d3d12core.dll (handle=%p)", (void*)g_hRealD3D12Core);
        else
            LOG_DEBUG("DllMain: d3d12core.dll not found in system32 (older Windows – OK).");

        LOG_DEBUG("DllMain: init complete.");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        StereoEngine::Shutdown();
        LOG_INFO("DllMain: unloading.");
        Log::Shutdown();
        if (g_hRealD3D12Core) { FreeLibrary(g_hRealD3D12Core); g_hRealD3D12Core = nullptr; }
        if (g_hRealD3D12)     { FreeLibrary(g_hRealD3D12);     g_hRealD3D12     = nullptr; }
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
    OutputDebugStringA("[3DV12] D3D12CreateDevice\n");
    LOG_DEBUG("D3D12CreateDevice: pAdapter=%p featureLevel=0x%X",
              (void*)pAdapter, (unsigned)MinimumFeatureLevel);

    using PFN = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateDevice");
    if (!pfn) { LOG_ERROR("D3D12CreateDevice: real function not found!"); return E_NOTIMPL; }

    if (Cfg::g.enableDebugLayer) {
        using PFN_GDI = HRESULT(WINAPI*)(REFIID, void**);
        auto pfnGDI = GetRealProc<PFN_GDI>("D3D12GetDebugInterface");
        if (pfnGDI) {
            ID3D12Debug* pDbg = nullptr;
            if (SUCCEEDED(pfnGDI(IID_PPV_ARGS(&pDbg))) && pDbg) {
                pDbg->EnableDebugLayer();
                pDbg->Release();
                LOG_INFO("D3D12CreateDevice: debug layer enabled.");
            }
        }
    }

    LOG_DEBUG("D3D12CreateDevice: calling real CreateDevice...");
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = pfn(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    if (FAILED(hr) || !pRealDevice) {
        LOG_ERROR("D3D12CreateDevice: FAILED hr=0x%08X", (unsigned)hr);
        return hr;
    }
    LOG_INFO("D3D12CreateDevice: real device OK (%p)", (void*)pRealDevice);
    DrainInfoQueue(pRealDevice);

    if (Cfg::g.stereoEnabled)
        StereoEngine::Init(pRealDevice);

    auto* pWrapped = new WrappedDevice(pRealDevice);
    pRealDevice->Release();

    hr = pWrapped->QueryInterface(riid, ppDevice);
    pWrapped->Release();

    if (SUCCEEDED(hr))
        LOG_INFO("D3D12CreateDevice: returning wrapped device (%p)",
                 ppDevice ? *ppDevice : nullptr);
    else
        LOG_ERROR("D3D12CreateDevice: QI for riid FAILED hr=0x%08X", (unsigned)hr);
    return hr;
}

// ===========================================================================
// FORWARDED EXPORTS
// ===========================================================================

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug)
{
    OutputDebugStringA("[3DV12] D3D12GetDebugInterface\n");
    LOG_TRACE("D3D12GetDebugInterface");
    using PFN = HRESULT(WINAPI*)(REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12GetDebugInterface");
    return pfn ? pfn(riid, ppvDebug) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID pSrc, SIZE_T SrcSize, REFIID riid, void** ppOut)
{
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateRootSignatureDeserializer");
    return pfn ? pfn(pSrc, SrcSize, riid, ppOut) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC* pDesc, D3D_ROOT_SIGNATURE_VERSION Ver,
    ID3DBlob** ppBlob, ID3DBlob** ppErr)
{
    using PFN = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                  D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    auto pfn  = GetRealProc<PFN>("D3D12SerializeRootSignature");
    return pfn ? pfn(pDesc, Ver, ppBlob, ppErr) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pDesc,
    ID3DBlob** ppBlob, ID3DBlob** ppErr)
{
    using PFN = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                  ID3DBlob**, ID3DBlob**);
    auto pfn  = GetRealProc<PFN>("D3D12SerializeVersionedRootSignature");
    return pfn ? pfn(pDesc, ppBlob, ppErr) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
    LPCVOID pSrc, SIZE_T SrcSize, REFIID riid, void** ppOut)
{
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    auto pfn  = GetRealProc<PFN>("D3D12CreateVersionedRootSignatureDeserializer");
    return pfn ? pfn(pSrc, SrcSize, riid, ppOut) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
    UINT NumFeatures, const IID* pIIDs,
    void* pConfigStructs, UINT* pConfigSizes)
{
    using PFN = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
    auto pfn  = GetRealProc<PFN>("D3D12EnableExperimentalFeatures");
    return pfn ? pfn(NumFeatures, pIIDs, pConfigStructs, pConfigSizes) : E_NOTIMPL;
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

extern "C" HRESULT WINAPI OpenAdapter12(void* pOpenData)
{
    OutputDebugStringA("[3DV12] OpenAdapter12\n");
    LOG_TRACE("OpenAdapter12");
    using PFN = HRESULT(WINAPI*)(void*);
    auto pfn  = GetRealProc<PFN>("OpenAdapter12");
    if (!pfn) { LOG_WARN("OpenAdapter12: not found in real DLLs – returning E_NOTIMPL"); return E_NOTIMPL; }
    return pfn(pOpenData);
}

extern "C" void WINAPI SetAppCompatStringPointer(void* pData, const char* pStr)
{
    LOG_TRACE("SetAppCompatStringPointer");
    using PFN = void(WINAPI*)(void*, const char*);
    auto pfn  = GetRealProc<PFN>("SetAppCompatStringPointer");
    if (pfn) pfn(pData, pStr);
}

// ---- Agility SDK bootstrap -------------------------------------------------

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

// ---- PIX -------------------------------------------------------------------

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
