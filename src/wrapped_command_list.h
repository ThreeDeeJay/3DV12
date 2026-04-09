#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WrappedCommandList intercepts SetGraphicsRootSignature and
// SetComputeRootSignature so it can automatically bind the stereo
// constant buffer to the appended root parameter slot on every draw/dispatch.
//
// Design:
//   • On SetGraphicsRootSignature / SetComputeRootSignature we look up the
//     RSMeta from WrappedDevice to find the stereo param root index.
//   • On DrawInstanced / DrawIndexedInstanced / Dispatch we ensure the stereo
//     CBV is bound (deferred-bind approach: bind on the first draw after an RS change).
//   • Per-eye update is driven externally: the game's Present wrapper calls
//     WrappedCommandList::UpdateStereoEye() before each eye render.

#include <d3d12.h>
#include <atomic>

class WrappedDevice;

class WrappedCommandList : public ID3D12GraphicsCommandList4
{
public:
    WrappedCommandList(ID3D12GraphicsCommandList* pReal, WrappedDevice* pDevice);
    virtual ~WrappedCommandList();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // ID3D12Object
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override;

    // ID3D12DeviceChild
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override;

    // ID3D12CommandList
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override;

    // ID3D12GraphicsCommandList – key overrides
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator*, ID3D12PipelineState*) override;
    void    STDMETHODCALLTYPE ClearState(ID3D12PipelineState*) override;
    void    STDMETHODCALLTYPE DrawInstanced(UINT, UINT, UINT, UINT) override;
    void    STDMETHODCALLTYPE DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) override;
    void    STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) override;
    void    STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature*) override;
    void    STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature*) override;

    // ID3D12GraphicsCommandList – pass-through stubs
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT64) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION*,UINT,UINT,UINT,const D3D12_TEXTURE_COPY_LOCATION*,const D3D12_BOX*) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource*,ID3D12Resource*) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*,const D3D12_TILED_RESOURCE_COORDINATE*,const D3D12_TILE_REGION_SIZE*,ID3D12Resource*,UINT64,D3D12_TILE_COPY_FLAGS) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource*,UINT,ID3D12Resource*,UINT,DXGI_FORMAT) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT,const D3D12_VIEWPORT*) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT,const D3D12_RECT*) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState*) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT,const D3D12_RESOURCE_BARRIER*) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT,ID3D12DescriptorHeap* const*) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT,D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT,D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT,UINT,UINT) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT,UINT,UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT,UINT,const void*,UINT) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT,UINT,const void*,UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT,D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT,D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT,D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT,D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT,D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT,D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT,UINT,const D3D12_VERTEX_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT,UINT,const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT,const D3D12_CPU_DESCRIPTOR_HANDLE*,BOOL,const D3D12_CPU_DESCRIPTOR_HANDLE*) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CLEAR_FLAGS,FLOAT,UINT8,UINT,const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE,const FLOAT[4],UINT,const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const UINT[4],UINT,const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,ID3D12Resource*,const FLOAT[4],UINT,const D3D12_RECT*) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource*,const D3D12_DISCARD_REGION*) override;
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT) override;
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap*,D3D12_QUERY_TYPE,UINT,UINT,ID3D12Resource*,UINT64) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource*,UINT64,D3D12_PREDICATION_OP) override;
    void STDMETHODCALLTYPE SetMarker(UINT,const void*,UINT) override;
    void STDMETHODCALLTYPE BeginEvent(UINT,const void*,UINT) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*,UINT,ID3D12Resource*,UINT64,ID3D12Resource*,UINT64) override;

    // ID3D12GraphicsCommandList1
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT,ID3D12Resource* const*,const D3D12_SUBRESOURCE_DATA*) override;
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource*,UINT64,ID3D12Resource*,UINT64,UINT,ID3D12Resource* const*,const D3D12_SUBRESOURCE_DATA*) override;
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT,FLOAT) override;
    void STDMETHODCALLTYPE SetSamplePositions(UINT,UINT,D3D12_SAMPLE_POSITION*) override;
    void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource*,UINT,UINT,UINT,ID3D12Resource*,UINT,D3D12_RECT*,DXGI_FORMAT,D3D12_RESOLVE_MODE) override;
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT) override;

    // ID3D12GraphicsCommandList2
    void STDMETHODCALLTYPE WriteBufferImmediate(UINT,const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER*,const D3D12_WRITEBUFFERIMMEDIATE_MODE*) override;

    // ID3D12GraphicsCommandList3
    void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession*) override;

    // ID3D12GraphicsCommandList4
    void STDMETHODCALLTYPE BeginRenderPass(UINT,const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,D3D12_RENDER_PASS_FLAGS) override;
    void STDMETHODCALLTYPE EndRenderPass() override;
    void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand*,const void*,SIZE_T) override;
    void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand*,const void*,SIZE_T) override;
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*,UINT,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*) override;
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*,UINT,const D3D12_GPU_VIRTUAL_ADDRESS*) override;
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS,D3D12_GPU_VIRTUAL_ADDRESS,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE) override;
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject*) override;
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC*) override;

    // Internal: update stereo CB binding for current eye
    void BindStereoParam(bool isGraphics);

    ID3D12GraphicsCommandList* GetReal() const { return m_pReal; }

private:
    ID3D12GraphicsCommandList4* m_pReal4 = nullptr; // QI'd from m_pReal for CL4 methods
    ID3D12GraphicsCommandList*  m_pReal  = nullptr;
    WrappedDevice*              m_pDevice;
    std::atomic<ULONG>          m_refCount;

    // Deferred stereo bind state
    ID3D12RootSignature* m_pGraphicsRS    = nullptr;
    ID3D12RootSignature* m_pComputeRS     = nullptr;
    bool                 m_graphicsDirty  = false;
    bool                 m_computeDirty   = false;

    void EnsureReal4();
};
