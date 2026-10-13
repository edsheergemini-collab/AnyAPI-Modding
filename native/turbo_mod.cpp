// Turbo: exhaust-driven turbochargers in four sizes (2x2x2 to 5x5x2), built from the stock impeller.
//
// Each size is a new part of the stock impeller's class (gas_pump_centrifugal). The impeller already has two gas
// nodes, the compressor inlet and outlet, and pumps between their two gas contents. The mod gives each turbo a
// hot side: two more gas contents and nodes (turbine inlet, turbine exhaust out), created with the game's own
// functions and registered in the vehicle's gas node container, so pipes, islands and pressure all treat them
// like any other gas port. Each server tick the turbo moves exhaust through its hot side, spools its shaft from
// that flow (with lag that grows with size) and runs its compressor at the spool's head. The engine's existing
// boost does the rest: intake pressure up to 10 (the game's "98 kPa") doubles engine power.
// The model and its numbers are in turbo_logic.h; the meshes are made from turbo_a.mesh (turbo_mesh.h).
//
// Getting it: tech_tier 3 loot in the "gas" category, like the stock impellers. Single player / host only for
// now: the definitions are added on this game's server and client scene, and every player would need the mod.
// The hot-side gas is not saved; it refills from the pipes after loading.
//
// EXPERIMENTAL. Hooks game functions through the experimental SDK (turbo_bindings.h): Anymaker 0.1.24 /
// Steam build 25826614 only. First runs are diagnostic: read anymaker_modding.log.
#define NOMINMAX
#include <windows.h>
#include "anyapi_services_v1.h"
#include "anyapi_experimental.hpp"
#include "turbo_bindings.h"
#include "turbo_logic.h"
#include "turbo_mesh.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace turbo {
namespace {
constexpr const char* kModId = "turbo";

AnyModHostV1 host;
std::atomic<bool> g_ready{false}, g_stop{false};

void log(int level, const char* fmt, ...) {
    if (!host.log) return;
    char buf[640];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    host.log(uint32_t(level), kModId, buf);
}

// Reference layouts (metadata, Anymaker 0.1.24).
namespace off {
constexpr ptrdiff_t component_id = 64;               // server_scene.vehicle_component.m_id (s32)
constexpr ptrdiff_t component_definition = 312;      // m_definition (ptr<const vehicle_component_definition>)
constexpr ptrdiff_t definition_id = 0;               // vehicle_component_definition.m_id (string)
constexpr ptrdiff_t pump_factor_speed = 480;         // gas_pump_centrifugal.m_factor_speed (replicated, drives the client's sound)
constexpr ptrdiff_t pump_content_0 = 528;            // m_content_0: compressor inlet
constexpr ptrdiff_t pump_content_1 = 584;            // m_content_1: compressor outlet
constexpr ptrdiff_t pump_pressure = 872;             // m_pressure (head; shown in the stock tooltip)
constexpr ptrdiff_t pump_display_power = 888;        // m_display_power
constexpr ptrdiff_t pump_flow = 896;                 // m_flow (litres this tick)
constexpr ptrdiff_t vehicle_gas_nodes = 2040;        // server_scene.vehicle.m_nodes_gas (vehicle_gas_node.container)
constexpr ptrdiff_t content_volume = 32;             // vehicle_gas_content.m_volume (f64, litres)
constexpr ptrdiff_t content_capacity = 40;           // m_capacity (f64, litres)
constexpr ptrdiff_t content_temperature = 48;        // m_temperature (f64, C)
constexpr ptrdiff_t node_node_index = 20;            // vehicle_gas_node.m_node_index (s32; -1 when not found)
constexpr size_t content_size = 56, node_size = 64, definition_file_size = 40;
constexpr int container_create_node_slot = 3, container_destroy_node_slot = 5;   // vehicle_gas_node.container method table
}  // namespace off
constexpr int32_t kStoreSystem = 1;                  // file.e_store.system (as GpsPart)
constexpr int32_t kGasAir = 0;                       // server_scene.e_vehicle_gas_type.air

template <class T> T& at(void* base, ptrdiff_t offset) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset); }
template <class T> const T& at(const void* base, ptrdiff_t offset) { return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + offset); }

