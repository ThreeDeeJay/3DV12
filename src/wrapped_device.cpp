// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wrapped_device.h"
#include "wrapped_command_list.h"
#include "shader_patcher.h"
#include "stereo_engine.h"
#include "config.h"
#include "log.h"
#include <d3dcompiler.h>
#include <cstring>
#include <vector>

// g_hRealD3D12 is set in dllmain.cpp and used for RS (de)serialisation.
extern HMODULE g_hRealD3D12;

using PFN_D3D12SerializeVersionedRS   = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using PFN_D3D12DeserializeVersionedRS = HRESULT(WINAPI*)(const void*, SIZE_T, ID3D12VersionedRootSignatureDeserializer**, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC**);

static PFN_D3D12SerializeVersionedRS   s_pfnSerVRS   = nullptr;
static PFN_D3D12DeserializeVersionedRS s_pfnDeserVRS = nullptr;

static void EnsureRSFuncs()
{
    if (!s_pfnSerVRS && g_hRealD3D12) {
        s_pfnSerVRS   = (PFN_D3D12SerializeVersionedRS)  GetProcAddress(g_hRealD3D12, "D3D12SerializeVersionedRootSignature");
        s_pfnDeserVRS = (PFN_D3D12DeserializeVersionedRS)GetProcAddress(g_hRealD3D12, "D3D12DeserializeVersionedRootSignature");
    }
}

// ---------------------------------------------------------------------------
// Root-signature stereo-CBV injection
// ---------------------------------------------------------------------------
static ID3DBlob* AppendStereoParam(const void* pBlob, SIZE_T blobLen,
                                    UINT cbReg, UINT cbSpace,
                                    UINT& outStereoIdx)
{
    EnsureRSFuncs();
    if (!s_pfnSerVRS || !s_pfnDeserVRS) return nullptr;

    ID3D12VersionedRootSignatureDeserializer* pDeserial = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pDesc = nullptr;
    if (FAILED(s_pfnDeserVRS(pBlob, blobLen, &pDeserial, &pDesc)) || !pDesc) {
        if (pDeserial) pDeserial->Release();
        return nullptr;
    }

    bool isV11 = (pDesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);
    UINT origCount = isV11 ? pDesc->Desc_1_1.NumParameters
                           : pDesc->Desc_1_0.NumParameters;
    outStereoIdx = origCount;

    std::vector<D3D12_ROOT_PARAMETER1> params11;
    std::vector<D3D12_ROOT_PARAMETER>  params10;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC newDesc = *pDesc;
    if (isV11) {
        params11.assign(pDesc->Desc_1_1.pParameters,
                        pDesc->Desc_1_1.pParameters + origCount);
        D3D12_ROOT_PARAMETER1 sp{};
        sp.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
        sp.Descriptor.ShaderRegister           = cbReg;
        sp.Descriptor.RegisterSpace            = cbSpace;
        sp.Descriptor.Flags                    = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        sp.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        params11.push_back(sp);
        newDesc.Desc_1_1.NumParameters         = (UINT)params11.size();
        newDesc.Desc_1_1.pParameters           = params11.data();
    } else {
        params10.assign(pDesc->Desc_1_0.pParameters,
                        pDesc->Desc_1_0.pParameters + origCount);
        D3D12_ROOT_PARAMETER sp{};
        sp.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
        sp.Descriptor.ShaderRegister           = cbReg;
        sp.Descriptor.RegisterSpace            = cbSpace;
        sp.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        params10.push_back(sp);
        newDesc.Desc_1_0.NumParameters         = (UINT)params10.size();
        newDesc.Desc_1_0.pParameters           = params10.data();
    }

    ID3DBlob* pOut = nullptr;
    ID3DBlob* pErr = nullptr;
    s_pfnSerVRS(&newDesc, &pOut, &pErr);
    if (pErr) pErr->Release();
    pDeserial->Release();
    return pOut;
}

// ===========================================================================
// Construction / destruction
// ===========================================================================
WrappedDevice::WrappedDevice(ID3D12Device* pReal)
    : m_pReal(pReal), m_refCount(1)
{
    m_pReal->AddRef();
    CreateStereoParamBuffer();
    LOG_INFO("WrappedDevice created (real=%p)", (void*)pReal);
}

