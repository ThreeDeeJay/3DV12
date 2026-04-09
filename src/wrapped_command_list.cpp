// 3DV12 – Direct3D 12 stereoscopic shader-patching wrapper
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wrapped_command_list.h"
#include "wrapped_device.h"
#include "stereo_engine.h"
#include "config.h"
#include "log.h"
#include <d3d12.h>

WrappedCommandList::WrappedCommandList(ID3D12GraphicsCommandList* pReal, WrappedDevice* pDevice)
    : m_pReal(pReal), m_pDevice(pDevice), m_refCount(1)
{
    m_pReal->AddRef();
    EnsureReal4();
}

WrappedCommandList::~WrappedCommandList()
{
    if (m_pReal4) m_pReal4->Release();
    m_pReal->Release();
}

void WrappedCommandList::EnsureReal4()
{
    if (!m_pReal4) m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4));
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
HRESULT WrappedCommandList::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown)                    ||
        riid == __uuidof(ID3D12Object)                ||
        riid == __uuidof(ID3D12DeviceChild)           ||
        riid == __uuidof(ID3D12CommandList)           ||
        riid == __uuidof(ID3D12GraphicsCommandList)   ||
        riid == __uuidof(ID3D12GraphicsCommandList1)  ||
        riid == __uuidof(ID3D12GraphicsCommandList2)  ||
        riid == __uuidof(ID3D12GraphicsCommandList3)  ||
        riid == __uuidof(ID3D12GraphicsCommandList4))
    {
        *ppvObj = this;
        AddRef();
        return S_OK;
    }
    return m_pReal->QueryInterface(riid, ppvObj);
}
ULONG WrappedCommandList::AddRef()  { return ++m_refCount; }
ULONG WrappedCommandList::Release() { ULONG c = --m_refCount; if (!c) delete this; return c; }

// ---------------------------------------------------------------------------
// ID3D12Object
// ---------------------------------------------------------------------------
HRESULT WrappedCommandList::GetPrivateData(REFGUID g, UINT* s, void* d)           { return m_pReal->GetPrivateData(g,s,d); }
HRESULT WrappedCommandList::SetPrivateData(REFGUID g, UINT s, const void* d)      { return m_pReal->SetPrivateData(g,s,d); }
HRESULT WrappedCommandList::SetPrivateDataInterface(REFGUID g, const IUnknown* p) { return m_pReal->SetPrivateDataInterface(g,p); }
HRESULT WrappedCommandList::SetName(LPCWSTR n)                                    { return m_pReal->SetName(n); }
HRESULT WrappedCommandList::GetDevice(REFIID r, void** p)                         { return m_pReal->GetDevice(r,p); }
D3D12_COMMAND_LIST_TYPE WrappedCommandList::GetType()                             { return m_pReal->GetType(); }

// ---------------------------------------------------------------------------
// Reset / Close / ClearState
// ---------------------------------------------------------------------------
HRESULT WrappedCommandList::Close()  { return m_pReal->Close(); }
HRESULT WrappedCommandList::Reset(ID3D12CommandAllocator* a, ID3D12PipelineState* p)
{
    m_graphicsDirty = false;
    m_computeDirty  = false;
    m_pGraphicsRS   = nullptr;
    m_pComputeRS    = nullptr;
    return m_pReal->Reset(a, p);
}
void WrappedCommandList::ClearState(ID3D12PipelineState* p)
{
    m_graphicsDirty = false;
    m_computeDirty  = false;
    m_pReal->ClearState(p);
}