// ------------------------------------------------------------------------------------- game functions
using self_fn_t = void (*)(void* self);
using content_create_t = void (*)(void* content, void* vehicle, const void* component, const int32_t* layer, const double* capacity_factor);
using content_modify_t = void (*)(void* content, const int32_t* type, const double* amount, const double* temperature);
using content_move_t = void (*)(void* from, void* to, const double* volume);
using create_node_t = void (*)(void* container, void* node, void* component, void* const* component_flood, const int32_t* index, void* const* content);
using destroy_node_t = void (*)(void* container, void* node);
using property_set_t = void (*)(void* property, const double* value);
using string_ctor_t = void (*)(anymaker::gc_string_view*, const char*);
using get_definition_t = void (*)(const void** ret, const void* container, const anymaker::gc_string_view* id);

self_fn_t g_content_ctor, g_content_dtor, g_node_ctor, g_node_dtor;
content_create_t g_content_create;
content_modify_t g_content_modify;
content_move_t g_content_move;
create_node_t g_create_node_bound;   // located by code; cross-checked against the container's method table
property_set_t g_property_set;
string_ctor_t g_string_ctor;
get_definition_t g_get_definition;
const double* g_delta_time;

// ------------------------------------------------------------------------------------- per-turbo state
// The hot side's gas lives in memory the mod owns; the game holds pointers to these nodes and contents in the
// vehicle's gas node container until the component is destroyed. Process heap, so it outlives the DLL.
struct HotSide {
    alignas(16) uint8_t content[2][off::content_size];
    alignas(16) uint8_t node[2][off::node_size];
};
struct Turbo {
    int size = 0;
    Spec spec;
    HotSide* hot = nullptr;
    State state;
    double last_log = -1e9;
    int ticks = 0;
};
std::unordered_map<const void*, Turbo> g_turbos;     // keyed by server component; nodes keep their address
std::unordered_map<const void*, int> g_definition_size;   // definition pointer -> 2-5, or 0 for other parts
std::recursive_mutex g_mutex;                        // tick, create, destroy and data lookups (recursive: game calls made
                                                     // under it may come back through our hooks)

double now_seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

int turbo_size(const void* component) {
    if (!component || !anymaker::readable(static_cast<const uint8_t*>(component) + off::component_definition, 8)) return 0;
    const void* definition = at<const void*>(component, off::component_definition);
    if (!definition) return 0;
    auto found = g_definition_size.find(definition);
    if (found != g_definition_size.end()) return found->second;
    int n = 0;
    if (anymaker::readable(static_cast<const uint8_t*>(definition) + off::definition_id, sizeof(anymaker::gc_string_view))) {
        auto& id = at<anymaker::gc_string_view>(definition, off::definition_id);
        if (id.data && id.length > 0 && id.length < 64 && anymaker::readable(id.data, size_t(id.length))) n = size_from_id(id.data, size_t(id.length));
    }
    g_definition_size[definition] = n;
    return n;
}

void* container_of(void* vehicle) { return static_cast<uint8_t*>(vehicle) + off::vehicle_gas_nodes; }
void* container_method(void* container, int slot) {
    if (!container || !anymaker::readable(container, 8)) return nullptr;
    auto ti = anymaker::typeinfo_of(container);
    if (!ti.p || !anymaker::readable(ti.p + 0x80, 8) || !ti.table() || !anymaker::readable(ti.table() + slot, 8)) return nullptr;
    return ti.method(slot);
}

double pressure_of(const void* content) {
    double capacity = at<double>(content, off::content_capacity);
    return capacity > 0 ? at<double>(content, off::content_volume) / capacity : 1.0;
}

