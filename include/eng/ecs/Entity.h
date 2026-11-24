#pragma once

#include <cstdint>

namespace eng::ecs {

// An entity handle: low 16 bits the slot, high 16 the slot's generation when
// it was handed out. A destroyed entity's slot gets a new generation, so old
// handles to it stop resolving instead of pointing at whatever reuses the
// slot. Generations start at 1, so 0 is never a live entity: a
// zero-initialized handle, and Lua's "no entity", are kNullEntity.
// It fits a Lua integer, so scripts hold entities as plain numbers.
using Entity = uint32_t;
constexpr Entity kNullEntity = 0;

constexpr uint16_t entitySlot(Entity e) { return static_cast<uint16_t>(e & 0xFFFFu); }
constexpr uint16_t entityGeneration(Entity e) { return static_cast<uint16_t>(e >> 16); }
constexpr Entity makeEntity(uint16_t slot, uint16_t generation) {
    return (static_cast<uint32_t>(generation) << 16) | slot;
}

// Slot 0xFFFF is never used, so a World holds at most 65535 entities.
constexpr uint32_t kMaxEntityCap = 0xFFFF;

}  // namespace eng::ecs
