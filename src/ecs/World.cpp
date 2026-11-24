#include "eng/ecs/World.h"

#include "eng/core/Log.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace eng::ecs {

World::World(const Registry& registry, WorldConfig config) : m_reg(registry), m_config(config), m_commands(*this) {
    if (!registry.finalized()) throw std::logic_error("World: the registry must be finalized first");
    if (config.maxEntities < 1 || config.maxEntities > kMaxEntityCap)
        throw std::invalid_argument("World: maxEntities must be 1..65535");
    m_cap = config.maxEntities;
    // Everything per slot is sized here, once: no reallocation mid-game.
    m_generation.assign(m_cap, 1);
    m_alive.assign(m_cap, 0);
    m_prefabOf.assign(m_cap, kNoPrefab);
    m_free.resize(m_cap);
    for (uint32_t i = 0; i < m_cap; ++i) m_free[i] = uint16_t(i);
    m_freeCount = m_cap;
    m_stores.resize(registry.count());
    for (size_t c = 0; c < registry.count(); ++c) {
        m_stores[c].size = registry.info(ComponentId(c)).size;
        m_stores[c].sparse.assign(m_cap, kAbsent);
    }
}

void World::checkStructural(const char* what) const {
    if (m_iterating) throw std::logic_error(std::string("World: ") + what + " while iterating - use commands()");
}

// ── Prefabs ──────────────────────────────────────────────────────────────────

PrefabId World::definePrefab(std::string name, uint16_t max) {
    if (findPrefab(name) != kNoPrefab) throw std::invalid_argument("World: prefab '" + name + "' defined twice");
    if (m_prefabs.size() >= kNoPrefab) throw std::length_error("World: too many prefabs");
    Prefab p;
    p.name = std::move(name);
    p.max = max ? max : m_config.defaultPrefabMax;
    m_prefabs.push_back(std::move(p));
    return PrefabId(m_prefabs.size() - 1);
}

PrefabId World::findPrefab(std::string_view name) const {
    for (size_t i = 0; i < m_prefabs.size(); ++i)
        if (m_prefabs[i].name == name) return PrefabId(i);
    return kNoPrefab;
}

void World::setPrefabComponent(PrefabId p, ComponentId c, const void* bytes) {
    const ComponentInfo& info = m_reg.info(c);
    std::vector<std::byte> data(info.size);
    std::memcpy(data.data(), bytes ? bytes : info.defaults.data(), info.size);
    for (auto& [id, existing] : m_prefabs.at(p).components)
        if (id == c) {
            existing = std::move(data);
            return;
        }
    m_prefabs[p].components.emplace_back(c, std::move(data));
}

// ── Entities ─────────────────────────────────────────────────────────────────

Entity World::reserve(PrefabId prefab) {
    if (prefab != kNoPrefab) {
        Prefab& p = m_prefabs.at(prefab);
        if (p.live >= p.max) {
            ENG_LOG("[ecs] spawn refused: prefab '" << p.name << "' is at its cap of " << p.max);
            return kNullEntity;
        }
    }
    if (m_freeCount == 0) {
        ENG_LOG("[ecs] spawn refused: the world is at its cap of " << m_cap << " entities");
        return kNullEntity;
    }
    uint16_t slot = m_free[m_freeHead];
    m_freeHead = (m_freeHead + 1) % m_cap;
    --m_freeCount;
    m_alive[slot] = 1;
    m_prefabOf[slot] = prefab;
    ++m_live;
    if (prefab != kNoPrefab) ++m_prefabs[prefab].live;
    return makeEntity(slot, m_generation[slot]);
}

void World::applyPrefab(Entity e, PrefabId prefab) {
    if (prefab == kNoPrefab) return;
    for (const auto& [c, bytes] : m_prefabs[prefab].components) addToSlot(entitySlot(e), c, bytes.data());
}

Entity World::spawn(PrefabId prefab) {
    checkStructural("spawn");
    Entity e = reserve(prefab);
    if (e != kNullEntity) applyPrefab(e, prefab);
    return e;
}