// Builds the hot side: two contents sized like the impeller's own, filled with ambient air, and two gas nodes
// for logic nodes 2 and 3 (gas ordinals 2 and 3 of the definition).
HotSide* create_hot_side(void* component, void* vehicle) {
    void* container = container_of(vehicle);
    auto create_node = reinterpret_cast<create_node_t>(container_method(container, off::container_create_node_slot));
    if (!create_node) { log(2, "gas node container not readable; this turbo has no hot side"); return nullptr; }
    static std::atomic<bool> checked{false};
    if (!checked.exchange(true) && g_create_node_bound && reinterpret_cast<void*>(create_node) != reinterpret_cast<void*>(g_create_node_bound))
        log(1, "container create_node slot (%p) differs from the located function (%p); using the slot", reinterpret_cast<void*>(create_node), reinterpret_cast<void*>(g_create_node_bound));
    auto* hot = static_cast<HotSide*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HotSide)));
    if (!hot) return nullptr;
    for (int i = 0; i < 2; ++i) {
        g_content_ctor(hot->content[i]);
        g_node_ctor(hot->node[i]);
        int32_t layer = 0; double factor = 0.25;
        g_content_create(hot->content[i], vehicle, component, &layer, &factor);
        double amount = at<double>(hot->content[i], off::content_capacity), temperature = kAmbientC;
        int32_t air = kGasAir;
        if (amount > 0) g_content_modify(hot->content[i], &air, &amount, &temperature);
        void* flood = nullptr; int32_t index = kHotIn + i; void* content = hot->content[i];
        create_node(container, hot->node[i], component, &flood, &index, &content);
    }
    if (at<int32_t>(hot->node[0], off::node_node_index) < 0 || at<int32_t>(hot->node[1], off::node_node_index) < 0)
        log(2, "the game did not find the turbo's hot-side logic nodes (indices %d, %d)", at<int32_t>(hot->node[0], off::node_node_index), at<int32_t>(hot->node[1], off::node_node_index));
    return hot;
}

void destroy_hot_side(HotSide* hot, void* vehicle) {
    if (!hot) return;
    void* container = vehicle ? container_of(vehicle) : nullptr;
    auto destroy_node = reinterpret_cast<destroy_node_t>(container_method(container, off::container_destroy_node_slot));
    if (!destroy_node) { log(2, "gas node container not readable at destroy; leaking a turbo hot side rather than risk a crash"); return; }
    for (int i = 1; i >= 0; --i) destroy_node(container, hot->node[i]);
    for (int i = 0; i < 2; ++i) { g_node_dtor(hot->node[i]); g_content_dtor(hot->content[i]); }
    HeapFree(GetProcessHeap(), 0, hot);
}

// ------------------------------------------------------------------------------------- impeller hooks
using create_t = void (*)(void* self, void* vehicle);
using destroy_t = void (*)(void* self, void* vehicle, void* scene);
using tick_t = void (*)(void* self, void* vehicle, void* scene);
using get_f64_t = void (*)(double** ret, void* self, const anymaker::gc_string_view* name);
anymaker::cell_hook h_create, h_destroy, h_tick, h_get_f64;

void hk_create(void* self, void* vehicle) {
    reinterpret_cast<create_t>(h_create.original)(self, vehicle);
    if (!self || !vehicle) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    int n = turbo_size(self);
    if (!n) return;
    Turbo& t = g_turbos[self];
    if (t.hot) return;
    t.size = n; t.spec = spec(n);
    t.hot = create_hot_side(self, vehicle);
    static std::atomic<int> logs{8};
    if (logs.fetch_sub(1) > 0 && t.hot)
        log(0, "Turbo %dx%d created (component %d): hot side nodes %d/%d, contents %.2f/%.2f L, impeller contents %.2f/%.2f L", n, n, at<int32_t>(self, off::component_id),
            at<int32_t>(t.hot->node[0], off::node_node_index), at<int32_t>(t.hot->node[1], off::node_node_index), at<double>(t.hot->content[0], off::content_capacity),
            at<double>(t.hot->content[1], off::content_capacity), at<double>(self, off::pump_content_0 + off::content_capacity), at<double>(self, off::pump_content_1 + off::content_capacity));
}

