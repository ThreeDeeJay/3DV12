// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wrapped_device.h"
#include "wrapped_command_list.h"
#include "shader_patcher.h"
#include "stereo_engine.h"
#include "config.h"
#include "log.h"
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Root-signature patching helpers
// These functions deserialise a root signature, append a stereo CBV parameter
// (register b<N>, space 13), then re-serialise.
// We use D3D12SerializeVersionedRootSignature / DeserializeVersionedRootSignature
// from the real d3d12.dll, obtained via the global real-DLL handle.
// ---------------------------------------------------------------------------
extern HMODULE g_hRealD3D12; // set in dllmain.cpp

using PFN_D3D12SerializeVersionedRS     = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using PFN_D3D12DeserializeVersionedRS   = HRESULT(WINAPI*)(const void*, SIZE_T, ID3D12VersionedRootSignatureDeserializer**, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC**);

static PFN_D3D12SerializeVersionedRS   pfnSerializeVRS   = nullptr;
static PFN_D3D12DeserializeVersionedRS pfnDeserializeVRS = nullptr;

static void EnsureRSFunctions()
{
    if (!pfnSerializeVRS && g_hRealD3D12) {
        pfnSerializeVRS   = (PFN_D3D12SerializeVersionedRS)  GetProcAddress(g_hRealD3D12, "D3D12SerializeVersionedRootSignature");
        pfnDeserializeVRS = (PFN_D3D12DeserializeVersionedRS)GetProcAddress(g_hRealD3D12, "D3D12DeserializeVersionedRootSignature");
    }
}

// Attempt to add a stereo CBV root descriptor to the root signature.
// Returns a new serialised blob, or nullptr on failure.
static ID3DBlob* AppendStereoParam(const void* pOrigBlob, SIZE_T origSize,
                                    UINT cbRegister, UINT cbSlotSpace,
                                    UINT& outStereoParamIndex)
{
    EnsureRSFunctions();
    if (!pfnSerializeVRS || !pfnDeserializeVRS) return nullptr;

    ID3D12VersionedRootSignatureDeserializer* pDeserial = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pDesc    = nullptr;

    if (FAILED(pfnDeserializeVRS(pOrigBlob, origSize, &pDeserial, &pDesc))) return nullptr;
    if (!pDesc) { pDeserial->Release(); return nullptr; }

    // Work with v1.0 or v1.1
    bool isV11 = (pDesc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);

    UINT origParamCount = 0;
    if (isV11)      origParamCount = pDesc->Desc_1_1.NumParameters;
    else            origParamCount = pDesc->Desc_1_0.NumParameters;

    // Build new parameter array with stereo slot appended
    std::vector<D3D12_ROOT_PARAMETER1> params11;
    std::vector<D3D12_ROOT_PARAMETER>  params10;

    if (isV11) {
        params11.assign(pDesc->Desc_1_1.pParameters,
                        pDesc->Desc_1_1.pParameters + origParamCount);
        D3D12_ROOT_PARAMETER1 stereoParam{};
        stereoParam.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        stereoParam.Descriptor.ShaderRegister = cbRegister;
        stereoParam.Descriptor.RegisterSpace  = cbSlotSpace;
        stereoParam.Descriptor.Flags          = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        stereoParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params11.push_back(stereoParam);
    } else {
        params10.assign(pDesc->Desc_1_0.pParameters,
                        pDesc->Desc_1_0.pParameters + origParamCount);
        D3D12_ROOT_PARAMETER stereoParam{};
        stereoParam.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        stereoParam.Descriptor.ShaderRegister = cbRegister;
        stereoParam.Descriptor.RegisterSpace  = cbSlotSpace;
        stereoParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params10.push_back(stereoParam);
    }

    outStereoParamIndex = origParamCount; // appended at end

    // Construct the new versioned descriptor
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC newDesc{};
    newDesc.Version = pDesc->Version;
    if (isV11) {
        newDesc.Desc_1_1 = pDesc->Desc_1_1;
        newDesc.Desc_1_1.NumParameters  = (UINT)params11.size();
        newDesc.Desc_1_1.pParameters    = params11.data();
    } else {
        newDesc.Desc_1_0 = pDesc->Desc_1_0;
        newDesc.Desc_1_0.NumParameters  = (UINT)params10.size();
        newDesc.Desc_1_0.pParameters    = params10.data();
    }

    ID3DBlob* pBlob = nullptr;
    ID3DBlob* pErr  = nullptr;
    pfnSerializeVRS(&newDesc, &pBlob, &pErr);
    if (pErr) pErr->Release();
    pDeserial->Release();
    return pBlob;
}