WrappedDevice::~WrappedDevice()
{
    if (m_pStereoParamBuf) {
        if (m_pStereoMapped) m_pStereoParamBuf->Unmap(0, nullptr);
        m_pStereoParamBuf->Release();
    }
    m_pReal->Release();
}

HRESULT WrappedDevice::CreateStereoParamBuffer()
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = 256;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_pReal->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_pStereoParamBuf));
    if (FAILED(hr)) { LOG_ERROR("Stereo param buffer creation failed: 0x%08X", hr); return hr; }
    m_pStereoParamBuf->Map(0, nullptr, &m_pStereoMapped);
    return S_OK;
}

void WrappedDevice::UpdateStereoParamBuffer(StereoParams* pParams)
{
    if (m_pStereoMapped && pParams)
        memcpy(m_pStereoMapped, pParams, sizeof(StereoParams));
}

ID3D12Resource* WrappedDevice::GetStereoParamBuffer() { return m_pStereoParamBuf; }

void WrappedDevice::TrackRSMeta(ID3D12RootSignature* pRS, const RSMeta& m)
{
    std::lock_guard<std::mutex> lk(m_rsMtx);
    m_rsMeta[pRS] = m;
}

bool WrappedDevice::GetRSMeta(ID3D12RootSignature* pRS, RSMeta& out)
{
    std::lock_guard<std::mutex> lk(m_rsMtx);
    auto it = m_rsMeta.find(pRS);
    if (it == m_rsMeta.end()) return false;
    out = it->second;
    return true;
}

HRESULT WrappedDevice::WrapCommandList(void** ppCmdList)
{
    if (!ppCmdList || !*ppCmdList) return E_POINTER;
    auto* pRaw     = static_cast<ID3D12GraphicsCommandList*>(*ppCmdList);
    auto* pWrapped = new WrappedCommandList(pRaw, this);
    pRaw->Release(); // WrappedCommandList took its own AddRef
    *ppCmdList = pWrapped;
    return S_OK;
}

// ===========================================================================
// IUnknown
// ===========================================================================
HRESULT WrappedDevice::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    // Interfaces we implement ourselves
    if (riid == __uuidof(IUnknown)      ||
        riid == __uuidof(ID3D12Object)  ||
        riid == __uuidof(ID3D12Device))
    {
        *ppvObj = static_cast<ID3D12Device*>(this);
        AddRef();
        return S_OK;
    }

    // For Device1..DeviceN and everything else, forward to the real device.
    // The real device's Device1+ pointer is still valid; calls to Device0
    // methods through that pointer still hit our vtable via normal COM QI
    // rules only when the app went through us first.
    // NOTE: If the app holds the real Device1+ pointer and calls
    //       CreateGraphicsPipelineState through it, we won't intercept that
    //       call, but in practice apps use the pointer returned by
    //       D3D12CreateDevice (which is ours) and QI from there.
    HRESULT hr = m_pReal->QueryInterface(riid, ppvObj);
    if (SUCCEEDED(hr))
        LOG_TRACE("WrappedDevice::QI forwarded to real device for unknown IID");
    return hr;
}

ULONG WrappedDevice::AddRef()  { return ++m_refCount; }
ULONG WrappedDevice::Release()
{
    ULONG c = --m_refCount;
    if (c == 0) delete this;
    return c;
}

// ===========================================================================
// ID3D12Object
// ===========================================================================
HRESULT WrappedDevice::GetPrivateData(REFGUID g, UINT* s, void* d)           { return m_pReal->GetPrivateData(g,s,d); }
HRESULT WrappedDevice::SetPrivateData(REFGUID g, UINT s, const void* d)      { return m_pReal->SetPrivateData(g,s,d); }
HRESULT WrappedDevice::SetPrivateDataInterface(REFGUID g, const IUnknown* p) { return m_pReal->SetPrivateDataInterface(g,p); }
HRESULT WrappedDevice::SetName(LPCWSTR n)                                    { return m_pReal->SetName(n); }