void hk_destroy(void* self, void* vehicle, void* scene) {
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto found = g_turbos.find(self);
        if (found != g_turbos.end()) { destroy_hot_side(found->second.hot, vehicle); g_turbos.erase(found); }
    }
    reinterpret_cast<destroy_t>(h_destroy.original)(self, vehicle, scene);
}

void hk_tick(void* self, void* vehicle, void* scene) {
    if (!self) return;
    std::unique_lock<std::recursive_mutex> lock(g_mutex);
    if (!turbo_size(self)) { lock.unlock(); reinterpret_cast<tick_t>(h_tick.original)(self, vehicle, scene); return; }
    auto found = g_turbos.find(self);
    if (found == g_turbos.end()) {                  // placed before the hooks were in: no hot side, no boost
        static std::atomic<int> logs{4};
        if (logs.fetch_sub(1) > 0) log(1, "a turbo without a hot side is ticking (it existed before Turbo was ready); rebuild or reload it");
        return;
    }
    Turbo& t = found->second;
    void* c0 = static_cast<uint8_t*>(self) + off::pump_content_0;
    void* c1 = static_cast<uint8_t*>(self) + off::pump_content_1;
    void* contents[kNodeCount] = {c0, c1, t.hot ? t.hot->content[0] : nullptr, t.hot ? t.hot->content[1] : nullptr};
    Inputs in;
    for (int i = 0; i < kNodeCount; ++i) {
        in.pressure[i] = contents[i] ? pressure_of(contents[i]) : 1.0;
        in.volume[i] = contents[i] ? at<double>(contents[i], off::content_volume) : 0.0;
    }
    if (contents[kHotIn]) in.exhaust_temperature = at<double>(contents[kHotIn], off::content_temperature);
    in.dt = g_delta_time && std::isfinite(*g_delta_time) ? *g_delta_time : 1.0 / 60.0;
    Step o = step(t.state, t.spec, in);
    if (t.hot) {
        if (o.turbine_move > 0) g_content_move(contents[kHotIn], contents[kHotOut], &o.turbine_move);
        else if (o.turbine_move < 0) { double v = -o.turbine_move; g_content_move(contents[kHotOut], contents[kHotIn], &v); }
    }
    if (o.compressor_move > 0) g_content_move(c0, c1, &o.compressor_move);
    else if (o.compressor_move < 0) { double v = -o.compressor_move; g_content_move(c1, c0, &v); }
    at<double>(self, off::pump_pressure) = o.head;
    at<double>(self, off::pump_display_power) = o.power;
    at<double>(self, off::pump_flow) = o.compressor_move;
    double sound = t.state.spool;
    g_property_set(static_cast<uint8_t*>(self) + off::pump_factor_speed, &sound);
    // Diagnostics for tuning: the first tick, then every 5 s per turbo while gas moves or any port is off ambient,
    // up to 300 lines a session.
    static std::atomic<int> logs{300};
    double now = now_seconds();
    bool off_ambient = false;
    for (int i = 0; i < kNodeCount; ++i) off_ambient = off_ambient || std::abs(in.pressure[i] - 1.0) > 0.05;
    if ((t.ticks++ == 0 || ((t.state.exhaust_flow > 0.05 || t.state.spool > 0.01 || off_ambient) && now - t.last_log >= 5.0)) && logs.fetch_sub(1) > 0) {
        t.last_log = now;
        log(0, "Turbo %dx%d id %d tick %d: hot in %.2f (%.2f L, %.0f C) -> hot out %.2f (%.2f L), exhaust %.1f L/s, spool %.2f, head %.2f, cold in %.2f (%.2f L) -> cold out %.2f (%.2f L), intake %.1f L/s (power x%.2f), dt %.4f",
            t.size, t.size, at<int32_t>(self, off::component_id), t.ticks, in.pressure[kHotIn], in.volume[kHotIn], in.exhaust_temperature, in.pressure[kHotOut], in.volume[kHotOut],
            t.state.exhaust_flow, t.state.spool, o.head, in.pressure[kColdIn], in.volume[kColdIn], in.pressure[kColdOut], in.volume[kColdOut], t.state.intake_flow,
            power_multiplier(in.pressure[kColdOut]), in.dt);
    }
}

