#include "effects/gi_gpu.h"
// Bounce light: after contact shadows, before volumetric lighting.
namespace scene_effects {
static scene_gi_gpu::Gpu gi_gpu;static uint32_t gi_frame_index{};
static void gi_bypass(int code,const char* why){gi_ready=false;error=code;auto n=++rejected;if(n==1||n%600==0)log_line(ANY_LOG_WARN,"framework",(std::string("SCENE_GI bypass=")+why).c_str());}
static void record_gi(void* renderer,void* scene){auto p=copy();if(!p.enabled||p.bounce_light<=0){gi_ready=false;return;}
 scene_effects_gpu::Native n;if(!scene_effects_gpu::capture(uintptr_t(renderer),uintptr_t(scene),n,safe_read_memory))return gi_bypass(500+int(n.failure),"native_frame_contract");
 volumetric::NativeTarget g[2];static constexpr uintptr_t offsets[2]={0x1f8,0x1e8};
 for(int i=0;i<2;++i){uintptr_t object{};if(!safe_read_memory(uintptr_t(renderer)+offsets[i],&object,8)||!volumetric::target(object,g[i],safe_read_memory)||g[i].width!=n.color.width||g[i].height!=n.color.height)return gi_bypass(510+i,"gbuffer_contract");}
 scene_gi_gpu::Frame f;double m[16]{};if(!safe_read_memory(uintptr_t(scene)+0x20+0x1a0,m,sizeof(m)))return gi_bypass(513,"camera");
 f.metrics={1.f/n.color.width,1.f/n.color.height,float(n.color.width),float(n.color.height)};
 for(int i=0;i<4;++i){f.right[i]=n.right[i];f.up[i]=n.up[i];f.forward[i]=n.forward[i];}f.forward[3]=5000;
 f.projection={float(m[10]),float(m[14]),float(m[11]),float(m[15])};f.params={2.5f,p.bounce_light,float(gi_frame_index%64),120};
 auto list=scene_antialiasing::command_list();auto saved=scene_antialiasing::states.find(list);
 if(!scene_antialiasing::tracking||!list||saved==scene_antialiasing::states.end()||!saved->second.ready()||n.color.state!=D3D12_RESOURCE_STATE_RENDER_TARGET)return gi_bypass(520,"command_bindings_or_target_state");
 if(!interface_valid(n.color.resource)||!interface_valid(n.depth.resource)||!interface_valid(g[0].resource)||!interface_valid(g[1].resource))return gi_bypass(521,"resources");
 std::lock_guard lock(platform::render_mutex);Microsoft::WRL::ComPtr<ID3D12Device> device,owner;auto target=(ID3D12Resource*)n.color.resource;
 if(!platform::queue||FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return gi_bypass(522,"device");
 for(auto r:{n.color.resource,n.depth.resource,g[0].resource,g[1].resource})if(FAILED(((ID3D12Resource*)r)->GetDevice(IID_PPV_ARGS(owner.ReleaseAndGetAddressOf())))||owner.Get()!=device.Get())return gi_bypass(522,"device");
 if(!gi_gpu.ensure(device.Get(),platform::queue.Get(),target->GetDesc()))return gi_bypass(523,"gpu_resources");
 auto input=[](const volumetric::NativeTarget& t){return scene_gi_gpu::Gpu::Input{(ID3D12Resource*)t.resource,D3D12_RESOURCE_STATES(t.state)};};
 auto bindings=saved->second;scene_antialiasing::owned=true;bool rendered=gi_gpu.record(list,target,input(n.depth),input(g[0]),input(g[1]),f);if(rendered)bindings.restore(list);scene_antialiasing::owned=false;
 if(!rendered)return gi_bypass(524,"gpu_record");
 ++gi_frame_index;gi_ready=true;gi_ms=float(gi_gpu.gpu_milliseconds());static uint64_t count{};
 if(++count==1||count%600==0)log_line(ANY_LOG_INFO,"framework",("SCENE_GI frames="+std::to_string(count)+" strength="+std::to_string(p.bounce_light)+" gpu_ms="+std::to_string(gi_ms.load())+" location=HDR_ADDITIVE_BEFORE_VOLUMETRICS").c_str());
}
}