// ===========================================================================
// ID3D12Device – pass-throughs (non-intercepted)
// ===========================================================================
UINT    WrappedDevice::GetNodeCount()                                                { return m_pReal->GetNodeCount(); }
HRESULT WrappedDevice::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* d, REFIID r, void** p) { return m_pReal->CreateCommandQueue(d,r,p); }
HRESULT WrappedDevice::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE t, REFIID r, void** p)     { return m_pReal->CreateCommandAllocator(t,r,p); }
HRESULT WrappedDevice::CheckFeatureSupport(D3D12_FEATURE f, void* d, UINT s)                     { return m_pReal->CheckFeatureSupport(f,d,s); }
HRESULT WrappedDevice::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID r, void** p) { return m_pReal->CreateDescriptorHeap(d,r,p); }
UINT    WrappedDevice::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE t)            { return m_pReal->GetDescriptorHandleIncrementSize(t); }
void    WrappedDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { m_pReal->CreateConstantBufferView(d,h); }
void    WrappedDevice::CreateShaderResourceView(ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { m_pReal->CreateShaderResourceView(r,d,h); }
void    WrappedDevice::CreateUnorderedAccessView(ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { m_pReal->CreateUnorderedAccessView(r,c,d,h); }
void    WrappedDevice::CreateRenderTargetView(ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)    { m_pReal->CreateRenderTargetView(r,d,h); }
void    WrappedDevice::CreateDepthStencilView(ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)    { m_pReal->CreateDepthStencilView(r,d,h); }
void    WrappedDevice::CreateSampler(const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)  { m_pReal->CreateSampler(d,h); }
void    WrappedDevice::CopyDescriptors(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* dp, const UINT* dr, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* sp, const UINT* sr, D3D12_DESCRIPTOR_HEAP_TYPE t) { m_pReal->CopyDescriptors(nd,dp,dr,ns,sp,sr,t); }
void    WrappedDevice::CopyDescriptorsSimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE t) { m_pReal->CopyDescriptorsSimple(n,d,s,t); }
D3D12_RESOURCE_ALLOCATION_INFO WrappedDevice::GetResourceAllocationInfo(UINT v, UINT n, const D3D12_RESOURCE_DESC* d) { return m_pReal->GetResourceAllocationInfo(v,n,d); }
D3D12_HEAP_PROPERTIES WrappedDevice::GetCustomHeapProperties(UINT m, D3D12_HEAP_TYPE t)          { return m_pReal->GetCustomHeapProperties(m,t); }
HRESULT WrappedDevice::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* h, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID r, void** p) { return m_pReal->CreateCommittedResource(h,f,d,s,c,r,p); }
HRESULT WrappedDevice::CreateHeap(const D3D12_HEAP_DESC* d, REFIID r, void** p)                  { return m_pReal->CreateHeap(d,r,p); }
HRESULT WrappedDevice::CreatePlacedResource(ID3D12Heap* h, UINT64 o, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID r, void** p) { return m_pReal->CreatePlacedResource(h,o,d,s,c,r,p); }
HRESULT WrappedDevice::CreateReservedResource(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID r, void** p) { return m_pReal->CreateReservedResource(d,s,c,r,p); }
HRESULT WrappedDevice::CreateSharedHandle(ID3D12DeviceChild* o, const SECURITY_ATTRIBUTES* a, DWORD acc, LPCWSTR n, HANDLE* h) { return m_pReal->CreateSharedHandle(o,a,acc,n,h); }
HRESULT WrappedDevice::OpenSharedHandle(HANDLE h, REFIID r, void** p)                            { return m_pReal->OpenSharedHandle(h,r,p); }
HRESULT WrappedDevice::OpenSharedHandleByName(LPCWSTR n, DWORD a, HANDLE* h)                     { return m_pReal->OpenSharedHandleByName(n,a,h); }
HRESULT WrappedDevice::MakeResident(UINT n, ID3D12Pageable* const* p)                            { return m_pReal->MakeResident(n,p); }
HRESULT WrappedDevice::Evict(UINT n, ID3D12Pageable* const* p)                                   { return m_pReal->Evict(n,p); }
HRESULT WrappedDevice::CreateFence(UINT64 v, D3D12_FENCE_FLAGS f, REFIID r, void** p)            { return m_pReal->CreateFence(v,f,r,p); }
HRESULT WrappedDevice::GetDeviceRemovedReason()                                                   { return m_pReal->GetDeviceRemovedReason(); }
void    WrappedDevice::GetCopyableFootprints(const D3D12_RESOURCE_DESC* d, UINT fi, UINT nl, UINT64 bo, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, UINT* rn, UINT64* rs, UINT64* ts) { m_pReal->GetCopyableFootprints(d,fi,nl,bo,fp,rn,rs,ts); }
HRESULT WrappedDevice::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* d, REFIID r, void** p)       { return m_pReal->CreateQueryHeap(d,r,p); }
HRESULT WrappedDevice::SetStablePowerState(BOOL b)                                               { return m_pReal->SetStablePowerState(b); }
HRESULT WrappedDevice::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* r, REFIID ri, void** p) { return m_pReal->CreateCommandSignature(d,r,ri,p); }
void    WrappedDevice::GetResourceTiling(ID3D12Resource* r, UINT* ts, D3D12_PACKED_MIP_INFO* pm, D3D12_TILE_SHAPE* st, UINT* snts, UINT fi, D3D12_SUBRESOURCE_TILING* t) { m_pReal->GetResourceTiling(r,ts,pm,st,snts,fi,t); }
LUID    WrappedDevice::GetAdapterLuid()                                                           { return m_pReal->GetAdapterLuid(); }