void hk_get_f64(double** ret, void* self, const anymaker::gc_string_view* name) {
    reinterpret_cast<get_f64_t>(h_get_f64.original)(ret, self, name);
    if (!ret || *ret || !self || !name) return;
    auto view = name->view();
    int i = channel_index(view.data(), view.size());
    if (i < 0) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto found = g_turbos.find(self);
    if (found == g_turbos.end()) return;
    *ret = &found->second.state.slots[i];
    static std::atomic<int> logs{8};
    if (logs.fetch_sub(1) > 0) log(0, "data channel %s connected", kChannels[i].name);
}

// ------------------------------------------------------------------------------------- part definitions
// GamePath = file.path { e_store m_store; string m_path; } (24 bytes)
struct GamePath { int32_t store; uint32_t pad; anymaker::gc_string_view path; };
static_assert(sizeof(GamePath) == 24, "file.path");
using add_definitions_t = void (*)(void* container, const void* file);
using file_load_t = void (*)(bool* ret, void* file, const void* path);
anymaker::cell_hook h_add_definitions;
self_fn_t g_file_ctor, g_file_dtor;
file_load_t g_file_load;
std::string g_json_path;                            // absolute, backslashes

std::vector<uint8_t> read_file(const std::wstring& path) {
    std::vector<uint8_t> data;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 24)) {
        data.resize(size_t(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, data.data(), DWORD(data.size()), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(h);
    return data;
}
bool write_file(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data, DWORD(size), &written, nullptr) && written == size;
    CloseHandle(h);
    return ok;
}

// Writes the four meshes into rom/meshes/components (where the game loads part meshes) and the definitions file
// into "AnyAPI and Modding/Turbo".
bool write_definition_file() {
    HMODULE self = nullptr;
    wchar_t dll[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&write_definition_file, &self) ||
        !GetModuleFileNameW(self, dll, MAX_PATH)) return false;
    std::wstring dir(dll);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));      // ...\AnyAPI and Modding\mods
    dir = dir.substr(0, dir.find_last_of(L"\\/"));      // ...\AnyAPI and Modding
    std::wstring game_dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring components = game_dir + L"\\rom\\meshes\\components\\";
    std::vector<uint8_t> stock = read_file(components + L"turbo_a.mesh");
    bool built[kMaxSize - kMinSize + 1]{};
    for (int n = kMinSize; n <= kMaxSize; ++n) {
        std::string id = definition_id(n);
        std::vector<uint8_t> mesh = build_turbo_mesh(stock, n, id.c_str());
        std::wstring file = components + L"turbo_" + std::wstring(id.begin(), id.end()) + L".mesh";
        built[n - kMinSize] = !mesh.empty() && write_file(file, mesh.data(), mesh.size());
    }
    if (!built[0] || !built[3]) log(1, "impeller mesh not recognised or not writable (%zu bytes); some turbos use the stock impeller look", stock.size());
    dir += L"\\Turbo";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\turbochargers.json";
    std::string json = definitions_json(built);
    bool ok = write_file(file, json.data(), json.size());
    int n = WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, nullptr, 0, nullptr, nullptr);
    g_json_path.assign(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, g_json_path.data(), n, nullptr, nullptr);
    return ok && !g_json_path.empty();
}

bool already_added(void* container) {
    if (!g_get_definition || !g_string_ctor) return false;
    static const std::string id = definition_id(kMinSize);
    anymaker::gc_string_view s{};
    g_string_ctor(&s, id.c_str());
    const void* found = nullptr;
    g_get_definition(&found, container, &s);
    return found != nullptr;
}