// ---------------------------------------------------------------------------
// Stereo CB auto-bind
// ---------------------------------------------------------------------------
void WrappedCommandList::BindStereoParam(bool isGraphics)
{
    if (!Cfg::g.stereoEnabled) return;
    if (!StereoEngine::IsActive())    return;

    ID3D12RootSignature* pRS = isGraphics ? m_pGraphicsRS : m_pComputeRS;
    if (!pRS) return;

    WrappedDevice::RSMeta meta{};
    if (!m_pDevice->GetRSMeta(pRS, meta)) return; // unknown RS – not patched

    ID3D12Resource* pBuf = m_pDevice->GetStereoParamBuffer();
    if (!pBuf) return;

    D3D12_GPU_VIRTUAL_ADDRESS va = pBuf->GetGPUVirtualAddress();
    if (isGraphics)
        m_pReal->SetGraphicsRootConstantBufferView(meta.stereoParamIdx, va);
    else
        m_pReal->SetComputeRootConstantBufferView(meta.stereoParamIdx, va);

    LOG_TRACE("BindStereoParam: slot=%u isGraphics=%d va=0x%llX",
              meta.stereoParamIdx, (int)isGraphics, (unsigned long long)va);
}

// ---------------------------------------------------------------------------
// KEY OVERRIDES: SetGraphicsRootSignature / SetComputeRootSignature
// ---------------------------------------------------------------------------
void WrappedCommandList::SetGraphicsRootSignature(ID3D12RootSignature* pRS)
{
    m_pGraphicsRS  = pRS;
    m_graphicsDirty = true;
    m_pReal->SetGraphicsRootSignature(pRS);
}
void WrappedCommandList::SetComputeRootSignature(ID3D12RootSignature* pRS)
{
    m_pComputeRS  = pRS;
    m_computeDirty = true;
    m_pReal->SetComputeRootSignature(pRS);
}

// ---------------------------------------------------------------------------
// KEY OVERRIDES: Draw* / Dispatch – bind stereo CB before each call
// ---------------------------------------------------------------------------
void WrappedCommandList::DrawInstanced(UINT v, UINT i, UINT sv, UINT si)
{
    if (m_graphicsDirty) { BindStereoParam(true); m_graphicsDirty = false; }
    m_pReal->DrawInstanced(v, i, sv, si);
}
void WrappedCommandList::DrawIndexedInstanced(UINT ipc, UINT ic, UINT sii, INT bv, UINT si)
{
    if (m_graphicsDirty) { BindStereoParam(true); m_graphicsDirty = false; }
    m_pReal->DrawIndexedInstanced(ipc, ic, sii, bv, si);
}
void WrappedCommandList::Dispatch(UINT x, UINT y, UINT z)
{
    if (m_computeDirty) { BindStereoParam(false); m_computeDirty = false; }
    m_pReal->Dispatch(x, y, z);
}