// ===========================================================================
// KEY INTERCEPTS
// ===========================================================================

HRESULT WrappedDevice::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPSO)
{
    if (!Cfg::g.stereoEnabled || !pDesc)
        return m_pReal->CreateGraphicsPipelineState(pDesc, riid, ppPSO);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = *pDesc;
    ShaderPatcher::PatchedPSO storage;
    bool patched = ShaderPatcher::PatchGraphicsPSO(desc, storage,
                                                    (uint32_t)Cfg::g.stereoConstantBuffer);
    if (patched) LOG_DEBUG("CreateGraphicsPSO: shader(s) patched.");
    return m_pReal->CreateGraphicsPipelineState(&desc, riid, ppPSO);
}

HRESULT WrappedDevice::CreateComputePipelineState(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPSO)
{
    if (!Cfg::g.stereoEnabled || !pDesc)
        return m_pReal->CreateComputePipelineState(pDesc, riid, ppPSO);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = *pDesc;
    ShaderPatcher::PatchedCSPSO storage;
    bool patched = ShaderPatcher::PatchComputePSO(desc, storage,
                                                   (uint32_t)Cfg::g.stereoConstantBuffer);
    if (patched) LOG_DEBUG("CreateComputePSO: shader patched.");
    return m_pReal->CreateComputePipelineState(&desc, riid, ppPSO);
}

HRESULT WrappedDevice::CreateRootSignature(
    UINT nodeMask, const void* pBlob, SIZE_T blobLen, REFIID riid, void** ppRS)
{
    if (!Cfg::g.stereoEnabled)
        return m_pReal->CreateRootSignature(nodeMask, pBlob, blobLen, riid, ppRS);

    UINT stereoIdx = 0;
    ID3DBlob* pNew = AppendStereoParam(pBlob, blobLen,
                                        (UINT)Cfg::g.stereoConstantBuffer, 13u,
                                        stereoIdx);
    HRESULT hr;
    if (pNew) {
        hr = m_pReal->CreateRootSignature(nodeMask,
                                          pNew->GetBufferPointer(),
                                          pNew->GetBufferSize(), riid, ppRS);
        if (SUCCEEDED(hr) && ppRS && *ppRS) {
            RSMeta meta{};
            meta.stereoParamIdx = stereoIdx;
            TrackRSMeta(static_cast<ID3D12RootSignature*>(*ppRS), meta);
            LOG_DEBUG("CreateRootSignature: stereo param appended at slot %u", stereoIdx);
        }
        pNew->Release();
    } else {
        LOG_WARN("CreateRootSignature: could not append stereo param – using original.");
        hr = m_pReal->CreateRootSignature(nodeMask, pBlob, blobLen, riid, ppRS);
    }
    return hr;
}

HRESULT WrappedDevice::CreateCommandList(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator* pAlloc, ID3D12PipelineState* pPSO,
    REFIID riid, void** ppCmdList)
{
    HRESULT hr = m_pReal->CreateCommandList(nodeMask, type, pAlloc, pPSO,
                                             IID_PPV_ARGS((ID3D12GraphicsCommandList**)ppCmdList));
    if (SUCCEEDED(hr)) WrapCommandList(ppCmdList);
    return hr;
}