// ---------------------------------------------------------------------------
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
    // Allocate an UPLOAD heap buffer large enough for one StereoParams (16 bytes).
    // We keep it persistently mapped for CPU writes.
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width              = 256; // minimum CB alignment
    rd.Height             = 1;
    rd.DepthOrArraySize   = 1;
    rd.MipLevels          = 1;
    rd.SampleDesc.Count   = 1;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = m_pReal->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr,
                                                   IID_PPV_ARGS(&m_pStereoParamBuf));
    if (FAILED(hr)) { LOG_ERROR("Failed to create stereo param buffer: 0x%08X", hr); return hr; }
    m_pStereoParamBuf->Map(0, nullptr, &m_pStereoMapped);
    return S_OK;
}

void WrappedDevice::UpdateStereoParamBuffer(StereoParams* pParams)
{
    if (m_pStereoMapped && pParams)
        memcpy(m_pStereoMapped, pParams, sizeof(StereoParams));
}

ID3D12Resource* WrappedDevice::GetStereoParamBuffer()
{
    return m_pStereoParamBuf;
}

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

HRESULT WrappedDevice::WrapCommandList(REFIID riid, void** ppCmdList)
{
    // ppCmdList points to a raw ID3D12GraphicsCommandList*; wrap it.
    if (!ppCmdList || !*ppCmdList) return E_POINTER;
    auto* pRawList = static_cast<ID3D12GraphicsCommandList*>(*ppCmdList);
    auto* pWrapped = new WrappedCommandList(pRawList, this);
    pRawList->Release(); // WrappedCommandList took its own ref
    *ppCmdList = pWrapped;
    return S_OK;
}

// ===========================================================================
// IUnknown
// ===========================================================================
HRESULT WrappedDevice::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown)       ||
        riid == __uuidof(ID3D12Object)   ||
        riid == __uuidof(ID3D12Device)   ||
        riid == __uuidof(ID3D12Device1)  ||
        riid == __uuidof(ID3D12Device2)  ||
        riid == __uuidof(ID3D12Device3)  ||
        riid == __uuidof(ID3D12Device4)  ||
        riid == __uuidof(ID3D12Device5))
    {
        *ppvObj = this;
        AddRef();
        return S_OK;
    }
    // For anything else, check if the real device supports it and pass through.
    HRESULT hr = m_pReal->QueryInterface(riid, ppvObj);
    if (SUCCEEDED(hr)) {
        LOG_TRACE("WrappedDevice::QI pass-through for unknown IID");
    }
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
HRESULT WrappedDevice::GetPrivateData(REFGUID g, UINT* s, void* d)          { return m_pReal->GetPrivateData(g,s,d); }
HRESULT WrappedDevice::SetPrivateData(REFGUID g, UINT s, const void* d)     { return m_pReal->SetPrivateData(g,s,d); }
HRESULT WrappedDevice::SetPrivateDataInterface(REFGUID g, const IUnknown* p){ return m_pReal->SetPrivateDataInterface(g,p); }
HRESULT WrappedDevice::SetName(LPCWSTR n)                                   { return m_pReal->SetName(n); }