Entity World::spawnAt(Entity e, PrefabId prefab) {
    checkStructural("spawnAt");
    uint16_t slot = entitySlot(e), gen = entityGeneration(e);
    if (slot >= m_cap || gen == 0) throw std::invalid_argument("World: spawnAt with an entity outside this world");
    if (m_alive[slot]) throw std::logic_error("World: spawnAt a slot that's in use");
    if (prefab != kNoPrefab && prefab >= m_prefabs.size()) throw std::invalid_argument("World: spawnAt with an unknown prefab");
    // Take the slot out of the free ring (keeping the others' order).
    for (uint32_t i = 0; i < m_freeCount; ++i) {
        if (m_free[(m_freeHead + i) % m_cap] != slot) continue;
        for (uint32_t j = i; j + 1 < m_freeCount; ++j) m_free[(m_freeHead + j) % m_cap] = m_free[(m_freeHead + j + 1) % m_cap];
        --m_freeCount;
        break;
    }
    m_generation[slot] = gen;
    m_alive[slot] = 1;
    m_prefabOf[slot] = prefab;
    ++m_live;
    if (prefab != kNoPrefab) ++m_prefabs[prefab].live;
    applyPrefab(e, prefab);
    return e;
}

void World::destroy(Entity e) {
    checkStructural("destroy");
    if (!alive(e)) return;
    uint16_t slot = entitySlot(e);
    for (size_t c = 0; c < m_stores.size(); ++c)
        if (m_stores[c].sparse[slot] != kAbsent) removeFromSlot(slot, ComponentId(c));
    if (m_prefabOf[slot] != kNoPrefab) --m_prefabs[m_prefabOf[slot]].live;
    m_prefabOf[slot] = kNoPrefab;
    m_alive[slot] = 0;
    // A new generation for the slot, so handles to this entity go stale;
    // never 0, so no live entity is ever kNullEntity.
    if (++m_generation[slot] == 0) m_generation[slot] = 1;
    m_free[(m_freeHead + m_freeCount) % m_cap] = slot;
    ++m_freeCount;
    --m_live;
}

bool World::alive(Entity e) const {
    uint16_t slot = entitySlot(e);
    return e != kNullEntity && slot < m_cap && m_alive[slot] && m_generation[slot] == entityGeneration(e);
}

PrefabId World::prefabOf(Entity e) const { return alive(e) ? m_prefabOf[entitySlot(e)] : kNoPrefab; }

Entity World::entityAt(uint16_t slot) const {
    return slot < m_cap && m_alive[slot] ? makeEntity(slot, m_generation[slot]) : kNullEntity;
}

// ── Components ───────────────────────────────────────────────────────────────

void World::reserveComponents(ComponentId c, size_t n) {
    Store& s = m_stores.at(c);
    n = std::min<size_t>(n, m_cap);
    s.slots.reserve(n);
    s.data.reserve(n * s.size);
}

void* World::addToSlot(uint16_t slot, ComponentId c, const void* bytes) {
    Store& s = m_stores[c];
    const void* src = bytes ? bytes : m_reg.info(c).defaults.data();
    if (s.sparse[slot] != kAbsent) {  // already there: overwrite
        std::byte* at = s.at(s.sparse[slot]);
        std::memmove(at, src, s.size);
        return at;
    }
    auto dense = uint16_t(s.slots.size());
    s.slots.push_back(slot);
    s.data.resize(s.data.size() + s.size);
    s.sparse[slot] = dense;
    std::byte* at = s.at(dense);
    std::memcpy(at, src, s.size);
    return at;
}