// Loads our JSON into a fresh vehicle_component_definition_file and hands it to the container. The path's
// characters must outlive the call ($string_ctor_cstr makes a non-owning view), so they are static. Another mod's
// hook may sit under ours on this cell (GpsPart hooks it too); calling down through it is fine because every
// mod checks whether its parts are already in the container before adding them.
bool add_ours(void* container, const std::string& text) {
    static std::string kept[4];
    static int next;
    std::string& chars = kept[next++ % 4];
    chars = text;
    GamePath path{kStoreSystem, 0, {}};
    g_string_ctor(&path.path, chars.c_str());
    alignas(16) uint8_t file[64]{};
    g_file_ctor(file);
    bool ok = false;
    g_file_load(&ok, file, &path);
    if (ok) reinterpret_cast<add_definitions_t>(h_add_definitions.original)(container, file);
    g_file_dtor(file);
    return ok;
}

thread_local bool t_adding = false;
void hk_add_definitions(void* container, const void* file) {
    reinterpret_cast<add_definitions_t>(h_add_definitions.original)(container, file);
    if (!t_adding) { std::lock_guard<std::recursive_mutex> lock(g_mutex); g_definition_size.clear(); }   // definitions may move
    if (!g_ready.load() || t_adding || !container || already_added(container)) return;
    t_adding = true;
    std::string forward = g_json_path;
    for (char& c : forward) if (c == '\\') c = '/';
    bool ok = add_ours(container, g_json_path) || add_ours(container, forward);
    t_adding = false;
    static std::atomic<int> logs{6};
    if (logs.fetch_sub(1) > 0) log(ok ? 0 : 2, ok ? "Turbo parts added to the game's parts (%s)" : "could not load the Turbo definitions from %s", g_json_path.c_str());
}

// ------------------------------------------------------------------------------------- install (background thread)
// The destroy_node slot is read from the stock impeller's destroy, which calls it through the container's method
// table: relocation 1 of that function, right after its code (code_end 0xe8). It must be 8 * slot.
bool destroy_slot_matches(void* destroy_entry) {
    if (!destroy_entry) return false;
    uint64_t value = anymaker::read_slot(static_cast<const uint8_t*>(destroy_entry), 0xe8 + 8);
    if (value != uint64_t(8 * off::container_destroy_node_slot)) {
        log(2, "impeller destroy's container slot reads %llu, expected %d; Turbo stays off", static_cast<unsigned long long>(value), 8 * off::container_destroy_node_slot);
        return false;
    }
    return true;
}