// ===========================================================================
// ID3D12Device – trivial pass-throughs
// ===========================================================================
UINT    WrappedDevice::GetNodeCount()                                        { return m_pReal->GetNodeCount(); }
HRESULT WrappedDevice::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* d, REFIID r, void** p) { return m_pReal->CreateCommandQueue(d,r,p); }
HRESULT WrappedDevice::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE t, REFIID r, void** p)    { return m_pReal->CreateCommandAllocator(t,r,p); }
HRESULT WrappedDevice::CheckFeatureSupport(D3D12_FEATURE f, void* d, UINT s)                    { return m_pReal->CheckFeatureSupport(f,d,s); }
HRESULT WrappedDevice::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID r, void** p) { return m_pReal->CreateDescriptorHeap(d,r,p); }
UINT    WrappedDevice::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE t)           { return m_pReal->GetDescriptorHandleIncrementSize(t); }
void    WrappedDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { m_pReal->CreateConstantBufferView(d,h); }
void    WrappedDevice::CreateShaderResourceView(ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { m_pReal->CreateShaderResourceView(r,d,h); }
void    WrappedDevice::CreateUnorderedAccessView(ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { m_pReal->CreateUnorderedAccessView(r,c,d,h); }
void    WrappedDevice::CreateRenderTargetView(ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)    { m_pReal->CreateRenderTargetView(r,d,h); }
void    WrappedDevice::CreateDepthStencilView(ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h)    { m_pReal->CreateDepthStencilView(r,d,h); }
void    WrappedDevice::CreateSampler(const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) { m_pReal->CreateSampler(d,h); }
void    WrappedDevice::CopyDescriptors(UINT nd,const D3D12_CPU_DESCRIPTOR_HANDLE* dp,const UINT* dr,UINT ns,const D3D12_CPU_DESCRIPTOR_HANDLE* sp,const UINT* sr,D3D12_DESCRIPTOR_HEAP_TYPE t) { m_pReal->CopyDescriptors(nd,dp,dr,ns,sp,sr,t); }
void    WrappedDevice::CopyDescriptorsSimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE t) { m_pReal->CopyDescriptorsSimple(n,d,s,t); }
D3D12_RESOURCE_ALLOCATION_INFO WrappedDevice::GetResourceAllocationInfo(UINT v, UINT n, const D3D12_RESOURCE_DESC* d) { return m_pReal->GetResourceAllocationInfo(v,n,d); }
D3D12_HEAP_PROPERTIES WrappedDevice::GetCustomHeapProperties(UINT m, D3D12_HEAP_TYPE t)         { return m_pReal->GetCustomHeapProperties(m,t); }
HRESULT WrappedDevice::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* h, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID r, void** p) { return m_pReal->CreateCommittedResource(h,f,d,s,c,r,p); }
HRESULT WrappedDevice::CreateHeap(const D3D12_HEAP_DESC* d, REFIID r, void** p)                 { return m_pReal->CreateHeap(d,r,p); }
HRESULT WrappedDevice::CreatePlacedResource(ID3D12Heap* h, UINT64 o, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID r, void** p) { return m_pReal->CreatePlacedResource(h,o,d,s,c,r,p); }
HRESULT WrappedDevice::CreateReservedResource(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID r, void** p) { return m_pReal->CreateReservedResource(d,s,c,r,p); }
HRESULT WrappedDevice::CreateSharedHandle(ID3D12DeviceChild* o, const SECURITY_ATTRIBUTES* a, DWORD acc, LPCWSTR n, HANDLE* h) { return m_pReal->CreateSharedHandle(o,a,acc,n,h); }
HRESULT WrappedDevice::OpenSharedHandle(HANDLE h, REFIID r, void** p)       { return m_pReal->OpenSharedHandle(h,r,p); }
HRESULT WrappedDevice::OpenSharedHandleByName(LPCWSTR n, DWORD a, HANDLE* h){ return m_pReal->OpenSharedHandleByName(n,a,h); }
HRESULT WrappedDevice::MakeResident(UINT n, ID3D12Pageable* const* p)       { return m_pReal->MakeResident(n,p); }
HRESULT WrappedDevice::Evict(UINT n, ID3D12Pageable* const* p)              { return m_pReal->Evict(n,p); }
HRESULT WrappedDevice::CreateFence(UINT64 v, D3D12_FENCE_FLAGS f, REFIID r, void** p) { return m_pReal->CreateFence(v,f,r,p); }
HRESULT WrappedDevice::GetDeviceRemovedReason()                             { return m_pReal->GetDeviceRemovedReason(); }
void    WrappedDevice::GetCopyableFootprints(const D3D12_RESOURCE_DESC* d, UINT fi, UINT nl, UINT64 bo, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, UINT* rn, UINT64* rs, UINT64* ts) { m_pReal->GetCopyableFootprints(d,fi,nl,bo,fp,rn,rs,ts); }
HRESULT WrappedDevice::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* d, REFIID r, void** p)      { return m_pReal->CreateQueryHeap(d,r,p); }
HRESULT WrappedDevice::SetStablePowerState(BOOL b)                          { return m_pReal->SetStablePowerState(b); }
HRESULT WrappedDevice::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* r, REFIID ri, void** p) { return m_pReal->CreateCommandSignature(d,r,ri,p); }
void    WrappedDevice::GetResourceTiling(ID3D12Resource* r, UINT* ts, D3D12_PACKED_MIP_INFO* pm, D3D12_TILE_SHAPE* st, UINT* snts, UINT fi, D3D12_SUBRESOURCE_TILING* t) { m_pReal->GetResourceTiling(r,ts,pm,st,snts,fi,t); }
LUID    WrappedDevice::GetAdapterLuid()                                     { return m_pReal->GetAdapterLuid(); }

