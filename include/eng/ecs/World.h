#pragma once

// eng::ecs::World — entities and their components. See docs/ecs-lua-plan.md.
//
//   ecs::Registry reg; reg.add<Pos>("Pos", {...}); reg.finalize();
//   ecs::World world(reg, {.maxEntities = 512});
//   auto hound = world.definePrefab("hound", /*max=*/8);
//   world.setPrefabComponent(hound, Health{3});
//   ecs::Entity e = world.spawn(hound);           // kNullEntity at a cap
//   world.each<Pos, Vel>([&](ecs::Entity e, Pos& p, Vel& v) { p.v += v.v; });
//
// Storage is a sparse set per component: a slot→dense-index array sized to
// the cap (2 bytes × cap) plus packed component bytes, so memory follows
// what's in use. Removing swaps the last element into the hole, so dense
// order follows add/remove history — deterministic given deterministic
// calls. Pointers/references to components stay valid only until that
// component type is next added or removed.
//
// Structural changes (spawn, destroy, add, remove) aren't allowed while an
// each()/query() is running — they would reorder what's being walked. Put
// them in commands() instead; flush() applies them in order (between systems).

#include "eng/ecs/Entity.h"
#include "eng/ecs/Registry.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eng::ecs {

using PrefabId = uint16_t;
constexpr PrefabId kNoPrefab = 0xFFFF;

struct WorldConfig {
    uint32_t maxEntities = 256;         // total cap, 1..65535; everything per-slot is sized to it up front
    uint16_t defaultPrefabMax = 8;      // a prefab's live cap unless definePrefab says otherwise
};

class World;

// Structural changes recorded during iteration, applied by World::flush().
// spawn() hands back the new entity right away (its slot is reserved, so
// caps are decided now, deterministically); its components arrive at flush.
class Commands {
public:
    Entity spawn(PrefabId prefab = kNoPrefab);
    void destroy(Entity e);
    void add(Entity e, ComponentId c, const void* bytes = nullptr);  // nullptr = the component's defaults
    template <class T> void add(Entity e, const T& value);
    void remove(Entity e, ComponentId c);
    bool empty() const { return m_ops.empty(); }

private:
    friend class World;
    explicit Commands(World& w) : m_world(w) {}
    enum class Op : uint8_t { Spawn, Destroy, Add, Remove };
    struct Record {
        Op op;
        Entity entity;
        ComponentId component;
        PrefabId prefab;
        uint32_t bytesAt;  // Add: offset into m_bytes (UINT32_MAX = defaults)
    };
    World& m_world;
    std::vector<Record> m_ops;
    std::vector<std::byte> m_bytes;
};

class World {
public:
    // `registry` must be finalized and outlive the world.
    explicit World(const Registry& registry, WorldConfig config = {});
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    const Registry& registry() const { return m_reg; }
    uint32_t capacity() const { return m_cap; }
    uint32_t size() const { return m_live; }

    // ── Prefabs ── (definitions; Phase 4 loads them from Lua)
    PrefabId definePrefab(std::string name, uint16_t max = 0);  // 0 = config.defaultPrefabMax
    PrefabId findPrefab(std::string_view name) const;           // kNoPrefab if missing
    void setPrefabComponent(PrefabId p, ComponentId c, const void* bytes = nullptr);
    template <class T> void setPrefabComponent(PrefabId p, const T& value) { setPrefabComponent(p, id<T>(), &value); }
    const std::string& prefabName(PrefabId p) const { return m_prefabs[p].name; }
    uint16_t prefabCount(PrefabId p) const { return m_prefabs[p].live; }
    size_t prefabDefinitions() const { return m_prefabs.size(); }  // valid PrefabIds are below this

    // ── Entities ──
    // kNullEntity when the world or the prefab is at its cap (logged).
    Entity spawn(PrefabId prefab = kNoPrefab);
    void destroy(Entity e);  // no-op for a stale handle
    // Spawns exactly `e` (its slot, with its generation) — for a client
    // mirroring the server's entities, so handles (and Entity fields) mean the
    // same thing on both. The slot must be free. Caps aren't checked: the
    // server already applied them. A mirrored world shouldn't also spawn()
    // entities of its own, or they'd take slots the server will use.
    Entity spawnAt(Entity e, PrefabId prefab = kNoPrefab);
    bool alive(Entity e) const;
    PrefabId prefabOf(Entity e) const;  // kNoPrefab if none / not alive
    // The live entity in `slot`, or kNullEntity — for net code, which sends slots.
    Entity entityAt(uint16_t slot) const;

    // ── Components, by id (generic: Lua, net, saves) ──
    // add() returns the component's bytes (initialized from `bytes`, else
    // its defaults); if it was already there it's overwritten.
    void* add(Entity e, ComponentId c, const void* bytes = nullptr);
    void remove(Entity e, ComponentId c);
    bool has(Entity e, ComponentId c) const;
    void* get(Entity e, ComponentId c);  // nullptr if absent or stale
    const void* get(Entity e, ComponentId c) const;
    size_t count(ComponentId c) const { return m_stores[c].slots.size(); }