// ---------------------------------------------------------------------------
// ID3D12GraphicsCommandList – pass-through stubs
// ---------------------------------------------------------------------------
void WrappedCommandList::CopyBufferRegion(ID3D12Resource* d,UINT64 do_,ID3D12Resource* s,UINT64 so,UINT64 n) { m_pReal->CopyBufferRegion(d,do_,s,so,n); }
void WrappedCommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* d,UINT dx,UINT dy,UINT dz,const D3D12_TEXTURE_COPY_LOCATION* s,const D3D12_BOX* b) { m_pReal->CopyTextureRegion(d,dx,dy,dz,s,b); }
void WrappedCommandList::CopyResource(ID3D12Resource* d,ID3D12Resource* s) { m_pReal->CopyResource(d,s); }
void WrappedCommandList::CopyTiles(ID3D12Resource* r,const D3D12_TILED_RESOURCE_COORDINATE* c,const D3D12_TILE_REGION_SIZE* s,ID3D12Resource* b,UINT64 o,D3D12_TILE_COPY_FLAGS f) { m_pReal->CopyTiles(r,c,s,b,o,f); }
void WrappedCommandList::ResolveSubresource(ID3D12Resource* d,UINT ds,ID3D12Resource* s,UINT ss,DXGI_FORMAT f) { m_pReal->ResolveSubresource(d,ds,s,ss,f); }
void WrappedCommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) { m_pReal->IASetPrimitiveTopology(t); }
void WrappedCommandList::RSSetViewports(UINT n,const D3D12_VIEWPORT* v) { m_pReal->RSSetViewports(n,v); }
void WrappedCommandList::RSSetScissorRects(UINT n,const D3D12_RECT* r) { m_pReal->RSSetScissorRects(n,r); }
void WrappedCommandList::OMSetBlendFactor(const FLOAT f[4]) { m_pReal->OMSetBlendFactor(f); }
void WrappedCommandList::OMSetStencilRef(UINT r) { m_pReal->OMSetStencilRef(r); }
void WrappedCommandList::SetPipelineState(ID3D12PipelineState* p) { m_pReal->SetPipelineState(p); }
void WrappedCommandList::ResourceBarrier(UINT n,const D3D12_RESOURCE_BARRIER* b) { m_pReal->ResourceBarrier(n,b); }
void WrappedCommandList::ExecuteBundle(ID3D12GraphicsCommandList* b) { m_pReal->ExecuteBundle(b); }
void WrappedCommandList::SetDescriptorHeaps(UINT n,ID3D12DescriptorHeap* const* h) { m_pReal->SetDescriptorHeaps(n,h); }
void WrappedCommandList::SetGraphicsRootDescriptorTable(UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h) { m_pReal->SetGraphicsRootDescriptorTable(i,h); }
void WrappedCommandList::SetComputeRootDescriptorTable(UINT i,D3D12_GPU_DESCRIPTOR_HANDLE h)  { m_pReal->SetComputeRootDescriptorTable(i,h); }
void WrappedCommandList::SetGraphicsRoot32BitConstant(UINT i,UINT v,UINT o)                   { m_pReal->SetGraphicsRoot32BitConstant(i,v,o); }
void WrappedCommandList::SetComputeRoot32BitConstant(UINT i,UINT v,UINT o)                    { m_pReal->SetComputeRoot32BitConstant(i,v,o); }
void WrappedCommandList::SetGraphicsRoot32BitConstants(UINT i,UINT n,const void* d,UINT o)    { m_pReal->SetGraphicsRoot32BitConstants(i,n,d,o); }
void WrappedCommandList::SetComputeRoot32BitConstants(UINT i,UINT n,const void* d,UINT o)     { m_pReal->SetComputeRoot32BitConstants(i,n,d,o); }
void WrappedCommandList::SetGraphicsRootConstantBufferView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS a) { m_pReal->SetGraphicsRootConstantBufferView(i,a); }
void WrappedCommandList::SetComputeRootConstantBufferView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS a)  { m_pReal->SetComputeRootConstantBufferView(i,a); }
void WrappedCommandList::SetGraphicsRootShaderResourceView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS a) { m_pReal->SetGraphicsRootShaderResourceView(i,a); }
void WrappedCommandList::SetComputeRootShaderResourceView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS a)  { m_pReal->SetComputeRootShaderResourceView(i,a); }
void WrappedCommandList::SetGraphicsRootUnorderedAccessView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS a){ m_pReal->SetGraphicsRootUnorderedAccessView(i,a); }
void WrappedCommandList::SetComputeRootUnorderedAccessView(UINT i,D3D12_GPU_VIRTUAL_ADDRESS a) { m_pReal->SetComputeRootUnorderedAccessView(i,a); }
void WrappedCommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) { m_pReal->IASetIndexBuffer(v); }
void WrappedCommandList::IASetVertexBuffers(UINT s,UINT n,const D3D12_VERTEX_BUFFER_VIEW* v)  { m_pReal->IASetVertexBuffers(s,n,v); }
void WrappedCommandList::SOSetTargets(UINT s,UINT n,const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) { m_pReal->SOSetTargets(s,n,v); }
void WrappedCommandList::OMSetRenderTargets(UINT n,const D3D12_CPU_DESCRIPTOR_HANDLE* rt,BOOL s,const D3D12_CPU_DESCRIPTOR_HANDLE* ds) { m_pReal->OMSetRenderTargets(n,rt,s,ds); }
void WrappedCommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE h,D3D12_CLEAR_FLAGS f,FLOAT d,UINT8 s,UINT n,const D3D12_RECT* r) { m_pReal->ClearDepthStencilView(h,f,d,s,n,r); }
void WrappedCommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE h,const FLOAT c[4],UINT n,const D3D12_RECT* r) { m_pReal->ClearRenderTargetView(h,c,n,r); }
void WrappedCommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g,D3D12_CPU_DESCRIPTOR_HANDLE c,ID3D12Resource* r,const UINT v[4],UINT n,const D3D12_RECT* re) { m_pReal->ClearUnorderedAccessViewUint(g,c,r,v,n,re); }
void WrappedCommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE g,D3D12_CPU_DESCRIPTOR_HANDLE c,ID3D12Resource* r,const FLOAT v[4],UINT n,const D3D12_RECT* re) { m_pReal->ClearUnorderedAccessViewFloat(g,c,r,v,n,re); }
void WrappedCommandList::DiscardResource(ID3D12Resource* r,const D3D12_DISCARD_REGION* g) { m_pReal->DiscardResource(r,g); }
void WrappedCommandList::BeginQuery(ID3D12QueryHeap* h,D3D12_QUERY_TYPE t,UINT i) { m_pReal->BeginQuery(h,t,i); }
void WrappedCommandList::EndQuery(ID3D12QueryHeap* h,D3D12_QUERY_TYPE t,UINT i)   { m_pReal->EndQuery(h,t,i); }
void WrappedCommandList::ResolveQueryData(ID3D12QueryHeap* h,D3D12_QUERY_TYPE t,UINT s,UINT n,ID3D12Resource* r,UINT64 o) { m_pReal->ResolveQueryData(h,t,s,n,r,o); }
void WrappedCommandList::SetPredication(ID3D12Resource* r,UINT64 o,D3D12_PREDICATION_OP op) { m_pReal->SetPredication(r,o,op); }
void WrappedCommandList::SetMarker(UINT m,const void* d,UINT s)  { m_pReal->SetMarker(m,d,s); }
void WrappedCommandList::BeginEvent(UINT m,const void* d,UINT s) { m_pReal->BeginEvent(m,d,s); }
void WrappedCommandList::EndEvent()                              { m_pReal->EndEvent(); }
void WrappedCommandList::ExecuteIndirect(ID3D12CommandSignature* s,UINT mc,ID3D12Resource* ab,UINT64 ao,ID3D12Resource* cb,UINT64 co) { m_pReal->ExecuteIndirect(s,mc,ab,ao,cb,co); }

