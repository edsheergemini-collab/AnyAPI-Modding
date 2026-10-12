#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "effects/lens_gpu.h"
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
// Lens on WARP: with focus on a near band in the middle, the far striped
// background blurs and the band stays sharp; turning the camera blurs the
// background but not things within a metre; alpha is kept; both off changes
// nothing.
// WARP can drop the device when the machine is busy (seen right after the other
// GPU tests). That is retried once on a fresh device; pixel checks never are.
struct DeviceLost:std::runtime_error{using std::runtime_error::runtime_error;};
static void body(){
 ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> factory;assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));ComPtr<IDXGIAdapter> adapter;assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));ComPtr<ID3D12Device> device;assert(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;assert(SUCCEEDED(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue))));ComPtr<ID3D12Fence> fence;assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial{};
 auto lost=[&](const char* where,HRESULT hr){char text[160];snprintf(text,sizeof(text),"%s failed hr=0x%08lx removed=0x%08lx",where,(unsigned long)hr,(unsigned long)device->GetDeviceRemovedReason());throw DeviceLost(text);};
 auto wait=[&]{if(HRESULT hr=queue->Signal(fence.Get(),++serial);FAILED(hr))lost("Signal",hr);assert(SUCCEEDED(fence->SetEventOnCompletion(serial,event)));assert(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);if(HRESULT hr=device->GetDeviceRemovedReason();FAILED(hr))lost("Wait",hr);};
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));assert(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))));assert(SUCCEEDED(list->Close()));
 const UINT W=256,H=256;
 struct Texture{ComPtr<ID3D12Resource> resource,upload,readback;D3D12_RESOURCE_DESC desc{};D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};};
 auto buffer=[&](ComPtr<ID3D12Resource>& out,UINT64 bytes,D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;assert(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out))));};
 auto texture=[&](DXGI_FORMAT format,bool target){Texture t;t.desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;t.desc.Width=W;t.desc.Height=H;t.desc.DepthOrArraySize=1;t.desc.MipLevels=1;t.desc.Format=format;t.desc.SampleDesc.Count=1;t.desc.Flags=target?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_NONE;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;assert(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&t.desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.resource))));device->GetCopyableFootprints(&t.desc,0,1,0,&t.footprint,nullptr,nullptr,&t.bytes);buffer(t.upload,t.bytes,D3D12_HEAP_TYPE_UPLOAD);buffer(t.readback,t.bytes,D3D12_HEAP_TYPE_READBACK);return t;};
 auto copy=[&](Texture& t,bool upload){D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=t.resource.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=upload?t.upload.Get():t.readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=t.footprint;if(upload)list->CopyTextureRegion(&a,0,0,0,&b,nullptr);else list->CopyTextureRegion(&b,0,0,0,&a,nullptr);};
 auto fill=[&](Texture& t,const std::function<void(unsigned char*,UINT,UINT)>& write){unsigned char* data{};D3D12_RANGE none{0,0};assert(SUCCEEDED(t.upload->Map(0,&none,(void**)&data)));for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x)write(data+t.footprint.Offset+y*t.footprint.Footprint.RowPitch,x,y);t.upload->Unmap(0,nullptr);copy(t,true);};
 scene_lens_gpu::Gpu gpu;const float near_plane=.1f;
 // One-pixel black and white columns 100 m away; rows 120..136 hold a band
 // `band_z` metres away, also striped.
 auto run=[&](const scene_lens_gpu::Frame& f,float band_z){std::vector<float> out(W*H*4);
  wait();assert(SUCCEEDED(allocator->Reset()));assert(SUCCEEDED(list->Reset(allocator.Get(),nullptr)));
  auto target=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,true),depth=texture(DXGI_FORMAT_R32_FLOAT,false);
  assert(gpu.ensure(device.Get(),queue.Get(),target.desc));
  auto rgba=[](unsigned char* p,UINT x,float r,float g,float b,float a){auto h=(HALF*)p+x*4;h[0]=XMConvertFloatToHalf(r);h[1]=XMConvertFloatToHalf(g);h[2]=XMConvertFloatToHalf(b);h[3]=XMConvertFloatToHalf(a);};
  fill(target,[&](unsigned char* p,UINT x,UINT){float v=float(x&1);rgba(p,x,v,v,v,.25f);});
  fill(depth,[&](unsigned char* p,UINT x,UINT y){((float*)p)[x]=near_plane/(y>=120&&y<136?band_z:100.f);});
  D3D12_RESOURCE_BARRIER b[2]={scene_lens_gpu::barrier(target.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET),scene_lens_gpu::barrier(depth.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON)};list->ResourceBarrier(2,b);
  assert(gpu.record(list.Get(),target.resource.Get(),{depth.resource.Get(),D3D12_RESOURCE_STATE_COMMON},f));
  b[0]=scene_lens_gpu::barrier(target.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,b);copy(target,false);assert(SUCCEEDED(list->Close()));ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);wait();
  unsigned char* pixels{};D3D12_RANGE range{0,SIZE_T(target.bytes)};if(HRESULT hr=target.readback->Map(0,&range,(void**)&pixels);FAILED(hr))lost("Map",hr);
  for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){auto h=(const HALF*)(pixels+target.footprint.Offset+y*target.footprint.Footprint.RowPitch)+x*4;for(int i=0;i<4;++i)out[(y*W+x)*4+i]=XMConvertHalfToFloat(h[i]);}
  D3D12_RANGE none{0,0};target.readback->Unmap(0,&none);return out;};
 auto base=[&]{scene_lens_gpu::Frame f;f.metrics={1.f/W,1.f/H,float(W),float(H)};f.right={1,0,0,.5f};f.up={0,1,0,.5f};f.forward={0,0,1,5000};f.projection={0,near_plane,1,0};f.camera_move={0,0,0,1};f.params={0,0,12,0};return f;};
 // Largest step between neighbouring pixels of a row across the middle of the screen.
 auto contrast=[&](const std::vector<float>& o,UINT y){float m=0;for(UINT x=96;x<160;++x)m=std::max(m,std::abs(o[(y*W+x)*4]-o[(y*W+x+1)*4]));return m;};
 unsigned cases=0;
 {auto o=run(base(),1.5f);for(UINT i=0;i<W*H;++i)assert(o[i*4]==float((i%W)&1)&&o[i*4+3]==.25f);std::cout<<"Both off unchanged\n";++cases;}
 {auto f=base();f.params[0]=2;auto o=run(f,1.5f);float distant=contrast(o,40),band=contrast(o,128);std::cout<<"Depth of field far contrast="<<distant<<" focused band contrast="<<band<<'\n';
  assert(distant<.5f&&band>.99f);for(UINT i=0;i<W*H;++i)assert(o[i*4+3]==.25f);cases+=2;}
 {auto f=base();const float a=.05f;f.params[1]=1;f.previous_right={std::cos(a),0,-std::sin(a),.5f};f.previous_up={0,1,0,.5f};f.previous_forward={std::sin(a),0,std::cos(a),0};
  auto o=run(f,.8f);float distant=contrast(o,40),close=contrast(o,128);std::cout<<"Motion blur far contrast="<<distant<<" held-item band contrast="<<close<<'\n';assert(distant<.5f&&close>.99f);cases+=2;
  auto still=base();still.params[1]=1;still.previous_right=still.right;still.previous_up=still.up;still.previous_forward={0,0,1,0};o=run(still,.8f);assert(contrast(o,40)>.99f);std::cout<<"Still camera unchanged\n";++cases;}
 auto bad=base();bad.params[2]=0;wait();assert(SUCCEEDED(allocator->Reset()));assert(SUCCEEDED(list->Reset(allocator.Get(),nullptr)));assert(!gpu.record(list.Get(),nullptr,{},bad));assert(SUCCEEDED(list->Close()));
 std::cout<<"PASS: lens "<<cases<<" D3D12 cases\n";CloseHandle(event);
}
int main(){std::cout<<std::unitbuf;try{body();}catch(const DeviceLost& first){std::cout<<"Device lost ("<<first.what()<<"), retrying once on a new device"<<std::endl;try{body();}catch(const DeviceLost& again){std::cout<<"FAIL: device lost again ("<<again.what()<<")"<<std::endl;return 1;}}}