    // ── Components, typed ──
    template <class T> ComponentId id() const;
    template <class T> T& add(Entity e, const T& value = T{}) { return *static_cast<T*>(add(e, id<T>(), &value)); }
    template <class T> void remove(Entity e) { remove(e, id<T>()); }
    template <class T> bool has(Entity e) const { return has(e, id<T>()); }
    template <class T> T* get(Entity e) { return static_cast<T*>(get(e, id<T>())); }
    template <class T> const T* get(Entity e) const { return static_cast<const T*>(get(e, id<T>())); }

    // Every entity with all of `Cs`: fn(Entity, Cs&...). Walks the smallest
    // of the sets and checks the others, in that set's dense order.
    template <class... Cs, class F> void each(F&& fn);
    // The same by ids (for Lua): fn(Entity).
    void query(std::span<const ComponentId> components, const std::function<void(Entity)>& fn);

    // Every T, packed, in dense order (what each<T>() walks), and the entity
    // owning the i-th: for code that wants plain loops or indices over one
    // component type (a game's container facade). Valid until T is next
    // added or removed; adding/removing T while looping is the caller's bug,
    // as with a vector.
    template <class T> std::span<T> raw() {
        Store& s = m_stores[id<T>()];
        return {reinterpret_cast<T*>(s.data.data()), s.slots.size()};
    }
    template <class T> std::span<const T> raw() const {
        const Store& s = m_stores[id<T>()];
        return {reinterpret_cast<const T*>(s.data.data()), s.slots.size()};
    }
    Entity denseEntity(ComponentId c, size_t i) const {
        uint16_t slot = m_stores[c].slots[i];
        return makeEntity(slot, m_generation[slot]);
    }

    // ── Deferred changes ──
    Commands& commands() { return m_commands; }
    void flush();  // applies commands() in order; not while iterating
    bool iterating() const { return m_iterating > 0; }

private:
    friend class Commands;
    struct Store {
        uint32_t size = 0;
        std::vector<uint16_t> sparse;  // slot → dense index (kAbsent if none); sized to the cap
        std::vector<uint16_t> slots;   // dense index → slot
        std::vector<std::byte> data;   // dense index × size
        std::byte* at(uint16_t dense) { return data.data() + size_t(dense) * size; }
    };
    struct Prefab {
        std::string name;
        uint16_t max = 0;
        uint16_t live = 0;
        std::vector<std::pair<ComponentId, std::vector<std::byte>>> components;
    };
    static constexpr uint16_t kAbsent = 0xFFFF;

    bool slotAlive(uint16_t slot) const { return m_alive[slot] != 0; }
    Entity reserve(PrefabId prefab);  // slot + generation + caps; no components yet
    void applyPrefab(Entity e, PrefabId prefab);
    void* addToSlot(uint16_t slot, ComponentId c, const void* bytes);
    void removeFromSlot(uint16_t slot, ComponentId c);
    void checkStructural(const char* what) const;

    const Registry& m_reg;
    WorldConfig m_config;
    uint32_t m_cap = 0;
    uint32_t m_live = 0;
    std::vector<uint16_t> m_generation;  // per slot; starts at 1, never 0
    std::vector<uint8_t> m_alive;
    std::vector<PrefabId> m_prefabOf;
    std::vector<uint16_t> m_free;  // FIFO ring of free slots: a freed slot is reused last
    uint32_t m_freeHead = 0, m_freeCount = 0;
    std::vector<Store> m_stores;  // by ComponentId
    std::vector<Prefab> m_prefabs;
    Commands m_commands;
    int m_iterating = 0;
};

// ── Templates ────────────────────────────────────────────────────────────────

template <class T>
ComponentId World::id() const {
    ComponentId c = m_reg.id<T>();
    if (c == kNoComponent) throw std::logic_error("component type not in the registry");
    return c;
}

template <class T>
void Commands::add(Entity e, const T& value) {
    add(e, m_world.id<T>(), &value);
}

template <class... Cs, class F>
void World::each(F&& fn) {
    static_assert(sizeof...(Cs) > 0, "each<>() needs at least one component");
    const ComponentId ids[] = {id<Cs>()...};
    size_t smallest = 0;
    for (size_t i = 1; i < sizeof...(Cs); ++i)
        if (m_stores[ids[i]].slots.size() < m_stores[ids[smallest]].slots.size()) smallest = i;
    Store& lead = m_stores[ids[smallest]];
    ++m_iterating;
    struct Guard {
        int& n;
        ~Guard() { --n; }
    } guard{m_iterating};
    for (size_t i = 0; i < lead.slots.size(); ++i) {
        uint16_t slot = lead.slots[i];
        bool all = true;
        for (ComponentId c : ids) all = all && m_stores[c].sparse[slot] != kAbsent;
        if (!all) continue;
        [&]<size_t... I>(std::index_sequence<I...>) {
            fn(makeEntity(slot, m_generation[slot]),
               *reinterpret_cast<Cs*>(m_stores[ids[I]].at(m_stores[ids[I]].sparse[slot]))...);
        }(std::index_sequence_for<Cs...>{});
    }
}

}  // namespace eng::ecs
