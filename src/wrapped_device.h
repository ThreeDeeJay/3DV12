#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <unordered_map>

class WrappedCommandList;

// ---------------------------------------------------------------------------
// WrappedDevice inherits only from ID3D12Device (the base, version 0).
//
// This sidesteps the SDK-version vtable fragility of Device1..Device5: the
// pure-virtual lists differ across Windows SDK releases (Build/Emit/Copy
// RaytracingAccelerationStructure appear on Device5 in some SDKs and on the
// command list in others).
//
// All interception targets live on the stable base interface:
//   CreateGraphicsPipelineState  – patch VS/PS shaders
//   CreateComputePipelineState   – patch CS shaders
//   CreateRootSignature          – append stereo CBV parameter
//   CreateCommandList            – return WrappedCommandList
//
// QueryInterface:
//   IUnknown / ID3D12Object / ID3D12Device  → this (our wrapper)
//   ID3D12Device1 … ID3D12DeviceN           → m_pReal (addref'd, pass-through)
// ---------------------------------------------------------------------------
class WrappedDevice : public ID3D12Device
{
public:
    explicit WrappedDevice(ID3D12Device* pReal);
    virtual ~WrappedDevice();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override;

    // ID3D12Device – all 37 pure virtuals
    UINT    STDMETHODCALLTYPE GetNodeCount() override;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE, void*, UINT) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**) override;
    UINT    STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override;
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT, const void*, SIZE_T, REFIID, void**) override;
    void    STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override;
    void    STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override;
    void    STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override;
    void    STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override;
    void    STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override;
    void    STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE) override;
    void    STDMETHODCALLTYPE CopyDescriptors(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE) override;
    void    STDMETHODCALLTYPE CopyDescriptorsSimple(UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE) override;
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT, UINT, const D3D12_RESOURCE_DESC*) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable* const*) override;
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable* const*) override;
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;
    void    STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC*, UINT, UINT, UINT64, D3D12_PLACED_SUBRESOURCE_FOOTPRINT*, UINT*, UINT64*, UINT64*) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override;
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**) override;
    void    STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*, UINT*, UINT, D3D12_SUBRESOURCE_TILING*) override;
    LUID    STDMETHODCALLTYPE GetAdapterLuid() override;

    // Accessors used by WrappedCommandList
    ID3D12Device*   GetReal()             const { return m_pReal; }
    ID3D12Resource* GetStereoParamBuffer();
    void            UpdateStereoParamBuffer(struct StereoParams* pParams);

    struct RSMeta { UINT origParamCount = 0; UINT stereoParamIdx = 0; };
    bool GetRSMeta(ID3D12RootSignature* pRS, RSMeta& out);

private:
    ID3D12Device*      m_pReal;
    std::atomic<ULONG> m_refCount;

    ID3D12Resource*    m_pStereoParamBuf = nullptr;
    void*              m_pStereoMapped   = nullptr;

    std::mutex         m_rsMtx;
    std::unordered_map<ID3D12RootSignature*, RSMeta> m_rsMeta;

    void    TrackRSMeta(ID3D12RootSignature* pRS, const RSMeta& m);
    HRESULT CreateStereoParamBuffer();
    HRESULT WrapCommandList(void** ppCmdList);
};
