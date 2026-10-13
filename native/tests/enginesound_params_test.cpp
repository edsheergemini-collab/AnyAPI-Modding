#include "../enginesound_params.h"
#include <cassert>
#include <climits>
#include <cmath>
using namespace enginesound;
int main(){
 // Slider fields don't overlap and use exactly the 31 payload bits.
 uint32_t used=0;for(auto& s:kSliders){uint32_t m=uint32_t((1<<s.bits)-1)<<s.shift;assert((used&m)==0);used|=m;}assert(used==0x7fffffffu);
 // Packed tunes round-trip, are always negative and never collide with an index.
 Params p=neutral();int32_t n=pack(p);assert(is_packed(n)&&n<0);
 for(int s=0;s<kSliderCount;++s)assert(unpack(n).v[s]==kSliders[s].neutral);
 for(int s=0;s<kSliderCount;++s){Params q=neutral();q.v[s]=uint8_t(max_step(s));auto r=unpack(pack(q));for(int t=0;t<kSliderCount;++t)assert(r.v[t]==q.v[t]);}
 Params zero{};assert(pack(zero)==INT32_MIN&&is_packed(INT32_MIN));
 Params all{};for(int s=0;s<kSliderCount;++s)all.v[s]=uint8_t(max_step(s));assert(pack(all)==-1);
 Params over=neutral();over.v[kPitch]=200;assert(unpack(pack(over)).v[kPitch]==max_step(kPitch));
 // Decoding: vanilla stays vanilla, presets and Custom map to their own params, junk falls back.
 for(int32_t i=kVanillaMin;i<=kVanillaMax;++i)assert(decode(i).mode==Mode::Vanilla);
 for(int32_t i=0;i<kPresetCount;++i){auto c=decode(kFirstPreset+i);assert(c.mode==Mode::Preset&&c.preset==i);}
 assert(kCustomIndex==kFirstPreset+kPresetCount&&decode(kCustomIndex).mode==Mode::Custom);
 assert(decode(kCustomIndex+1).mode==Mode::Vanilla&&decode(0x7fffffff).mode==Mode::Vanilla&&decode(0x40000000).mode==Mode::Vanilla);
 Params c=neutral();c.v[kVolume]=3;assert(decode(pack(c)).mode==Mode::Custom&&decode(pack(c)).params.v[kVolume]==3);
 // Every preset survives packing, so "Customize" reproduces it exactly.
 for(auto& pr:kPresets){auto r=unpack(pack(pr.params));for(int s=0;s<kSliderCount;++s)assert(r.v[s]==pr.params.v[s]&&pr.params.v[s]<=max_step(s));}
 // Neutral Custom is exactly stock sound 1: unit multipliers everywhere, no extra layers.
 Params nn=neutral();assert(main_effect(nn)==kEngineHighA&&low_effect(nn)==kEngineBaseA);
 for(double r=0;r<=1;r+=.1)for(double l=0;l<=1;l+=.5){assert(std::abs(pitch_multiplier(nn,r,l)-1)<1e-12);assert(std::abs(body_pitch_multiplier(nn,r)-1)<1e-12);assert(load_gain(nn,l)==1);}
 assert(std::abs(volume(nn)-1)<1e-12&&std::abs(body_layer(nn)-1)<1e-12&&smoothing_seconds(nn)==0&&crackle_rate(nn)==0);
 Layer layer;assert(!induction_layer(nn,.5,.5,.5,layer));auto lp=lope(nn,0,1.234);assert(lp.volume==1&&lp.pitch==1);
 // Rev range widens pitch at high rpm only; curve shapes the middle; type ratio scales everything.
 Params wide=neutral();wide.v[kRange]=7;assert(std::abs(pitch_multiplier(wide,0)-1)<1e-12&&std::abs(pitch_multiplier(wide,1)-2)<1e-12);
 Params steep=wide;steep.v[kCurve]=3;assert(pitch_multiplier(steep,.5)<pitch_multiplier(wide,.5));
 assert(std::isfinite(pitch_multiplier(wide,NAN))&&pitch_multiplier(wide,5)==pitch_multiplier(wide,1));
 Params v8=neutral();v8.v[kType]=6;assert(std::abs(pitch_multiplier(v8,0)-kTypes[6].main_ratio)<1e-12&&main_effect(v8)==kEngineHighC);
 // Throttle response: quieter off throttle, louder on it, symmetric around 1.
 Params lr=neutral();lr.v[kLoad]=3;assert(load_gain(lr,0)==.5&&load_gain(lr,1)==1.5);
 // Cam lope pulses at idle, stays within bounds and fades out by 40% revs.
 Params cam=neutral();cam.v[kType]=6;cam.v[kLope]=7;double lo=1,hi=0;
 for(double t=0;t<3;t+=.005){auto l=lope(cam,0,t);lo=std::min(lo,l.volume);hi=std::max(hi,l.volume);assert(l.volume>=.4-1e-9&&l.volume<=1+1e-9&&l.pitch>.94&&l.pitch<1.06);}
 assert(hi-lo>.3);for(double t=0;t<1;t+=.01){auto l=lope(cam,.4,t);assert(l.volume==1&&l.pitch==1);}
 Params ev=cam;ev.v[kType]=15;assert(lope(ev,0,.3).volume==1);
 // Induction layers: turbo follows spool, supercharger follows revs, both silent at rest.
 Params tb=neutral();tb.v[kInduction]=1;tb.v[kBoost]=3;assert(induction_layer(tb,1,1,0,layer)&&layer.volume==0);
 assert(induction_layer(tb,1,1,1,layer)&&layer.effect==kTurbineCompressorLoop&&std::abs(layer.volume-1.1)<1e-12&&layer.speed>2);
 Params sc=tb;sc.v[kInduction]=2;assert(induction_layer(sc,0,1,1,layer)&&layer.volume==0&&layer.effect==kFluidGasSupercharger);
 assert(induction_layer(sc,1,1,0,layer)&&layer.volume>1);
 Params tt=tb;tt.v[kInduction]=3;assert(induction_layer(tt,1,1,1,layer)&&layer.effect==kTurbineCompressorLoopB&&spool_seconds(3)<spool_seconds(1));
 assert(spool_target(0,1)==0&&spool_target(1,1)==1&&spool_target(1,0)==.25);
 // Smoothing converges, passes through when off and ignores bad dt.
 assert(smooth(0,1,0,.016)==1);double v=0;for(int i=0;i<600;++i)v=smooth(v,1,.2,.016);assert(std::abs(v-1)<1e-6);
 assert(smooth(.3,1,.2,0)==1&&smooth(NAN,1,.2,.016)==1);
 assert(clamp_speed(NAN)==1&&clamp_speed(100)==4&&clamp_volume(-1)==0&&clamp_volume(NAN)==0);
 // Realism layer: off for stock types (neutral Custom stays stock), on for the others.
 assert(!realism(nn)&&startup_flare(nn,.2)==1&&rev_limiter(nn,1,1,.01).cut==0&&firing_wobble(nn,1,.3).pitch==1&&overrun_body_gain(nn,1,0)==1&&crackle_burst_size(nn,.5)==0);
 assert(smoothing_seconds(nn,false)==0);
 Params i6=neutral();i6.v[kType]=4;assert(realism(i6)&&smoothing_seconds(i6,true)<smoothing_seconds(i6,false));
 i6.v[kSmoothing]=1;assert(smoothing_seconds(i6,true)>.1&&smoothing_seconds(i6,false)>smoothing_seconds(i6,true));
 // Start-up flare rises fast, peaks near 1+flare and settles back to 1.
 double peak=1;for(double t=0;t<2.5;t+=.01){double f=startup_flare(v8,t);assert(f>=1&&f<=1+kTypes[6].flare+1e-9);peak=std::max(peak,f);}
 assert(peak>1+.8*kTypes[6].flare&&std::abs(startup_flare(v8,2.4)-1)<.01&&startup_flare(v8,-1)==1&&startup_flare(v8,NAN)==1);
 assert(startup_flare(ev,.2)==1);
 // Rev limiter only bites at the top under load, and cuts in periodic bursts.
 assert(rev_limiter(v8,.9,1,.01).cut==0&&rev_limiter(v8,1,.2,.01).cut==0);
 int cuts=0;for(double t=0;t<1;t+=.001){auto l=rev_limiter(v8,1,1,t);assert(l.volume>=.5-1e-9&&l.volume<=1&&l.pitch>=.96-1e-9);cuts+=l.cut>.5;}
 assert(cuts>50&&cuts<500&&rev_limiter(ev,1,1,.01).cut==0);
 // Wobble is small and bounded; overrun lifts the body only when coasting at revs.
 for(double t=0;t<2;t+=.01){auto w=firing_wobble(v8,1,t);assert(std::abs(w.pitch-1)<.0061&&std::abs(w.volume-1)<.061);}
 assert(overrun_body_gain(v8,1,0)>1.1&&overrun_body_gain(v8,1,1)==1&&overrun_body_gain(v8,0,0)==1);
 // Crackle bursts grow with the level.
 Params cr=v8;for(int lv=1;lv<4;++lv){cr.v[kCrackle]=uint8_t(lv);int a=crackle_burst_size(cr,0),b=crackle_burst_size(cr,.999);assert(a==1+lv&&b==1+lv+lv&&b>=a);}
 return 0;}
