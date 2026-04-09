// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "stereo_engine.h"
#include "log.h"
#include <Windows.h>

// ---------------------------------------------------------------------------
// NVAPI function pointer typedefs (runtime loaded)
// We declare only the subset we use.
// ---------------------------------------------------------------------------
using PFN_NvAPI_Initialize                       = NvAPI_Status (*)();
using PFN_NvAPI_Unload                           = NvAPI_Status (*)();
using PFN_NvAPI_Stereo_CreateHandleFromIUnknown  = NvAPI_Status (*)(IUnknown*, void**);
using PFN_NvAPI_Stereo_DestroyHandle             = NvAPI_Status (*)(void*);
using PFN_NvAPI_Stereo_IsEnabled                 = NvAPI_Status (*)(NvU32*);
using PFN_NvAPI_Stereo_IsActivated               = NvAPI_Status (*)(void*, NvU32*);
using PFN_NvAPI_Stereo_GetSeparation             = NvAPI_Status (*)(void*, float*);
using PFN_NvAPI_Stereo_SetSeparation             = NvAPI_Status (*)(void*, float);
using PFN_NvAPI_Stereo_GetConvergence            = NvAPI_Status (*)(void*, float*);
using PFN_NvAPI_Stereo_SetActiveEye              = NvAPI_Status (*)(void*, NV_STEREO_ACTIVE_EYE);

// NVAPI QueryInterface – all public functions are retrieved this way.
using PFN_nvapi_QueryInterface = void* (*)(unsigned int functionId);

// Known function IDs (from nvapi_lite_common.h and community RE)
#define NVAPI_ID_Initialize                       0x0150E828u
#define NVAPI_ID_Unload                           0xD22BDD7Eu
#define NVAPI_ID_Stereo_CreateHandleFromIUnknown  0xAC7E37F4u
#define NVAPI_ID_Stereo_DestroyHandle             0x3A153134u
#define NVAPI_ID_Stereo_IsEnabled                 0x348FF8E1u
#define NVAPI_ID_Stereo_IsActivated               0x1FB0BC30u
#define NVAPI_ID_Stereo_GetSeparation             0x451F2134u
#define NVAPI_ID_Stereo_SetSeparation             0x5C069FA3u
#define NVAPI_ID_Stereo_GetConvergence            0x4AB00934u
#define NVAPI_ID_Stereo_SetActiveEye              0x96EEA9F8u

// ---------------------------------------------------------------------------
static HMODULE                                s_hNvapi      = nullptr;
static PFN_nvapi_QueryInterface               s_QueryIface  = nullptr;
static void*                                  s_hStereo     = nullptr;
static bool                                   s_initialized = false;

// Helper to retrieve a function from NVAPI QueryInterface
template<typename T>
static T NvapiFunc(unsigned int id) {
    if (!s_QueryIface) return nullptr;
    return reinterpret_cast<T>(s_QueryIface(id));
}

