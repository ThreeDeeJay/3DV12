#pragma once
// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WrappedCommandList inherits only from ID3D12GraphicsCommandList (CL0).
//
// The same SDK vtable fragility that affects ID3D12Device5 affects
// ID3D12GraphicsCommandList1..4: the pure-virtual lists differ across Windows
// SDK releases.  Inheriting any CL1+ interface risks a corrupt vtable.
//
// ID3D12GraphicsCommandList has a stable set of pure virtuals across every
// SDK.  For CL1–4 methods (AtomicCopyBuffer, BeginRenderPass, DispatchRays,
// etc.) we QI the real command list at call time and forward through it.
//
// QueryInterface returns 'this' for all CL0..CL9 IIDs after verifying the
// real object supports the version, exactly as WrappedDevice does.

#include <d3d12.h>
#include <atomic>

class WrappedDevice;

class WrappedCommandList : public ID3D12GraphicsCommandList
{
public:
    WrappedCommandList(ID3D12GraphicsCommandList* pReal, WrappedDevice* pDevice);
    virtual ~WrappedCommandList();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
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

    // ID3D12GraphicsCommandList – all 57 pure virtuals (stable across all SDKs)
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator*, ID3D12PipelineState*) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState*) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT, UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) override;
    void STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource*, ID3D12Resource*) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT, const D3D12_VIEWPORT*) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState*) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER*) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT, ID3D12DescriptorHeap* const*) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature*) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature*) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT, UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const FLOAT[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource*, const D3D12_DISCARD_REGION*) override;
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override;
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) override;
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64) override;

    ID3D12GraphicsCommandList* GetReal() const { return m_pReal; }

private:
    void BindStereoParam(bool isGraphics);

    ID3D12GraphicsCommandList* m_pReal;
    WrappedDevice*             m_pDevice;
    std::atomic<ULONG>         m_refCount;

    // Deferred stereo bind state
    ID3D12RootSignature* m_pGraphicsRS   = nullptr;
    ID3D12RootSignature* m_pComputeRS    = nullptr;
    bool                 m_graphicsDirty = false;
    bool                 m_computeDirty  = false;
};