// ---------------------------------------------------------------------------
// ID3D12GraphicsCommandList1 stubs
// ---------------------------------------------------------------------------
void WrappedCommandList::AtomicCopyBufferUINT(ID3D12Resource* d,UINT64 do_,ID3D12Resource* s,UINT64 so,UINT n,ID3D12Resource* const* dd,const D3D12_SUBRESOURCE_DATA* sd) {
    ID3D12GraphicsCommandList1* p = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p));
    if (p) { p->AtomicCopyBufferUINT(d,do_,s,so,n,dd,sd); p->Release(); }
}
void WrappedCommandList::AtomicCopyBufferUINT64(ID3D12Resource* d,UINT64 do_,ID3D12Resource* s,UINT64 so,UINT n,ID3D12Resource* const* dd,const D3D12_SUBRESOURCE_DATA* sd) {
    ID3D12GraphicsCommandList1* p = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p));
    if (p) { p->AtomicCopyBufferUINT64(d,do_,s,so,n,dd,sd); p->Release(); }
}
void WrappedCommandList::OMSetDepthBounds(FLOAT mn,FLOAT mx) {
    ID3D12GraphicsCommandList1* p = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p));
    if (p) { p->OMSetDepthBounds(mn,mx); p->Release(); }
}
void WrappedCommandList::SetSamplePositions(UINT spp,UINT pix,D3D12_SAMPLE_POSITION* pos) {
    ID3D12GraphicsCommandList1* p = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p));
    if (p) { p->SetSamplePositions(spp,pix,pos); p->Release(); }
}
void WrappedCommandList::ResolveSubresourceRegion(ID3D12Resource* d,UINT ds,UINT dx,UINT dy,ID3D12Resource* s,UINT ss,D3D12_RECT* sr,DXGI_FORMAT f,D3D12_RESOLVE_MODE m) {
    ID3D12GraphicsCommandList1* p = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p));
    if (p) { p->ResolveSubresourceRegion(d,ds,dx,dy,s,ss,sr,f,m); p->Release(); }
}
void WrappedCommandList::SetViewInstanceMask(UINT mask) {
    ID3D12GraphicsCommandList1* p = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p));
    if (p) { p->SetViewInstanceMask(mask); p->Release(); }
}