void World::removeFromSlot(uint16_t slot, ComponentId c) {
    Store& s = m_stores[c];
    uint16_t dense = s.sparse[slot];
    if (dense == kAbsent) return;
    auto last = uint16_t(s.slots.size() - 1);
    if (dense != last) {  // the last element fills the hole
        uint16_t moved = s.slots[last];
        std::memcpy(s.at(dense), s.at(last), s.size);
        s.slots[dense] = moved;
        s.sparse[moved] = dense;
    }
    s.slots.pop_back();
    s.data.resize(s.data.size() - s.size);
    s.sparse[slot] = kAbsent;
}

void* World::add(Entity e, ComponentId c, const void* bytes) {
    checkStructural("add");
    if (!alive(e)) return nullptr;
    return addToSlot(entitySlot(e), c, bytes);
}

void World::remove(Entity e, ComponentId c) {
    checkStructural("remove");
    if (alive(e)) removeFromSlot(entitySlot(e), c);
}

bool World::has(Entity e, ComponentId c) const { return alive(e) && m_stores[c].sparse[entitySlot(e)] != kAbsent; }

void* World::get(Entity e, ComponentId c) {
    if (!alive(e)) return nullptr;
    Store& s = m_stores[c];
    uint16_t dense = s.sparse[entitySlot(e)];
    return dense == kAbsent ? nullptr : s.at(dense);
}

const void* World::get(Entity e, ComponentId c) const { return const_cast<World*>(this)->get(e, c); }

void World::query(std::span<const ComponentId> components, const std::function<void(Entity)>& fn) {
    if (components.empty()) return;
    size_t smallest = 0;
    for (size_t i = 1; i < components.size(); ++i)
        if (m_stores[components[i]].slots.size() < m_stores[components[smallest]].slots.size()) smallest = i;
    const Store& lead = m_stores[components[smallest]];
    ++m_iterating;
    struct Guard {
        int& n;
        ~Guard() { --n; }
    } guard{m_iterating};
    for (size_t i = 0; i < lead.slots.size(); ++i) {
        uint16_t slot = lead.slots[i];
        bool all = true;
        for (ComponentId c : components) all = all && m_stores[c].sparse[slot] != kAbsent;
        if (all) fn(makeEntity(slot, m_generation[slot]));
    }
}

// ── Commands ─────────────────────────────────────────────────────────────────

Entity Commands::spawn(PrefabId prefab) {
    Entity e = m_world.reserve(prefab);  // caps decided now; components at flush
    if (e != kNullEntity) m_ops.push_back({Op::Spawn, e, kNoComponent, prefab, 0});
    return e;
}

void Commands::destroy(Entity e) { m_ops.push_back({Op::Destroy, e, kNoComponent, kNoPrefab, 0}); }

void Commands::add(Entity e, ComponentId c, const void* bytes) {
    uint32_t at = UINT32_MAX;
    if (bytes) {
        at = uint32_t(m_bytes.size());
        uint32_t size = m_world.m_reg.info(c).size;
        m_bytes.resize(m_bytes.size() + size);
        std::memcpy(m_bytes.data() + at, bytes, size);
    }
    m_ops.push_back({Op::Add, e, c, kNoPrefab, at});
}

void Commands::remove(Entity e, ComponentId c) { m_ops.push_back({Op::Remove, e, c, kNoPrefab, 0}); }

void World::flush() {
    checkStructural("flush");
    // Ops may record more (a component added in an earlier op is fine); take
    // the list so anything queued during the flush waits for the next one.
    std::vector<Commands::Record> ops;
    std::vector<std::byte> bytes;
    ops.swap(m_commands.m_ops);
    bytes.swap(m_commands.m_bytes);
    for (const Commands::Record& r : ops) {
        switch (r.op) {
            case Commands::Op::Spawn:
                if (alive(r.entity)) applyPrefab(r.entity, r.prefab);  // unless destroyed since
                break;
            case Commands::Op::Destroy: destroy(r.entity); break;
            case Commands::Op::Add: add(r.entity, r.component, r.bytesAt == UINT32_MAX ? nullptr : bytes.data() + r.bytesAt); break;
            case Commands::Op::Remove: remove(r.entity, r.component); break;
        }
    }
}

}  // namespace eng::ecs
