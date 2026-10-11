#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <algorithm>
#include "effects/sky_frame.h"
using Microsoft::WRL::ComPtr;
// Compiles the night sky shaders, checks constant size and sampler
// slots, and optionally writes the bytecode header used by the runtime.
int wmain(int argc,wchar_t** argv){assert(argc==2||argc==3);std::ofstream out;if(argc==3){out.open(argv[2],std::ios::binary);assert(out);out<<"#pragma once\nnamespace sky_bytecode {\n";}
 for(auto entry:{"Vertex","Sky"}){ComPtr<ID3DBlob> code,error;auto hr=D3DCompileFromFile(argv[1],nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,entry,entry[0]=='V'?"vs_5_0":"ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
  if(FAILED(hr)){if(error)std::cerr<<(const char*)error->GetBufferPointer();return 1;}
  ComPtr<ID3D11ShaderReflection> r;assert(SUCCEEDED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&r))));D3D11_SHADER_DESC sd{};r->GetDesc(&sd);
  for(UINT i=0;i<sd.BoundResources;++i){D3D11_SHADER_INPUT_BIND_DESC b{};r->GetResourceBindingDesc(i,&b);if(b.Type==D3D_SIT_SAMPLER)assert(b.BindPoint==(!strcmp(b.Name,"point_sampler")?1u:0u));if(b.Type==D3D_SIT_TEXTURE)assert(b.BindPoint<4);}
  if(entry[0]!='V'){D3D11_SHADER_BUFFER_DESC b{};assert(SUCCEEDED(r->GetConstantBufferByName("Frame")->GetDesc(&b))&&b.Size==sizeof(scene_sky_gpu::Frame));}
  if(out){out<<"inline constexpr unsigned char "<<entry<<"[]={";auto bytes=(const unsigned char*)code->GetBufferPointer();for(size_t i=0;i<code->GetBufferSize();++i)out<<unsigned(bytes[i])<<',';out<<"};\n";}}
 if(out)out<<"}\n";
 // Day/night: no night at noon, full night at midnight; the moon sits opposite the sun and is high at midnight.
 assert(scene_sky_gpu::night_factor(.8f)==0&&scene_sky_gpu::night_factor(.05f)==0&&scene_sky_gpu::night_factor(-.5f)==1&&scene_sky_gpu::night_factor(-.03f)>0&&scene_sky_gpu::night_factor(-.03f)<1);
 float midnight[3]={0,-1,0},dusk[3]={0,0,1};auto m=scene_sky_gpu::moon_direction(midnight);assert(m[1]>.99f);m=scene_sky_gpu::moon_direction(dusk);assert(m[2]<-.9f&&m[1]>0);
 float right[4]{1,0,0,.8f},up[4]{0,1,0,.45f},forward[4]{0,0,1,0};auto f=scene_sky_gpu::build(right,up,forward,midnight,64,64,12.5f,1,1,1);assert(scene_sky_gpu::valid(f)&&f.sun[3]==1&&f.moon[3]==1&&std::abs(f.celestial[0]*f.celestial[0]+f.celestial[1]*f.celestial[1]-1)<1e-5f);
 float noon[3]={0,1,0};f=scene_sky_gpu::build(right,up,forward,noon,64,64,0,1,1,1);assert(scene_sky_gpu::valid(f)&&f.sun[3]==0&&f.moon[3]==0);
 auto bad=f;bad.params[1]=3;assert(!scene_sky_gpu::valid(bad));bad=f;bad.sun[3]=NAN;assert(!scene_sky_gpu::valid(bad));bad=f;bad.metrics[2]=4;assert(!scene_sky_gpu::valid(bad));
 std::cout<<"PASS: night sky shaders compile; constants, sampler slots, night factor and moon placement validated\n";
}