// ---------------------------------------------------------------------------
// ID3D12GraphicsCommandList2 stubs
// ---------------------------------------------------------------------------
void WrappedCommandList::WriteBufferImmediate(UINT n,const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p,const D3D12_WRITEBUFFERIMMEDIATE_MODE* m) {
    ID3D12GraphicsCommandList2* p2 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p2));
    if (p2) { p2->WriteBufferImmediate(n,p,m); p2->Release(); }
}

// ---------------------------------------------------------------------------
// ID3D12GraphicsCommandList3 stubs
// ---------------------------------------------------------------------------
void WrappedCommandList::SetProtectedResourceSession(ID3D12ProtectedResourceSession* s) {
    ID3D12GraphicsCommandList3* p3 = nullptr; m_pReal->QueryInterface(IID_PPV_ARGS(&p3));
    if (p3) { p3->SetProtectedResourceSession(s); p3->Release(); }
}

// ---------------------------------------------------------------------------
// ID3D12GraphicsCommandList4 stubs
// ---------------------------------------------------------------------------
void WrappedCommandList::BeginRenderPass(UINT n,const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt,const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds,D3D12_RENDER_PASS_FLAGS f) {
    EnsureReal4(); if (m_pReal4) m_pReal4->BeginRenderPass(n,rt,ds,f);
}
void WrappedCommandList::EndRenderPass() { EnsureReal4(); if (m_pReal4) m_pReal4->EndRenderPass(); }
void WrappedCommandList::InitializeMetaCommand(ID3D12MetaCommand* m,const void* p,SIZE_T s) {
    EnsureReal4(); if (m_pReal4) m_pReal4->InitializeMetaCommand(m,p,s);
}
void WrappedCommandList::ExecuteMetaCommand(ID3D12MetaCommand* m,const void* p,SIZE_T s) {
    EnsureReal4(); if (m_pReal4) m_pReal4->ExecuteMetaCommand(m,p,s);
}
void WrappedCommandList::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d,UINT n,const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* i) {
    EnsureReal4(); if (m_pReal4) m_pReal4->BuildRaytracingAccelerationStructure(d,n,i);
}
void WrappedCommandList::EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d,UINT n,const D3D12_GPU_VIRTUAL_ADDRESS* a) {
    EnsureReal4(); if (m_pReal4) m_pReal4->EmitRaytracingAccelerationStructurePostbuildInfo(d,n,a);
}
void WrappedCommandList::CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS d,D3D12_GPU_VIRTUAL_ADDRESS s,D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) {
    EnsureReal4(); if (m_pReal4) m_pReal4->CopyRaytracingAccelerationStructure(d,s,m);
}
void WrappedCommandList::SetPipelineState1(ID3D12StateObject* s) {
    EnsureReal4(); if (m_pReal4) m_pReal4->SetPipelineState1(s);
}
void WrappedCommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) {
    EnsureReal4(); if (m_pReal4) m_pReal4->DispatchRays(d);
}
