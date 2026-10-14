#pragma once
// Call cells of bindings reached through a call route, tolerant of a route root that matches twice.
//
// The SDK's cell_of() refuses when the code signature of a route's root matches more than once. On the laptop
// some sessions have a second copy of client_scene.create's code in memory (the root of the definition
// functions' routes), so mods adding parts found nothing and stayed off. Here every match is followed, and the
// cell is taken when all the matches that lead to a live function agree on it. (Rotary carries the same fix.)
#include <windows.h>
#include "anyapi_experimental.hpp"

namespace route {

inline bool executable(const void* p) {
    MEMORY_BASIC_INFORMATION r{};
    const DWORD x = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return p && VirtualQuery(p, &r, sizeof(r)) && r.State == MEM_COMMIT && (r.Protect & x) && !(r.Protect & PAGE_GUARD);
}

// The binding's call cell, or nullptr. `roots`, if given, gets how many times the route's root matched (-1 when
// the SDK's own lookup already worked or the binding has no route).
inline void** cell(const anymaker::func_desc& d, int* roots = nullptr) {
    if (roots) *roots = -1;
    if (!anymaker::experimental::matching_build()) return nullptr;
    if (void** c = anymaker::cell_of(d)) return c;
    const anymaker::slot_route& r = d.call_route;
    if (!r.nhops || r.nhops > 8 || !r.root_sig || !*r.root_sig) return nullptr;
    uint8_t* hits[8];
    size_t n = anymaker::find_gcl_functions(r.root_sig, hits, 8);
    if (roots) *roots = int(n);
    void** found = nullptr;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* fn = hits[i];
        void** c = nullptr;
        for (uint8_t h = 0; fn && h < r.nhops; ++h) {
            c = reinterpret_cast<void**>(anymaker::read_slot(fn, r.hops[h]));
            fn = anymaker::readable(c, 8) ? reinterpret_cast<uint8_t*>(*c) : nullptr;
        }
        if (!fn || !executable(fn)) continue;
        if (found && found != c) return nullptr;   // the copies disagree: refuse rather than guess
        found = c;
    }
    return found;
}
// The function a binding's route leads to, or nullptr. Tries the SDK's own resolve first (unique code first).
inline void* function(const anymaker::func_desc& d, int* roots = nullptr) {
    if (roots) *roots = -1;
    if (!anymaker::experimental::matching_build()) return nullptr;
    if (void* f = anymaker::resolve(d)) return f;
    void** c = cell(d, roots);
    return c ? *c : nullptr;
}

}  // namespace route
