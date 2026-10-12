#pragma once
// ThirdPerson game bindings, generated for Anymaker 0.1.24 / Steam build 25826614 with
//   python tools/bind.py --reference reference --signature "<signature>" --out <file>.hpp
// run against the 0.1.24 SDK reference, one call per signature below.
// Experimental: located by code signature, not reviewed as an AnyAPI service.
// Regenerate and re-test after any game update.
#include "anymaker_sdk_runtime.hpp"
namespace thirdperson::bind {
using anymaker::func_desc;
// (mat34) client_scene.get_camera_transform (const client_scene) | side: client; threads: main, worker
// Virtual slot 16 of client_scene; the cell hook covers direct and virtual calls.
inline constexpr func_desc client_scene_get_camera_transform{"(mat34) client_scene.get_camera_transform (const client_scene)","53 55 56 48 81 ec e0 01 00 00 48 89 8c 24 80 00 00 00 48 89 94 24 88 00 00 00 48 8d 84 24 90 00",{"", {}, 0},"53 48 83 ec 30 48 89 4c 24 20 48 8d 01 48 8b 15 cc 08 00 00 48 89 4c 24 28 48 89 c1 ff 12 48 8b",2280u,16};
} // namespace thirdperson::bind
