#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

// Forward declaration
class WrappedCommandList;

// ---------------------------------------------------------------------------
// WrappedDevice wraps ID3D12Device (base interface).
// Higher-versioned interfaces (Device1..Device9) are forwarded via QI to the
// real device, then re-wrapped on the fly.  The critical overrides are:
//   • CreateGraphicsPipelineState  – patch VS/PS shaders
//   • CreateComputePipelineState   – patch CS shaders
//   • CreateRootSignature          – append stereo CBV parameter
//   • CreateCommandList / 1        – return WrappedCommandList
//   • CreateCommandQueue           – pass-through (no wrapping needed)
// ---------------------------------------------------------------------------
class WrappedDevice : public ID3D12Device5
{
public:
    WrappedDevice(ID3D12Device* pReal);
    virtual ~WrappedDevice();

    // ----- IUnknown -----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // ----- ID3D12Object -----
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // ----- ID3D12Device -----
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

    // ----- ID3D12Device1 -----
    HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void*, SIZE_T, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(ID3D12Fence* const*, const UINT64*, UINT, D3D12_MULTIPLE_FENCE_WAIT_FLAGS, HANDLE) override;
    HRESULT STDMETHODCALLTYPE SetResidencyPriority(UINT, ID3D12Pageable* const*, const D3D12_RESIDENCY_PRIORITY*) override;

    // ----- ID3D12Device2 -----
    HRESULT STDMETHODCALLTYPE CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**) override;

    // ----- ID3D12Device3 -----
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(const void*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(HANDLE, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE EnqueueMakeResident(D3D12_RESIDENCY_FLAGS, UINT, ID3D12Pageable* const*, ID3D12Fence*, UINT64) override;

    // ----- ID3D12Device4 -----
    HRESULT STDMETHODCALLTYPE CreateCommandList1(UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC*, ID3D12ProtectedResourceSession*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource1(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**) override;
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo1(UINT, UINT, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_ALLOCATION_INFO1*) override;

    // ----- ID3D12Device5 -----
    HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(ID3D12LifetimeOwner*, REFIID, void**) override;
    void    STDMETHODCALLTYPE RemoveDevice() override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(UINT*, D3D12_META_COMMAND_DESC*) override;
    HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(REFGUID, D3D12_META_COMMAND_PARAMETER_STAGE, UINT*, UINT*, D3D12_META_COMMAND_PARAMETER_DESC*) override;
    HRESULT STDMETHODCALLTYPE CreateMetaCommand(REFGUID, UINT, const void*, SIZE_T, REFIID, void**) override;
    void    STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(ID3D12GraphicsCommandList4*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*) override;
    void    STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(ID3D12GraphicsCommandList4*, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*, UINT, const D3D12_GPU_VIRTUAL_ADDRESS*) override;
    void    STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(ID3D12GraphicsCommandList4*, D3D12_GPU_VIRTUAL_ADDRESS, D3D12_GPU_VIRTUAL_ADDRESS, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE) override;
    HRESULT STDMETHODCALLTYPE CreateStateObject(const D3D12_STATE_OBJECT_DESC*, REFIID, void**) override;
    void    STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*) override;
    D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER*) override;

    // ----- Accessors -----
    ID3D12Device* GetReal() const { return m_pReal; }

    // Stereo param GPU buffer: one per frame-in-flight for safe upload
    ID3D12Resource* GetStereoParamBuffer();
    void UpdateStereoParamBuffer(struct StereoParams* pParams);

    // Root signature metadata (how many parameters the original RS had)
    struct RSMeta {
        UINT origParamCount = 0;
        UINT stereoParamIdx = 0; // index of the appended stereo CBV
    };
    bool GetRSMeta(ID3D12RootSignature* pRS, RSMeta& out);

private:
    ID3D12Device*        m_pReal;
    std::atomic<ULONG>   m_refCount;

    // Stereo constant buffer
    ID3D12Resource*       m_pStereoParamBuf  = nullptr;
    void*                 m_pStereoMapped    = nullptr;

    // RS metadata map
    std::mutex            m_rsMtx;
    std::unordered_map<ID3D12RootSignature*, RSMeta> m_rsMeta;

    void TrackRSMeta(ID3D12RootSignature* pRS, const RSMeta& m);
    HRESULT CreateStereoParamBuffer();

    // Helper: wrap a raw command list pointer in a WrappedCommandList
    HRESULT WrapCommandList(REFIID riid, void** ppCmdList);
};
