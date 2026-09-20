// language: C++17, file: dllmain.cpp, target: Windows 11, MSVC
// Unity IL2CPP external ESP DLL — overlay via GDI+ colorkey window, no ImGui
//
// Entity sources (from dump):
//   Players  → GameManager.players       : Dictionary<int, PlayerManager>
//   Mobs     → MobManager.Instance.mobs  : Dictionary<int, Mob>
//              Mob.mobType (MobType SO)   → .name, .behaviour (Enemy/Neutral/Dragon/EnemyMeleeAndRanged)
//              Mob.hitable (Hitable)      → .hp, .maxHp
//
// Menu:  INSERT open/close  |  NUM8/2 navigate  |  NUM5 toggle  |  END unload

#include "pch.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#pragma comment(lib, "dwmapi.lib")
#include <dwmapi.h>

// ─────────────────────────────────────────────────────────────────────────────
// IL2CPP runtime API
// ─────────────────────────────────────────────────────────────────────────────
using il2cpp_domain_get_t                = void*(*)();
using il2cpp_thread_attach_t             = void*(*)(void*);
using il2cpp_domain_assembly_open_t      = void*(*)(void*, const char*);
using il2cpp_assembly_get_image_t        = void*(*)(void*);
using il2cpp_class_from_name_t           = void*(*)(void*, const char*, const char*);
using il2cpp_class_get_field_from_name_t = void*(*)(void*, const char*);
using il2cpp_field_static_get_value_t    = void (*)(void*, void*);
using il2cpp_field_get_value_t           = void (*)(void*, void*, void*);
using il2cpp_field_get_offset_t          = size_t(*)(void*);
using il2cpp_string_chars_t              = wchar_t*(*)(void*);
using il2cpp_object_get_class_t          = void*(*)(void*);

struct IL2CPP
{
    il2cpp_domain_get_t                domain_get{};
    il2cpp_thread_attach_t             thread_attach{};
    il2cpp_domain_assembly_open_t      domain_assembly_open{};
    il2cpp_assembly_get_image_t        assembly_get_image{};
    il2cpp_class_from_name_t           class_from_name{};
    il2cpp_class_get_field_from_name_t class_get_field_from_name{};
    il2cpp_field_static_get_value_t    field_static_get_value{};
    il2cpp_field_get_value_t           field_get_value{};
    il2cpp_field_get_offset_t          field_get_offset{};
    il2cpp_string_chars_t              string_chars{};
    il2cpp_object_get_class_t          object_get_class{};

    bool load(HMODULE mod)
    {
#define LOAD(n) n = reinterpret_cast<decltype(n)>(GetProcAddress(mod,"il2cpp_" #n)); if(!n) return false;
        LOAD(domain_get) LOAD(thread_attach) LOAD(domain_assembly_open)
        LOAD(assembly_get_image) LOAD(class_from_name)
        LOAD(class_get_field_from_name) LOAD(field_static_get_value)
        LOAD(field_get_value) LOAD(field_get_offset)
        LOAD(string_chars) LOAD(object_get_class)
#undef LOAD
        return true;
    }
} il2cpp;

// ─────────────────────────────────────────────────────────────────────────────
// Safe memory helpers
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
static T mem_read(uintptr_t addr)
{
    if (!addr) return T{};
    __try { return *reinterpret_cast<T*>(addr); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return T{}; }
}

static std::wstring read_il2cpp_string(uintptr_t str_obj)
{
    if (!str_obj) return L"";
    wchar_t* chars = il2cpp.string_chars(reinterpret_cast<void*>(str_obj));
    if (!chars) return L"";
    int len = mem_read<int>(str_obj + 0x10); // System.String._stringLength
    if (len <= 0 || len > 256) return L"";
    return std::wstring(chars, static_cast<size_t>(len));
}

// ─────────────────────────────────────────────────────────────────────────────
// IL2CPP Dictionary<int, TValue> traversal
// Unity 2020.3 IL2CPP x64 layout:
//   +0x10  object header end
//   +0x18  _entries  : Il2CppObject* (array of Entry)
//   +0x20  _count    : int32
//   Entry stride 0x18: hashCode(4)+next(4)+key(4)+pad(4)+value*(8)
//
// NOTE: Unity 2021+ may use stride 0x20 (extra padding).
//       Adjust ENTRY_STRIDE below if mobs come back empty on a newer build.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t DICT_ENTRIES_OFF  = 0x18;
static constexpr size_t DICT_COUNT_OFF    = 0x20;
static constexpr size_t DICT_DATA_START   = 0x20; // array object data starts at +0x20
static constexpr size_t ENTRY_STRIDE      = 0x18;
static constexpr size_t ENTRY_KEY_OFF     = 0x08;
static constexpr size_t ENTRY_VAL_OFF     = 0x10;