// ===========================================================================
// KEY OVERRIDE: CreateGraphicsPipelineState
// ===========================================================================
HRESULT WrappedDevice::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPSO)
{
    if (!Cfg::g.stereoEnabled || !pDesc) return m_pReal->CreateGraphicsPipelineState(pDesc, riid, ppPSO);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = *pDesc;
    ShaderPatcher::PatchedPSO storage;
    bool patched = ShaderPatcher::PatchGraphicsPSO(desc, storage, (uint32_t)Cfg::g.stereoConstantBuffer);
    if (patched) LOG_DEBUG("CreateGraphicsPSO: shader(s) patched.");
    return m_pReal->CreateGraphicsPipelineState(&desc, riid, ppPSO);
}

// ===========================================================================
// KEY OVERRIDE: CreateComputePipelineState
// ===========================================================================
HRESULT WrappedDevice::CreateComputePipelineState(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPSO)
{
    if (!Cfg::g.stereoEnabled || !pDesc) return m_pReal->CreateComputePipelineState(pDesc, riid, ppPSO);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = *pDesc;
    ShaderPatcher::PatchedCSPSO storage;
    bool patched = ShaderPatcher::PatchComputePSO(desc, storage, (uint32_t)Cfg::g.stereoConstantBuffer);
    if (patched) LOG_DEBUG("CreateComputePSO: shader patched.");
    return m_pReal->CreateComputePipelineState(&desc, riid, ppPSO);
}

// ===========================================================================
// KEY OVERRIDE: CreateRootSignature – append stereo CBV parameter
// ===========================================================================
HRESULT WrappedDevice::CreateRootSignature(
    UINT nodeMask, const void* pBlob, SIZE_T blobLen, REFIID riid, void** ppRS)
{
    if (!Cfg::g.stereoEnabled)
        return m_pReal->CreateRootSignature(nodeMask, pBlob, blobLen, riid, ppRS);

    UINT stereoIdx = 0;
    ID3DBlob* pNewBlob = AppendStereoParam(pBlob, blobLen,
                                            (UINT)Cfg::g.stereoConstantBuffer, 13u,
                                            stereoIdx);
    HRESULT hr;
    if (pNewBlob) {
        hr = m_pReal->CreateRootSignature(nodeMask, pNewBlob->GetBufferPointer(),
                                          pNewBlob->GetBufferSize(), riid, ppRS);
        if (SUCCEEDED(hr) && ppRS && *ppRS) {
            RSMeta meta{};
            meta.stereoParamIdx = stereoIdx;
            TrackRSMeta(static_cast<ID3D12RootSignature*>(*ppRS), meta);
            LOG_DEBUG("CreateRootSignature: appended stereo param at slot %u", stereoIdx);
        }
        pNewBlob->Release();
    } else {
        LOG_WARN("CreateRootSignature: failed to append stereo param – using original RS");
        hr = m_pReal->CreateRootSignature(nodeMask, pBlob, blobLen, riid, ppRS);
    }
    return hr;
}

