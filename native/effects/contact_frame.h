#pragma once
#include <array>
#include <cmath>
// Root constants of the contact shadow pass; layout matches cbuffer Frame in contact.hlsl.
namespace scene_contact_gpu {
struct Frame {std::array<float,4> metrics{},right{},up{},forward{},projection{},sun{},sun_colour{},sky_colour{},back_colour{},params{};};
static_assert(sizeof(Frame)==40*4);
inline bool valid(const Frame& f){for(auto* a:{&f.metrics,&f.right,&f.up,&f.forward,&f.projection,&f.sun,&f.sun_colour,&f.sky_colour,&f.back_colour,&f.params})for(auto v:*a)if(!std::isfinite(v))return false;
 for(int i=0;i<3;++i)if(f.sun_colour[i]<0||f.sun_colour[i]>10000||f.sky_colour[i]<0||f.sky_colour[i]>10000||f.back_colour[i]<0||f.back_colour[i]>10000)return false;
 return f.metrics[2]>=16&&f.metrics[3]>=16&&f.right[3]>0&&f.right[3]<10&&f.up[3]>0&&f.up[3]<10&&f.forward[3]>=1&&f.sun[3]>=0&&f.sun[3]<=1&&f.params[0]>=.05f&&f.params[0]<=4&&f.params[1]>=.01f&&f.params[1]<=2&&std::abs(f.projection[0]*f.projection[3]-f.projection[1]*f.projection[2])>1e-8f;}
}