static std::vector<std::pair<int,uintptr_t>> dict_entries(uintptr_t dict_obj)
{
    std::vector<std::pair<int,uintptr_t>> out;
    if (!dict_obj) return out;
    uintptr_t arr    = mem_read<uintptr_t>(dict_obj + DICT_ENTRIES_OFF);
    int       count  = mem_read<int>(dict_obj + DICT_COUNT_OFF);
    if (!arr || count <= 0 || count > 512) return out;
    uintptr_t data = arr + DICT_DATA_START;
    out.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        uintptr_t ep   = data + i * ENTRY_STRIDE;
        int       hash = mem_read<int>(ep);
        int       key  = mem_read<int>(ep + ENTRY_KEY_OFF);
        uintptr_t val  = mem_read<uintptr_t>(ep + ENTRY_VAL_OFF);
        if (hash >= 0 && val)
            out.emplace_back(key, val);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────
struct Vec3 { float x, y, z; };

// Mob behaviour enum mirrors MobType.MobBehaviour from dump
enum class MobBehaviour : int
{
    Enemy              = 0,
    Neutral            = 1,
    EnemyMeleeAndRanged= 2,
    Dragon             = 3,
    Unknown            = 99
};

struct EntityData
{
    enum class Kind { Player, Mob } kind;

    // shared
    int          id;
    Vec3         world_pos;
    float        hp, max_hp;    // Hitable.hp / Hitable.maxHp  (mobs)
    float        hp_ratio;      // PlayerManager.hpRatio (players)
    bool         dead;
    bool         is_local;
    float        distance;

    // screen
    bool         on_screen;
    float        sx, sy;

    // display
    std::wstring label;         // username (player) or mobType.name (mob)
    MobBehaviour behaviour;     // mobs only
    COLORREF     color;         // pre-computed draw color
};

// ─────────────────────────────────────────────────────────────────────────────
// ESP settings
// ─────────────────────────────────────────────────────────────────────────────
struct Settings
{
    std::atomic<bool> master          {true};
    // players
    std::atomic<bool> players         {true};
    std::atomic<bool> player_box      {true};
    std::atomic<bool> player_name     {true};
    std::atomic<bool> player_hp       {true};
    std::atomic<bool> player_dist     {true};
    std::atomic<bool> player_dead     {false};
    // mobs — per-behaviour
    std::atomic<bool> mob_enemy       {true};
    std::atomic<bool> mob_neutral     {false};
    std::atomic<bool> mob_dragon      {true};
    std::atomic<bool> mob_melee_ranged{true};
    // shared
    std::atomic<bool> show_tracers    {false};
    std::atomic<float> max_dist       {600.f};
} g_cfg;

// ─────────────────────────────────────────────────────────────────────────────
// Menu
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int MENU_COUNT = 14;
static const wchar_t* MENU_LABELS[MENU_COUNT] = {
    L"[MASTER] ESP",
    L"--- PLAYERS ---",
    L"  Players ESP",
    L"  Boxes",
    L"  Names",
    L"  Health Bars",
    L"  Distance",
    L"  Show Dead",
    L"--- MOBS ---",
    L"  Enemies",
    L"  Neutral",
    L"  Dragon",
    L"  Melee+Ranged",
    L"  Tracers",
};

// NULL means section header (not toggleable)
static std::atomic<bool>* MENU_BINDS[MENU_COUNT] = {
    &g_cfg.master,
    nullptr,
    &g_cfg.players,
    &g_cfg.player_box,
    &g_cfg.player_name,
    &g_cfg.player_hp,
    &g_cfg.player_dist,
    &g_cfg.player_dead,
    nullptr,
    &g_cfg.mob_enemy,
    &g_cfg.mob_neutral,
    &g_cfg.mob_dragon,
    &g_cfg.mob_melee_ranged,
    &g_cfg.show_tracers,
};

static std::atomic<bool> g_menu_open{false};
static int                g_menu_sel{0};

// ─────────────────────────────────────────────────────────────────────────────
// Shared frame state
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex              g_mtx;
static std::vector<EntityData> g_frame;
static int                     g_sw{1920}, g_sh{1080};

// ─────────────────────────────────────────────────────────────────────────────
// IL2CPP class/field cache
// ─────────────────────────────────────────────────────────────────────────────
// PlayerManager
static size_t g_off_pm_username{};
static size_t g_off_pm_dead{};
static size_t g_off_pm_hpRatio{};

// Mob
static size_t g_off_mob_hitable{};    // Hitable component ptr (field)
static size_t g_off_mob_mobType{};    // MobType ScriptableObject ptr (field)

// Hitable
static size_t g_off_hit_hp{};
static size_t g_off_hit_maxHp{};

// MobType
static size_t g_off_mt_name{};        // inherited from Object (UnityEngine.Object.name is a property; fallback to m_Name field)
static size_t g_off_mt_behaviour{};   // MobType.behaviour : MobBehaviour (int enum)

// Statics
static void* g_fld_gm_players{};      // static GameManager.players
static void* g_fld_mm_instance{};     // static MobManager.Instance
static void* g_fld_lc_instance{};     // static LocalClient.instance
static size_t g_off_mm_mobs{};        // MobManager.mobs field offset
static size_t g_off_lc_myId{};        // LocalClient.myId field offset

static bool g_resolved{false};

// ─────────────────────────────────────────────────────────────────────────────
// Transform position
// Unity 2020.3 IL2CPP x64:
//   MonoBehaviour_o.m_CachedPtr (IntPtr, at +0x10) → GfxTransformType*
//   GfxTransformType.local_position.x at GfxTransformType+0x90
//   DERIVE PER BUILD: attach x64dbg, BP Transform::get_position, inspect struct.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uintptr_t CACHED_PTR_OFF  = 0x10; // MonoBehaviour->m_CachedPtr
static constexpr uintptr_t GFXTRANSFORM_POS= 0x90; // GfxTransform->local_position

static Vec3 read_position(uintptr_t mono_obj)
{
    if (!mono_obj) return {};
    uintptr_t cached = mem_read<uintptr_t>(mono_obj + CACHED_PTR_OFF);
    if (!cached) return {};
    return {
        mem_read<float>(cached + GFXTRANSFORM_POS + 0x0),
        mem_read<float>(cached + GFXTRANSFORM_POS + 0x4),
        mem_read<float>(cached + GFXTRANSFORM_POS + 0x8)
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera WorldToScreenPoint via vtable
// Vtable index for WorldToScreenPoint: 0x4C on Unity 2020.3 — DERIVE PER BUILD
// ─────────────────────────────────────────────────────────────────────────────
static constexpr size_t W2S_VTABLE_IDX   = 0x4C;
static constexpr uintptr_t KLASS_VTABLE_OFF = 0x28;

static Vec3 world_to_screen(uintptr_t cam, Vec3 w)
{
    if (!cam) return {-1,-1,-1};
    uintptr_t klass  = mem_read<uintptr_t>(cam);
    uintptr_t vtable = mem_read<uintptr_t>(klass + KLASS_VTABLE_OFF);
    typedef Vec3(__fastcall* fn_t)(uintptr_t, Vec3);
    auto fn = mem_read<fn_t>(vtable + W2S_VTABLE_IDX * 8);
    if (!fn) return {-1,-1,-1};
    __try { return fn(cam, w); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return {-1,-1,-1}; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera.main — static field m_cachedCamera on Camera class
// ─────────────────────────────────────────────────────────────────────────────
static void* g_cls_Camera{};

static uintptr_t get_main_cam()
{
    if (!g_cls_Camera) return 0;
    void* fld = il2cpp.class_get_field_from_name(g_cls_Camera, "m_cachedCamera");
    if (!fld) return 0;
    uintptr_t cam{};
    il2cpp.field_static_get_value(fld, &cam);
    return cam;
}

// Camera world position (same cached ptr chain)
static Vec3 cam_world_pos(uintptr_t cam)
{
    if (!cam) return {};
    uintptr_t cached = mem_read<uintptr_t>(cam + CACHED_PTR_OFF);
    if (!cached) return {};
    return {
        mem_read<float>(cached + GFXTRANSFORM_POS + 0x0),
        mem_read<float>(cached + GFXTRANSFORM_POS + 0x4),
        mem_read<float>(cached + GFXTRANSFORM_POS + 0x8)
    };
}

static float dist3(Vec3 a, Vec3 b)
{
    float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
    return sqrtf(dx*dx+dy*dy+dz*dz);
}

// ─────────────────────────────────────────────────────────────────────────────
// Resolve classes + field offsets
// ─────────────────────────────────────────────────────────────────────────────
static bool resolve_all()
{
    void* domain = il2cpp.domain_get();
    if (!domain) return false;
    il2cpp.thread_attach(domain);

    auto open = [&](const char* name) -> void* {
        void* a = il2cpp.domain_assembly_open(domain, name);
        return a ? il2cpp.assembly_get_image(a) : nullptr;
    };

    void* img_cs    = open("Assembly-CSharp");
    void* img_core  = open("UnityEngine.CoreModule");
    if (!img_cs || !img_core) return false;

    auto cls = [&](void* img, const char* ns, const char* name) -> void* {
        return il2cpp.class_from_name(img, ns, name);
    };
    auto fld_off = [&](void* klass, const char* name) -> size_t {
        void* f = il2cpp.class_get_field_from_name(klass, name);
        return f ? il2cpp.field_get_offset(f) : 0;
    };
    auto fld_ptr = [&](void* klass, const char* name) -> void* {
        return il2cpp.class_get_field_from_name(klass, name);
    };

    // ── PlayerManager ──────────────────────────────────────────────────────
    void* cls_pm = cls(img_cs, "", "PlayerManager");
    if (!cls_pm) return false;
    g_off_pm_username = fld_off(cls_pm, "username");
    g_off_pm_dead     = fld_off(cls_pm, "dead");
    g_off_pm_hpRatio  = fld_off(cls_pm, "hpRatio");
    if (!g_off_pm_hpRatio) g_off_pm_hpRatio = fld_off(cls_pm, "currentHpRatio");

    // ── GameManager.players (static) ───────────────────────────────────────
    void* cls_gm = cls(img_cs, "", "GameManager");
    if (!cls_gm) return false;
    g_fld_gm_players = fld_ptr(cls_gm, "players");
    if (!g_fld_gm_players) return false;

    // ── LocalClient ────────────────────────────────────────────────────────
    void* cls_lc = cls(img_cs, "", "LocalClient");
    if (cls_lc)
    {
        g_fld_lc_instance = fld_ptr(cls_lc, "instance");
        g_off_lc_myId     = fld_off(cls_lc, "myId");
    }

    // ── MobManager ────────────────────────────────────────────────────────
    void* cls_mm = cls(img_cs, "", "MobManager");
    if (cls_mm)
    {
        g_fld_mm_instance = fld_ptr(cls_mm, "Instance");
        g_off_mm_mobs     = fld_off(cls_mm, "mobs");
    }

    // ── Mob ────────────────────────────────────────────────────────────────
    void* cls_mob = cls(img_cs, "", "Mob");
    if (cls_mob)
    {
        g_off_mob_hitable = fld_off(cls_mob, "hitable");
        g_off_mob_mobType = fld_off(cls_mob, "mobType");
    }

    // ── Hitable ───────────────────────────────────────────────────────────
    void* cls_hit = cls(img_cs, "", "Hitable");
    if (cls_hit)
    {
        g_off_hit_hp    = fld_off(cls_hit, "hp");
        g_off_hit_maxHp = fld_off(cls_hit, "maxHp");
    }

    // ── MobType (ScriptableObject) ─────────────────────────────────────────
    void* cls_mt = cls(img_cs, "", "MobType");
    if (cls_mt)
    {
        // behaviour enum field
        g_off_mt_behaviour = fld_off(cls_mt, "behaviour");
        // name: UnityEngine.Object.m_Name (backing field of .name property)
        // MobType inherits ScriptableObject → Object; field is on the base class
        // Resolve via UnityEngine.Object class
        void* cls_uobj = cls(img_core, "UnityEngine", "Object");
        if (cls_uobj)
            g_off_mt_name = fld_off(cls_uobj, "m_CachedPtr"); // not name — see below
        // m_Name is at a known offset in UnityEngine.Object IL2CPP objects: +0x10
        // (string field of UnityEngine.Object, same as m_Name backing field)
        // We'll read it directly as a string obj at obj+0x10 for ScriptableObjects
        g_off_mt_name = 0x10; // direct read — standard for UE.Object subclasses
    }

    // ── Camera ────────────────────────────────────────────────────────────
    g_cls_Camera = cls(img_core, "UnityEngine", "Camera");

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read local player ID
// ─────────────────────────────────────────────────────────────────────────────
static int local_id()
{
    if (!g_fld_lc_instance || !g_off_lc_myId) return -1;
    uintptr_t inst{};
    il2cpp.field_static_get_value(g_fld_lc_instance, &inst);
    if (!inst) return -1;
    return mem_read<int>(inst + g_off_lc_myId);
}

// ─────────────────────────────────────────────────────────────────────────────
// Behaviour → colour
// ─────────────────────────────────────────────────────────────────────────────
static COLORREF mob_color(MobBehaviour b)
{
    switch(b)
    {
        case MobBehaviour::Enemy:               return RGB(255,80,80);    // red
        case MobBehaviour::EnemyMeleeAndRanged: return RGB(255,140,0);    // orange
        case MobBehaviour::Dragon:              return RGB(180,0,255);    // purple
        case MobBehaviour::Neutral:             return RGB(255,220,50);   // yellow
        default:                                return RGB(200,200,200);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Collect frame data
// ─────────────────────────────────────────────────────────────────────────────
static void collect()
{
    if (!g_resolved) return;

    uintptr_t cam = get_main_cam();
    Vec3 cam_pos  = cam_world_pos(cam);
    int  lid      = local_id();

    std::vector<EntityData> frame;
    frame.reserve(64);

    // ── Players ─────────────────────────────────────────────────────────────
    if (g_fld_gm_players)
    {
        uintptr_t dict{};
        il2cpp.field_static_get_value(g_fld_gm_players, &dict);
        for (auto& [id, pm] : dict_entries(dict))
        {
            EntityData e{};
            e.kind     = EntityData::Kind::Player;
            e.id       = id;
            e.is_local = (id == lid);
            e.dead     = g_off_pm_dead     ? mem_read<bool>(pm  + g_off_pm_dead)     : false;
            e.hp_ratio = g_off_pm_hpRatio  ? mem_read<float>(pm + g_off_pm_hpRatio)  : 1.f;
            e.hp = e.hp_ratio; e.max_hp = 1.f;

            if (g_off_pm_username)
            {
                uintptr_t sptr = mem_read<uintptr_t>(pm + g_off_pm_username);
                e.label = read_il2cpp_string(sptr);
            }
            if (e.label.empty()) e.label = L"Player";

            e.world_pos = read_position(pm);
            e.distance  = dist3(e.world_pos, cam_pos);
            e.color     = e.is_local ? RGB(0,255,0) :
                          (e.dead    ? RGB(120,120,120) : RGB(80,200,255));

            Vec3 sc = world_to_screen(cam, e.world_pos);
            e.on_screen = sc.z > 0.f && sc.x > 0 && sc.x < g_sw && sc.y > 0 && sc.y < g_sh;
            e.sx = sc.x; e.sy = (float)g_sh - sc.y;

            frame.push_back(std::move(e));
        }
    }

    // ── Mobs ────────────────────────────────────────────────────────────────
    if (g_fld_mm_instance && g_off_mm_mobs)
    {
        uintptr_t mm_inst{};
        il2cpp.field_static_get_value(g_fld_mm_instance, &mm_inst);
        if (mm_inst)
        {
            uintptr_t dict = mem_read<uintptr_t>(mm_inst + g_off_mm_mobs);
            for (auto& [id, mob] : dict_entries(dict))
            {
                if (!mob) continue;
                EntityData e{};
                e.kind      = EntityData::Kind::Mob;
                e.id        = id;
                e.is_local  = false;
                e.behaviour = MobBehaviour::Unknown;

                // mobType ScriptableObject
                if (g_off_mob_mobType)
                {
                    uintptr_t mt = mem_read<uintptr_t>(mob + g_off_mob_mobType);
                    if (mt)
                    {
                        // name: UnityEngine.Object.m_Name string at mt+0x10
                        uintptr_t name_str = mem_read<uintptr_t>(mt + g_off_mt_name);
                        e.label = read_il2cpp_string(name_str);
                        if (e.label.empty()) e.label = L"Mob";

                        if (g_off_mt_behaviour)
                            e.behaviour = static_cast<MobBehaviour>(mem_read<int>(mt + g_off_mt_behaviour));
                    }
                }
                if (e.label.empty()) e.label = L"Mob";

                // Hitable → hp, maxHp
                if (g_off_mob_hitable)
                {
                    uintptr_t hit = mem_read<uintptr_t>(mob + g_off_mob_hitable);
                    if (hit)
                    {
                        e.hp     = g_off_hit_hp    ? mem_read<float>(hit + g_off_hit_hp)    : 0.f;
                        e.max_hp = g_off_hit_maxHp ? mem_read<float>(hit + g_off_hit_maxHp) : 1.f;
                    }
                }
                e.hp_ratio = (e.max_hp > 0.f) ? (e.hp / e.max_hp) : 1.f;
                e.dead     = (e.hp <= 0.f);
                e.color    = mob_color(e.behaviour);

                e.world_pos = read_position(mob);
                e.distance  = dist3(e.world_pos, cam_pos);

                Vec3 sc = world_to_screen(cam, e.world_pos);
                e.on_screen = sc.z > 0.f && sc.x > 0 && sc.x < g_sw && sc.y > 0 && sc.y < g_sh;
                e.sx = sc.x; e.sy = (float)g_sh - sc.y;

                frame.push_back(std::move(e));
            }
        }
    }

    std::lock_guard<std::mutex> lk(g_mtx);
    g_frame = std::move(frame);
}

// ─────────────────────────────────────────────────────────────────────────────
// GDI drawing helpers
// ─────────────────────────────────────────────────────────────────────────────
static void gdi_text(HDC hdc, const std::wstring& s, int x, int y,
                     COLORREF col, int sz=13, bool shadow=true)
{
    HFONT f = CreateFontW(sz,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Arial");
    HFONT of = static_cast<HFONT>(SelectObject(hdc,f));
    SetBkMode(hdc,TRANSPARENT);
    if (shadow) { SetTextColor(hdc,RGB(0,0,0)); TextOutW(hdc,x+1,y+1,s.c_str(),(int)s.size()); }
    SetTextColor(hdc,col); TextOutW(hdc,x,y,s.c_str(),(int)s.size());
    SelectObject(hdc,of); DeleteObject(f);
}

static void gdi_rect(HDC hdc, int x,int y,int w,int h, COLORREF col, int thick=1)
{
    HPEN p   = CreatePen(PS_SOLID,thick,col);
    HPEN op  = static_cast<HPEN>(SelectObject(hdc,p));
    HBRUSH b = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HBRUSH ob= static_cast<HBRUSH>(SelectObject(hdc,b));
    Rectangle(hdc,x,y,x+w,y+h);
    SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(p);
}

static void gdi_hpbar(HDC hdc, int x,int y,int h, float ratio)
{
    // background
    HBRUSH bg = CreateSolidBrush(RGB(50,10,10));
    RECT rc={x-5,y,x-2,y+h}; FillRect(hdc,&rc,bg); DeleteObject(bg);
    // fill
    int fill=std::max(1,(int)(h*ratio)), fy=y+h-fill;
    COLORREF fc = ratio>0.5f ? RGB(30,210,30) : (ratio>0.25f ? RGB(210,210,0) : RGB(210,40,40));
    HBRUSH fg = CreateSolidBrush(fc);
    RECT rf={x-5,fy,x-2,y+h}; FillRect(hdc,&rf,fg); DeleteObject(fg);
}

static void gdi_tracer(HDC hdc, float sx, float sy, COLORREF col)
{
    HPEN p  = CreatePen(PS_SOLID,1,col);
    HPEN op = static_cast<HPEN>(SelectObject(hdc,p));
    MoveToEx(hdc,g_sw/2,g_sh,nullptr);
    LineTo(hdc,(int)sx,(int)sy);
    SelectObject(hdc,op); DeleteObject(p);
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-entity render
// ─────────────────────────────────────────────────────────────────────────────
static bool should_draw(const EntityData& e)
{
    if (!g_cfg.master.load()) return false;
    if (e.distance > g_cfg.max_dist.load()) return false;

    if (e.kind == EntityData::Kind::Player)
    {
        if (!g_cfg.players.load()) return false;
        if (e.dead && !g_cfg.player_dead.load()) return false;
        return true;
    }
    // Mob
    if (e.dead) return false; // always skip dead mobs
    switch(e.behaviour)
    {
        case MobBehaviour::Enemy:               return g_cfg.mob_enemy.load();
        case MobBehaviour::EnemyMeleeAndRanged: return g_cfg.mob_melee_ranged.load();
        case MobBehaviour::Dragon:              return g_cfg.mob_dragon.load();
        case MobBehaviour::Neutral:             return g_cfg.mob_neutral.load();
        default: return g_cfg.mob_enemy.load();
    }
}

static void draw_entity(HDC hdc, const EntityData& e)
{
    if (!e.on_screen) return;
    if (!should_draw(e)) return;

    float scale = 1800.f / std::max(1.f, e.distance);
    int bh = std::max(18, (int)(scale * 1.8f));
    int bw = std::max(10, (int)(bh * 0.45f));
    int bx = (int)e.sx - bw/2;
    int by = (int)e.sy - bh;

    COLORREF col = e.color;

    bool is_player = (e.kind == EntityData::Kind::Player);

    // box
    bool draw_box = is_player ? g_cfg.player_box.load() : true;
    if (draw_box) gdi_rect(hdc, bx, by, bw, bh, col);

    // hp bar
    bool draw_hp = is_player ? g_cfg.player_hp.load() : true;
    if (draw_hp && !e.dead) gdi_hpbar(hdc, bx, by, bh, e.hp_ratio);

    int ty = by - 15;

    // name / label
    bool draw_name = is_player ? g_cfg.player_name.load() : true;
    if (draw_name)
    {
        std::wstring lbl = e.label;
        if (!is_player)
        {
            // mob: show hp numbers too
            wchar_t buf[64];
            swprintf_s(buf,L"%s [%.0f/%.0f]", e.label.c_str(), e.hp, e.max_hp);
            lbl = buf;
        }
        gdi_text(hdc, lbl, bx, ty, col, 13);
        ty -= 14;
    }

    // distance
    bool draw_dist = is_player ? g_cfg.player_dist.load() : true;
    if (draw_dist)
    {
        wchar_t buf[32]; swprintf_s(buf,L"%.0fm",e.distance);
        gdi_text(hdc, buf, bx, ty, RGB(190,190,190), 12);
    }

    // tracers
    if (g_cfg.show_tracers.load())
        gdi_tracer(hdc, e.sx, e.sy, col);
}

// ─────────────────────────────────────────────────────────────────────────────
// Menu rendering
// ─────────────────────────────────────────────────────────────────────────────
static void draw_menu(HDC hdc)
{
    int mx=30, my=30, mw=290, row=22;
    int mh = 28 + MENU_COUNT*row + 8;

    HBRUSH bg = CreateSolidBrush(RGB(10,12,22));
    RECT pr={mx,my,mx+mw,my+mh}; FillRect(hdc,&pr,bg); DeleteObject(bg);
    gdi_rect(hdc,mx,my,mw,mh,RGB(60,100,220),2);
    gdi_text(hdc,L"VANTA ESP",mx+8,my+6,RGB(140,190,255),15,false);

    for (int i=0;i<MENU_COUNT;++i)
    {
        int iy = my+28+i*row;
        bool is_hdr = (MENU_BINDS[i]==nullptr);

        if (is_hdr)
        {
            gdi_text(hdc, MENU_LABELS[i], mx+6, iy, RGB(120,120,180), 12, false);
            continue;
        }

        bool sel = (i==g_menu_sel);
        if (sel)
        {
            HBRUSH sb = CreateSolidBrush(RGB(25,35,75));
            RECT sr={mx+2,iy-1,mx+mw-2,iy+row-2}; FillRect(hdc,&sr,sb); DeleteObject(sb);
        }

        bool on = MENU_BINDS[i]->load();
        COLORREF lc = on ? RGB(80,230,100) : RGB(200,70,70);
        gdi_text(hdc, MENU_LABELS[i], mx+8, iy, lc, 13, false);
        gdi_text(hdc, on?L"ON":L"OFF", mx+mw-40, iy,
                 on?RGB(0,210,60):RGB(210,50,50), 13, false);
    }

    gdi_text(hdc, L"NUM8/2 nav  NUM5 toggle  INS close  END unload",
             mx+4, my+mh-14, RGB(80,80,120), 11, false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Overlay window
// ─────────────────────────────────────────────────────────────────────────────
static HWND g_overlay{}, g_target{};
static ULONG_PTR g_gdi_token{};

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg==WM_DESTROY){ PostQuitMessage(0); return 0; }
    if (msg==WM_PAINT)
    {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps);
        RECT rc; GetClientRect(hwnd,&rc);
        HDC mem=CreateCompatibleDC(hdc);
        HBITMAP bm=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
        HBITMAP ob=static_cast<HBITMAP>(SelectObject(mem,bm));
        // clear to black (colorkey)
        HBRUSH clr=CreateSolidBrush(RGB(0,0,0));
        FillRect(mem,&rc,clr); DeleteObject(clr);

        if (g_cfg.master.load())
        {
            std::vector<EntityData> snap;
            { std::lock_guard<std::mutex> lk(g_mtx); snap=g_frame; }
            // sort back-to-front by distance
            std::sort(snap.begin(),snap.end(),[](const EntityData&a,const EntityData&b){
                return a.distance>b.distance;
            });
            for (auto& e : snap) draw_entity(mem,e);
        }
        if (g_menu_open.load()) draw_menu(mem);

        BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem,ob); DeleteObject(bm); DeleteDC(mem);
        EndPaint(hwnd,&ps); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

// ─────────────────────────────────────────────────────────────────────────────
// Threads
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_run{true};

// advance selection, skipping headers
static void menu_move(int delta)
{
    int sel = g_menu_sel;
    for (int attempt=0; attempt<MENU_COUNT*2; ++attempt)
    {
        sel = (sel + delta + MENU_COUNT) % MENU_COUNT;
        if (MENU_BINDS[sel] != nullptr) { g_menu_sel=sel; return; }
    }
}

static void hotkey_thread_fn()
{
    while (g_run.load())
    {
        if (GetAsyncKeyState(VK_INSERT)&1)
            g_menu_open.store(!g_menu_open.load());

        if (g_menu_open.load())
        {
            if (GetAsyncKeyState(VK_NUMPAD8)&1) menu_move(-1);
            if (GetAsyncKeyState(VK_NUMPAD2)&1) menu_move(+1);
            if (GetAsyncKeyState(VK_NUMPAD5)&1)
            {
                auto* b = MENU_BINDS[g_menu_sel];
                if (b) b->store(!b->load());
            }
        }
        if (GetAsyncKeyState(VK_END)&1)
        {
            g_run.store(false);
            if (g_overlay) PostMessage(g_overlay,WM_CLOSE,0,0);
        }
        Sleep(10);
    }
}

static void collect_thread_fn()
{
    HMODULE ga = nullptr;
    for (int i=0; i<120 && g_run.load(); ++i)
    {
        ga = GetModuleHandleA("GameAssembly.dll");
        if (ga) break;
        Sleep(500);
    }
    if (!ga) { g_run.store(false); return; }
    if (!il2cpp.load(ga)) { g_run.store(false); return; }

    for (int i=0; i<60 && g_run.load(); ++i)
    {
        if (il2cpp.domain_get()) break;
        Sleep(1000);
    }
    Sleep(3000); // wait for game scene to fully load

    g_resolved = resolve_all();

    while (g_run.load())
    {
        if (g_target)
        {
            RECT r{}; GetClientRect(g_target,&r);
            if (r.right>0){ g_sw=r.right; g_sh=r.bottom; }
        }
        collect();
        if (g_overlay) InvalidateRect(g_overlay,nullptr,FALSE);
        Sleep(8);
    }
}

static void overlay_thread_fn()
{
    for (int i=0; i<120 && g_run.load(); ++i)
    {
        g_target = FindWindowA(nullptr,"Muck");
        if (!g_target) g_target = FindWindowA("Muck ",nullptr);
        if (!g_target) g_target = FindWindowA("UnityWndClass",nullptr);
        if (g_target) break;
        Sleep(500);
    }
    if (!g_target){ g_run.store(false); return; }

    RECT wr{}; GetWindowRect(g_target,&wr);
    int wx=wr.left,wy=wr.top,ww=wr.right-wr.left,wh=wr.bottom-wr.top;
    g_sw=ww; g_sh=wh;

    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc);
    wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=wnd_proc;
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"VantaESP_Overlay";
    RegisterClassExW(&wc);

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW,
        L"VantaESP_Overlay",L"",WS_POPUP,
        wx,wy,ww,wh,nullptr,nullptr,wc.hInstance,nullptr);
    if (!g_overlay){ g_run.store(false); return; }

    SetLayeredWindowAttributes(g_overlay,RGB(0,0,0),0,LWA_COLORKEY);
    MARGINS m{-1,-1,-1,-1}; DwmExtendFrameIntoClientArea(g_overlay,&m);
    ShowWindow(g_overlay,SW_SHOW); UpdateWindow(g_overlay);

    std::thread([]{
        while(g_run.load() && IsWindow(g_overlay))
        {
            RECT r{}; GetWindowRect(g_target,&r);
            SetWindowPos(g_overlay,HWND_TOPMOST,r.left,r.top,
                         r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE);
            Sleep(16);
        }
    }).detach();

    MSG msg{};
    while(g_run.load() && GetMessageW(&msg,nullptr,0,0))
    {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    DestroyWindow(g_overlay); g_overlay=nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// DLL entry
// ─────────────────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, LPVOID)
{
    if (reason==DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hmod);
        Gdiplus::GdiplusStartupInput gsi{};
        Gdiplus::GdiplusStartup(&g_gdi_token,&gsi,nullptr);
        std::thread(overlay_thread_fn).detach();
        std::thread(collect_thread_fn).detach();
        std::thread(hotkey_thread_fn).detach();
    }
    else if (reason==DLL_PROCESS_DETACH)
    {
        g_run.store(false);
        Gdiplus::GdiplusShutdown(g_gdi_token);
    }
    return TRUE;
}