// ===========================================================================
// KEY OVERRIDE: CreateCommandList
// ===========================================================================
HRESULT WrappedDevice::CreateCommandList(
    UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pAlloc,
    ID3D12PipelineState* pInitPSO, REFIID riid, void** ppCmdList)
{
    HRESULT hr = m_pReal->CreateCommandList(nodeMask, type, pAlloc, pInitPSO,
                                             IID_PPV_ARGS((ID3D12GraphicsCommandList**)ppCmdList));
    if (SUCCEEDED(hr)) WrapCommandList(riid, ppCmdList);
    return hr;
}

// ===========================================================================
// ID3D12Device1
// ===========================================================================
HRESULT WrappedDevice::CreatePipelineLibrary(const void* d, SIZE_T s, REFIID r, void** p) {
    ID3D12Device1* p1 = nullptr;
    m_pReal->QueryInterface(IID_PPV_ARGS(&p1));
    HRESULT hr = p1 ? p1->CreatePipelineLibrary(d,s,r,p) : E_NOINTERFACE;
    if (p1) p1->Release();
    return hr;
}
HRESULT WrappedDevice::SetEventOnMultipleFenceCompletion(ID3D12Fence* const* f, const UINT64* v, UINT n, D3D12_MULTIPLE_FENCE_WAIT_FLAGS fl, HANDLE h) {
    ID3D12Device1* p1 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p1));
    HRESULT hr = p1 ? p1->SetEventOnMultipleFenceCompletion(f,v,n,fl,h) : E_NOINTERFACE;
    if (p1) p1->Release(); return hr;
}
HRESULT WrappedDevice::SetResidencyPriority(UINT n, ID3D12Pageable* const* p, const D3D12_RESIDENCY_PRIORITY* r) {
    ID3D12Device1* p1 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p1));
    HRESULT hr = p1 ? p1->SetResidencyPriority(n,p,r) : E_NOINTERFACE;
    if (p1) p1->Release(); return hr;
}

// ===========================================================================
// ID3D12Device2
// ===========================================================================
HRESULT WrappedDevice::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID r, void** p) {
    ID3D12Device2* p2 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p2));
    HRESULT hr = p2 ? p2->CreatePipelineState(d,r,p) : E_NOINTERFACE;
    if (p2) p2->Release(); return hr;
}

// ===========================================================================
// ID3D12Device3
// ===========================================================================
HRESULT WrappedDevice::OpenExistingHeapFromAddress(const void* a, REFIID r, void** p) {
    ID3D12Device3* p3 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p3));
    HRESULT hr = p3 ? p3->OpenExistingHeapFromAddress(a,r,p) : E_NOINTERFACE;
    if (p3) p3->Release(); return hr;
}
HRESULT WrappedDevice::OpenExistingHeapFromFileMapping(HANDLE h, REFIID r, void** p) {
    ID3D12Device3* p3 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p3));
    HRESULT hr = p3 ? p3->OpenExistingHeapFromFileMapping(h,r,p) : E_NOINTERFACE;
    if (p3) p3->Release(); return hr;
}
HRESULT WrappedDevice::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS f, UINT n, ID3D12Pageable* const* p, ID3D12Fence* fc, UINT64 v) {
    ID3D12Device3* p3 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p3));
    HRESULT hr = p3 ? p3->EnqueueMakeResident(f,n,p,fc,v) : E_NOINTERFACE;
    if (p3) p3->Release(); return hr;
}