// ---------------------------------------------------------------------------
namespace StereoEngine {

bool Init(ID3D12Device* pDevice)
{
    s_initialized = false;

#ifdef _WIN64
    s_hNvapi = LoadLibraryW(L"nvapi64.dll");
#else
    s_hNvapi = LoadLibraryW(L"nvapi.dll");
#endif
    if (!s_hNvapi) {
        LOG_INFO("StereoEngine: nvapi not found – stereo disabled.");
        return false;
    }

    s_QueryIface = reinterpret_cast<PFN_nvapi_QueryInterface>(
        GetProcAddress(s_hNvapi, "nvapi_QueryInterface"));
    if (!s_QueryIface) {
        LOG_ERROR("StereoEngine: nvapi_QueryInterface not found.");
        FreeLibrary(s_hNvapi);
        s_hNvapi = nullptr;
        return false;
    }

    auto pfnInit = NvapiFunc<PFN_NvAPI_Initialize>(NVAPI_ID_Initialize);
    if (!pfnInit || pfnInit() != NVAPI_OK) {
        LOG_WARN("StereoEngine: NvAPI_Initialize failed.");
        return false;
    }

    // Check if stereo is globally enabled in the driver
    auto pfnIsEnabled = NvapiFunc<PFN_NvAPI_Stereo_IsEnabled>(NVAPI_ID_Stereo_IsEnabled);
    NvU32 enabled = 0;
    if (pfnIsEnabled) pfnIsEnabled(&enabled);
    if (!enabled) {
        LOG_INFO("StereoEngine: 3D Vision stereo is disabled in the driver.");
        // Don't return false – we can still inject params (they'll be zero)
    }

    // Create stereo handle from D3D12 device (IUnknown*)
    auto pfnCreate = NvapiFunc<PFN_NvAPI_Stereo_CreateHandleFromIUnknown>(
        NVAPI_ID_Stereo_CreateHandleFromIUnknown);
    if (!pfnCreate) {
        LOG_WARN("StereoEngine: Stereo_CreateHandleFromIUnknown not available.");
        return false;
    }

    NvAPI_Status status = pfnCreate(pDevice, &s_hStereo);
    if (status != NVAPI_OK || !s_hStereo) {
        LOG_WARN("StereoEngine: CreateHandleFromIUnknown returned status %d.", status);
        return false;
    }

    s_initialized = true;
    LOG_INFO("StereoEngine: Initialized OK (stereo %s).", enabled ? "ENABLED" : "INACTIVE");
    return true;
}

void Shutdown()
{
    if (s_hStereo) {
        auto pfnDestroy = NvapiFunc<PFN_NvAPI_Stereo_DestroyHandle>(NVAPI_ID_Stereo_DestroyHandle);
        if (pfnDestroy) pfnDestroy(s_hStereo);
        s_hStereo = nullptr;
    }
    if (s_hNvapi) {
        auto pfnUnload = NvapiFunc<PFN_NvAPI_Unload>(NVAPI_ID_Unload);
        if (pfnUnload) pfnUnload();
        FreeLibrary(s_hNvapi);
        s_hNvapi = nullptr;
    }
    s_initialized = false;
}

bool IsActive()
{
    if (!s_initialized || !s_hStereo) return false;
    auto pfnIsAct = NvapiFunc<PFN_NvAPI_Stereo_IsActivated>(NVAPI_ID_Stereo_IsActivated);
    if (!pfnIsAct) return false;
    NvU32 active = 0;
    pfnIsAct(s_hStereo, &active);
    return active != 0;
}

StereoParams QueryParams(NV_STEREO_ACTIVE_EYE eye,
                         float separationScale,
                         float convergenceScale)
{
    StereoParams p{};
    p.eyeSign  = (eye == NVAPI_STEREO_EYE_LEFT) ? -0.5f : 0.5f;
    p.padding  = 0.0f;

    if (!s_initialized || !s_hStereo) return p;

    auto pfnSep = NvapiFunc<PFN_NvAPI_Stereo_GetSeparation>(NVAPI_ID_Stereo_GetSeparation);
    auto pfnCon = NvapiFunc<PFN_NvAPI_Stereo_GetConvergence>(NVAPI_ID_Stereo_GetConvergence);

    float sep = 0.0f, con = 1.0f;
    if (pfnSep) pfnSep(s_hStereo, &sep);
    if (pfnCon) pfnCon(s_hStereo, &con);

    // NVAPI separation is 0–100; convert to a world-unit offset factor.
    // The formula sep * 0.01f * separationScale gives a fraction of IPD.
    p.separation  = sep  * 0.01f * separationScale;
    p.convergence = con  * convergenceScale;
    return p;
}

bool SetEye(NV_STEREO_ACTIVE_EYE eye)
{
    if (!s_initialized || !s_hStereo) return false;
    auto pfnSetEye = NvapiFunc<PFN_NvAPI_Stereo_SetActiveEye>(NVAPI_ID_Stereo_SetActiveEye);
    if (!pfnSetEye) return false;
    return pfnSetEye(s_hStereo, eye) == NVAPI_OK;
}

} // namespace StereoEngine
