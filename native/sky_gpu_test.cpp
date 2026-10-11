#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <vector>
#include "effects/sky_gpu.h"
using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;
// Night sky on WARP: daylight changes nothing, the night sky gains stars, the
// moon shows where it should, the ocean and the target's alpha are untouched,
// and moonlight lights only surfaces facing the moon.
int main(){std::cout<<std::unitbuf;
 ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
 ComPtr<IDXGIFactory4> factory;assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));ComPtr<IDXGIAdapter> adapter;assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));ComPtr<ID3D12Device> device;assert(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
 D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;assert(SUCCEEDED(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue))));ComPtr<ID3D12Fence> fence;assert(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial{};
 auto wait=[&]{assert(SUCCEEDED(queue->Signal(fence.Get(),++serial)));assert(SUCCEEDED(fence->SetEventOnCompletion(serial,event)));assert(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);};
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))));assert(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))));assert(SUCCEEDED(list->Close()));
 const UINT W=64,H=64;
 struct Texture{ComPtr<ID3D12Resource> resource,upload,readback;D3D12_RESOURCE_DESC desc{};D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};};
 auto buffer=[&](ComPtr<ID3D12Resource>& out,UINT64 bytes,D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;assert(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out))));};
 auto texture=[&](DXGI_FORMAT format,bool target){Texture t;t.desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;t.desc.Width=W;t.desc.Height=H;t.desc.DepthOrArraySize=1;t.desc.MipLevels=1;t.desc.Format=format;t.desc.SampleDesc.Count=1;t.desc.Flags=target?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_NONE;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;assert(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&t.desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.resource))));device->GetCopyableFootprints(&t.desc,0,1,0,&t.footprint,nullptr,nullptr,&t.bytes);buffer(t.upload,t.bytes,D3D12_HEAP_TYPE_UPLOAD);buffer(t.readback,t.bytes,D3D12_HEAP_TYPE_READBACK);return t;};
 auto copy=[&](Texture& t,bool upload){D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=t.resource.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=upload?t.upload.Get():t.readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=t.footprint;if(upload)list->CopyTextureRegion(&a,0,0,0,&b,nullptr);else list->CopyTextureRegion(&b,0,0,0,&a,nullptr);};
 auto fill=[&](Texture& t,const std::function<void(unsigned char*,UINT,UINT)>& write){unsigned char* data{};D3D12_RANGE none{0,0};assert(SUCCEEDED(t.upload->Map(0,&none,(void**)&data)));for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x)write(data+t.footprint.Offset+y*t.footprint.Footprint.RowPitch,x,y);t.upload->Unmap(0,nullptr);copy(t,true);};
 scene_sky_gpu::Gpu gpu;
 struct Scene{std::function<float(UINT,UINT)> depth=[](UINT,UINT){return 0.f;};std::function<bool(UINT,UINT)> sea=[](UINT,UINT){return false;};float normal[3]{0,1,0};};
 // Returns RGBA after the pass; the target starts at 0.1 grey with alpha 0.5.
 auto run=[&](const scene_sky_gpu::Frame& f,const Scene& s){std::vector<float> out(W*H*4);
  wait();assert(SUCCEEDED(allocator->Reset()));assert(SUCCEEDED(list->Reset(allocator.Get(),nullptr)));
  auto target=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,true),depth=texture(DXGI_FORMAT_R32_FLOAT,false),normals=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,false),albedo=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,false),ocean=texture(DXGI_FORMAT_R16G16B16A16_FLOAT,false);
  assert(gpu.ensure(device.Get(),queue.Get(),target.desc));
  auto rgba=[](unsigned char* p,UINT x,float r,float g,float b,float a){auto h=(HALF*)p+x*4;h[0]=XMConvertFloatToHalf(r);h[1]=XMConvertFloatToHalf(g);h[2]=XMConvertFloatToHalf(b);h[3]=XMConvertFloatToHalf(a);};
  fill(target,[&](unsigned char* p,UINT x,UINT){rgba(p,x,.1f,.1f,.1f,.5f);});
  fill(depth,[&](unsigned char* p,UINT x,UINT y){((float*)p)[x]=s.depth(x,y);});
  fill(normals,[&](unsigned char* p,UINT x,UINT){rgba(p,x,s.normal[0],s.normal[1],s.normal[2],0);});
  fill(albedo,[&](unsigned char* p,UINT x,UINT){rgba(p,x,.5f,.5f,.5f,0);});
  fill(ocean,[&](unsigned char* p,UINT x,UINT y){bool w=s.sea(x,y);rgba(p,x,w?1.f:0.f,w?50.f:0.f,0,0);});
  D3D12_RESOURCE_BARRIER b[5]={scene_sky_gpu::barrier(target.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET),scene_sky_gpu::barrier(depth.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),scene_sky_gpu::barrier(normals.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON),scene_sky_gpu::barrier(albedo.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON),scene_sky_gpu::barrier(ocean.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON)};list->ResourceBarrier(5,b);
  using I=scene_sky_gpu::Gpu::Input;const auto read=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,common=D3D12_RESOURCE_STATE_COMMON;
  assert(gpu.record(list.Get(),target.resource.Get(),I{depth.resource.Get(),read},I{normals.resource.Get(),common},I{albedo.resource.Get(),common},I{ocean.resource.Get(),common},f));
  b[0]=scene_sky_gpu::barrier(target.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,b);copy(target,false);assert(SUCCEEDED(list->Close()));ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);wait();
  unsigned char* pixels{};D3D12_RANGE range{0,SIZE_T(target.bytes)};assert(SUCCEEDED(target.readback->Map(0,&range,(void**)&pixels)));
  for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){auto h=(const HALF*)(pixels+target.footprint.Offset+y*target.footprint.Footprint.RowPitch)+x*4;for(int i=0;i<4;++i)out[(y*W+x)*4+i]=XMConvertHalfToFloat(h[i]);}
  D3D12_RANGE none{0,0};target.readback->Unmap(0,&none);return out;};
 float noon[3]={0,1,0},midnight[3]={0,-1,0};unsigned cases=0;
 // Looking north and up, away from the moon (straight up at midnight).
 float right[4]{1,0,0,.8f},up[4]{0,.866f,.5f,.8f},forward[4]{0,.5f,-.866f,0};
 auto alpha_kept=[&](const std::vector<float>& o){for(UINT i=0;i<W*H;++i)assert(o[i*4+3]==.5f);};
 {auto o=run(scene_sky_gpu::build(right,up,forward,noon,W,H,3,1,1,1),Scene{});for(UINT i=0;i<W*H*4;i+=4)assert(o[i]==.1f||std::abs(o[i]-.1f)<1e-4f);alpha_kept(o);std::cout<<"Daylight unchanged\n";++cases;}
 {auto o=run(scene_sky_gpu::build(right,up,forward,midnight,W,H,3,1,1,1),Scene{});unsigned lit=0;float lowest=1;for(UINT i=0;i<W*H;++i){if(o[i*4+1]>.11f)++lit;lowest=std::min(lowest,o[i*4]);}std::cout<<"Night stars lit="<<lit<<" lowest="<<lowest<<'\n';assert(lit>20&&lowest>=.0999f);alpha_kept(o);++cases;}
 {float r2[4]{1,0,0,.05f},u2[4]{0,0,-1,.05f},f2[4]{0,1,0,0};auto o=run(scene_sky_gpu::build(r2,u2,f2,midnight,W,H,3,0,1,0),Scene{});float centre=o[(H/2*W+W/2)*4],corner=o[(2*W+2)*4];std::cout<<"Moon centre="<<centre<<" corner="<<corner<<'\n';assert(centre>1&&corner<.2f);
  Scene sea;sea.sea=[](UINT,UINT){return true;};o=run(scene_sky_gpu::build(r2,u2,f2,midnight,W,H,3,1,1,1),sea);for(UINT i=0;i<W*H*4;i+=4)assert(std::abs(o[i]-.1f)<1e-4f);std::cout<<"Ocean untouched\n";cases+=2;}
 {Scene ground;ground.depth=[](UINT,UINT){return .5f;};auto o=run(scene_sky_gpu::build(right,up,forward,midnight,W,H,3,1,1,1),ground);float facing=o[(H/2*W+W/2)*4];
  ground.normal[1]=-1;auto away=run(scene_sky_gpu::build(right,up,forward,midnight,W,H,3,1,1,1),ground);float back=away[(H/2*W+W/2)*4];std::cout<<"Moonlight facing="<<facing<<" away="<<back<<'\n';assert(facing>.12f&&std::abs(back-.1f)<1e-4f);++cases;}
 // Daytime sky: a sun disc where the sun is, and a cloud layer above the horizon only.
 {const float sun_colour[3]{1,.9f,.8f},sky_colour[3]{.3f,.4f,.6f};float r2[4]{1,0,0,.05f},u2[4]{0,0,-1,.05f},f2[4]{0,1,0,0};
  auto o=run(scene_sky_gpu::build(r2,u2,f2,noon,W,H,3,0,0,0,sun_colour,sky_colour,1,0),Scene{});float centre=o[(H/2*W+W/2)*4],corner=o[(2*W+2)*4];std::cout<<"Sun centre="<<centre<<" corner="<<corner<<'\n';assert(centre>5&&corner<1);alpha_kept(o);
  o=run(scene_sky_gpu::build(right,up,forward,noon,W,H,3,0,0,0,sun_colour,sky_colour,0,1.5f),Scene{});unsigned covered=0;for(UINT i=0;i<W*H;++i)if(std::abs(o[i*4]-.1f)>.01f)++covered;
  bool below=true;for(UINT x=0;x<W;++x)below=below&&std::abs(o[((H-1)*W+x)*4]-.1f)<1e-4f;std::cout<<"Clouds covered="<<covered<<" below horizon untouched="<<below<<'\n';assert(covered>W*H/20&&below);alpha_kept(o);cases+=2;}
 auto bad=scene_sky_gpu::build(right,up,forward,midnight,W,H,3,1,1,1);bad.params[0]=5;wait();assert(SUCCEEDED(allocator->Reset()));assert(SUCCEEDED(list->Reset(allocator.Get(),nullptr)));using I=scene_sky_gpu::Gpu::Input;assert(!gpu.record(list.Get(),nullptr,I{},I{},I{},I{},bad));assert(SUCCEEDED(list->Close()));
 std::cout<<"PASS: night sky "<<cases<<" D3D12 cases\n";CloseHandle(event);
}