// ===========================================================================
// ID3D12Device4
// ===========================================================================
HRESULT WrappedDevice::CreateCommandList1(UINT nm, D3D12_COMMAND_LIST_TYPE t, D3D12_COMMAND_LIST_FLAGS f, REFIID r, void** p) {
    ID3D12Device4* p4 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p4));
    HRESULT hr = p4 ? p4->CreateCommandList1(nm,t,f,IID_PPV_ARGS((ID3D12GraphicsCommandList**)p)) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) WrapCommandList(r, p);
    if (p4) p4->Release(); return hr;
}
HRESULT WrappedDevice::CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC* d, REFIID r, void** p) {
    ID3D12Device4* p4 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p4));
    HRESULT hr = p4 ? p4->CreateProtectedResourceSession(d,r,p) : E_NOINTERFACE;
    if (p4) p4->Release(); return hr;
}
HRESULT WrappedDevice::CreateHeap1(const D3D12_HEAP_DESC* d, ID3D12ProtectedResourceSession* s, REFIID r, void** p) {
    ID3D12Device4* p4 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p4));
    HRESULT hr = p4 ? p4->CreateHeap1(d,s,r,p) : E_NOINTERFACE;
    if (p4) p4->Release(); return hr;
}
HRESULT WrappedDevice::CreateReservedResource1(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* ps, REFIID r, void** p) {
    ID3D12Device4* p4 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p4));
    HRESULT hr = p4 ? p4->CreateReservedResource1(d,s,c,ps,r,p) : E_NOINTERFACE;
    if (p4) p4->Release(); return hr;
}
D3D12_RESOURCE_ALLOCATION_INFO WrappedDevice::GetResourceAllocationInfo1(UINT vm, UINT n, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_ALLOCATION_INFO1* i) {
    ID3D12Device4* p4 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p4));
    D3D12_RESOURCE_ALLOCATION_INFO r{};
    if (p4) { r = p4->GetResourceAllocationInfo1(vm,n,d,i); p4->Release(); }
    return r;
}

// ===========================================================================
// ID3D12Device5 (mostly pass-through / RT)
// ===========================================================================
HRESULT WrappedDevice::CreateLifetimeTracker(ID3D12LifetimeOwner* o, REFIID r, void** p) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->CreateLifetimeTracker(o,r,p) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
void WrappedDevice::RemoveDevice() { m_pReal->GetDeviceRemovedReason(); /* no-op pass */ }
HRESULT WrappedDevice::EnumerateMetaCommands(UINT* n, D3D12_META_COMMAND_DESC* d) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->EnumerateMetaCommands(n,d) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
HRESULT WrappedDevice::EnumerateMetaCommandParameters(REFGUID g, D3D12_META_COMMAND_PARAMETER_STAGE s, UINT* ts, UINT* n, D3D12_META_COMMAND_PARAMETER_DESC* d) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->EnumerateMetaCommandParameters(g,s,ts,n,d) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
HRESULT WrappedDevice::CreateMetaCommand(REFGUID g, UINT nm, const void* pc, SIZE_T ps, REFIID r, void** p) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->CreateMetaCommand(g,nm,pc,ps,r,p) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
HRESULT WrappedDevice::ExecuteMetaCommand(ID3D12MetaCommand* m, const void* ec, SIZE_T es, const void* dc, SIZE_T ds) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->ExecuteMetaCommand(m,ec,es,dc,ds) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
HRESULT WrappedDevice::BuildRaytracingAccelerationStructure(ID3D12GraphicsCommandList4* cl, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT ni, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* i) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->BuildRaytracingAccelerationStructure(cl,d,ni,i) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
HRESULT WrappedDevice::EmitRaytracingAccelerationStructurePostbuildInfo(ID3D12GraphicsCommandList4* cl, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n, const D3D12_GPU_VIRTUAL_ADDRESS* a) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->EmitRaytracingAccelerationStructurePostbuildInfo(cl,d,n,a) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
HRESULT WrappedDevice::CopyRaytracingAccelerationStructure(ID3D12GraphicsCommandList4* cl, D3D12_GPU_VIRTUAL_ADDRESS d, D3D12_GPU_VIRTUAL_ADDRESS s, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->CopyRaytracingAccelerationStructure(cl,d,s,m) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
HRESULT WrappedDevice::CreateStateObject(const D3D12_STATE_OBJECT_DESC* d, REFIID r, void** p) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    HRESULT hr = p5 ? p5->CreateStateObject(d,r,p) : E_NOINTERFACE;
    if (p5) p5->Release(); return hr;
}
void WrappedDevice::GetRaytracingAccelerationStructurePrebuildInfo(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* i, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* o) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    if (p5) { p5->GetRaytracingAccelerationStructurePrebuildInfo(i,o); p5->Release(); }
}
D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS WrappedDevice::CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE t, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* i) {
    ID3D12Device5* p5 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p5));
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS r = D3D12_DRIVER_MATCHING_IDENTIFIER_INCOMPATIBLE_VERSION;
    if (p5) { r = p5->CheckDriverMatchingIdentifier(t,i); p5->Release(); }
    return r;
}