void install_hooks() {
    using namespace anymaker::experimental;
    for (int attempt = 0; attempt < 180 && !g_stop; ++attempt) {
        if (attempt) Sleep(1000);
        if (!matching_build()) { if (attempt == 0) log(1, "game build does not match the SDK reference; Turbo stays off"); return; }
        void** add = hook_cell(bind::definitions_add_definitions);
        void** create = hook_cell(bind::centrifugal_create);
        void** destroy = hook_cell(bind::centrifugal_destroy);
        void** tick = hook_cell(bind::centrifugal_tick);
        void** get = hook_cell(bind::centrifugal_get_data_f64);
        auto fctor = reinterpret_cast<self_fn_t>(function(bind::definition_file_ctor));
        auto fdtor = reinterpret_cast<self_fn_t>(function(bind::definition_file_dtor));
        auto fload = reinterpret_cast<file_load_t>(function(bind::definition_file_load));
        auto cctor = reinterpret_cast<self_fn_t>(function(bind::gas_content_ctor));
        auto cdtor = reinterpret_cast<self_fn_t>(function(bind::gas_content_dtor));
        auto nctor = reinterpret_cast<self_fn_t>(function(bind::gas_node_ctor));
        auto ndtor = reinterpret_cast<self_fn_t>(function(bind::gas_node_dtor));
        auto ccreate = reinterpret_cast<content_create_t>(function(bind::gas_content_create));
        auto cmodify = reinterpret_cast<content_modify_t>(function(bind::gas_content_modify));
        auto cmove = reinterpret_cast<content_move_t>(function(bind::gas_content_move));
        auto pset = reinterpret_cast<property_set_t>(function(bind::property_u8_set));
        auto sc = reinterpret_cast<string_ctor_t>(native(anymaker::sym::string_ctor_cstr_rva));
        auto dt = static_cast<const double*>(global(bind::server_delta_time));
        bool core = add && create && destroy && tick && get && fctor && fdtor && fload && cctor && cdtor && nctor && ndtor && ccreate && cmodify && cmove && pset && sc;
        if (!core) {
            if (attempt == 179)
                log(2, "could not locate the game functions (add=%d create=%d destroy=%d tick=%d get=%d file=%d/%d/%d content=%d/%d/%d/%d/%d node=%d/%d property=%d string=%d)",
                    !!add, !!create, !!destroy, !!tick, !!get, !!fctor, !!fdtor, !!fload, !!cctor, !!cdtor, !!ccreate, !!cmodify, !!cmove, !!nctor, !!ndtor, !!pset, !!sc);
            continue;
        }
        if (!destroy_slot_matches(*destroy)) return;
        g_file_ctor = fctor; g_file_dtor = fdtor; g_file_load = fload;
        g_content_ctor = cctor; g_content_dtor = cdtor; g_node_ctor = nctor; g_node_dtor = ndtor;
        g_content_create = ccreate; g_content_modify = cmodify; g_content_move = cmove; g_property_set = pset; g_string_ctor = sc;
        g_delta_time = dt && anymaker::readable(dt, 8) ? dt : nullptr;
        g_create_node_bound = reinterpret_cast<create_node_t>(function(bind::gas_container_create_node));
        g_get_definition = reinterpret_cast<get_definition_t>(function(bind::definitions_get_by_id));
        if (!g_get_definition) log(1, "definition lookup not found; Turbo may add its parts twice if another mod re-adds definitions");
        if (!write_definition_file()) { log(2, "could not write the Turbo definitions file; Turbo is off"); return; }
        bool ok = h_destroy.install(destroy, (void*)&hk_destroy) && h_create.install(create, (void*)&hk_create) &&
                  h_tick.install(tick, (void*)&hk_tick) && h_get_f64.install(get, (void*)&hk_get_f64) &&
                  h_add_definitions.install(add, (void*)&hk_add_definitions);
        if (!ok) { log(2, "a hook cell was already taken or not writable; Turbo is off"); return; }
        g_ready = true;
        log(0, "Ready after %d s (server tick %s). Load or start a world to add the Turbo parts (definitions %s).", attempt,
            g_delta_time ? "found" : "not found, assuming 60 Hz", g_json_path.c_str());
        return;
    }
}
void remove_hooks() {
    h_add_definitions.remove((void*)&hk_add_definitions);
    h_get_f64.remove((void*)&hk_get_f64);
    h_tick.remove((void*)&hk_tick);
    h_create.remove((void*)&hk_create);
    h_destroy.remove((void*)&hk_destroy);
}
}  // namespace
}  // namespace turbo

using namespace turbo;
extern "C" __declspec(dllexport) bool AnyAPI_ModInit(const AnyModHostV1* h, AnyModCallbacksV1* out) {
    if (!h || !out || h->struct_size != sizeof(*h) || h->abi != ANYAPI_MOD_ABI || out->struct_size != sizeof(*out)) return false;
    host = *h; out->id = kModId;
    out->shutdown = [](void*) { g_stop = true; remove_hooks(); };
    return true;
}
extern "C" __declspec(dllexport) void AnyAPI_ModReady() {
    std::thread(install_hooks).detach();   // resolving scans game memory; keep it off the loader and frame threads
}
