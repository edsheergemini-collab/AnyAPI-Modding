// AnyEngineParts: engine parts for Anymaker in one mod. It carries two parts sets, each in its own file:
//   - turbo_mod.cpp: exhaust-driven turbochargers (2x2x2 to 5x5x2), inline blow-off valves and a wastegate with
//     antilag (docs/mods/turbo.md);
//   - rotary_mod.cpp: a stretchable 1-4 rotor rotary engine with its own sound (docs/mods/rotary.md).
// They keep their part ids, so vehicles built with the old Turbo and Rotary prototypes load unchanged, and they log
// under their own tags ("turbo", "rotary"). Each hooks and unhooks on its own: one failing to hook leaves the
// other working.
//
// EXPERIMENTAL. Both hook game functions through the experimental SDK: Anymaker 0.1.25 / Steam build 25904830 only.
#define NOMINMAX
#include <windows.h>
#include "anyapi_services_v1.h"
#include <thread>

namespace turbo { void init(const AnyModHostV1& h); void start(); void shutdown(); }
namespace rotary { void init(const AnyModHostV1& h); void start(); void shutdown(); }

extern "C" __declspec(dllexport) bool AnyAPI_ModInit(const AnyModHostV1* h, AnyModCallbacksV1* out) {
    if (!h || !out || h->struct_size != sizeof(*h) || h->abi != ANYAPI_MOD_ABI || out->struct_size != sizeof(*out)) return false;
    turbo::init(*h);
    rotary::init(*h);
    out->id = "anyengineparts";
    out->shutdown = [](void*) { turbo::shutdown(); rotary::shutdown(); };
    return true;
}
extern "C" __declspec(dllexport) void AnyAPI_ModReady() {
    // Resolving scans game memory: keep it off the loader and frame threads. One thread, one part after the other,
    // because both hook some of the same game functions and a cell must not be hooked from two threads at once.
    std::thread([] { turbo::start(); rotary::start(); }).detach();
}
