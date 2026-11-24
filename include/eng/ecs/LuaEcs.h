#pragma once

// eng::ecs::LuaEcs — the ECS as scripts see it: the `ecs` and `events`
// globals, shared (read-only) by every script environment. Phase 3 of
// docs/ecs-lua-plan.md.
//
//   eng::Scripts scripts;  ecs::Registry reg;
//   ecs::LuaEcs luaEcs(scripts, reg);   // before scripts declare components
//   ... register C++ components; run scripts that call ecs.component(...) ...
//   reg.finalize();
//   ecs::World world(reg);
//   luaEcs.attach(&world);
//
// Lua API (entities are plain integers; components by name or ecs.id(name)):
//
//   ecs.component(name, schema)   declare a component (only before finalize)
//       schema: { {"time", "f32"}, {"stacks", "u8"} }   fields in this order, or
//               { time = "f32", stacks = "u8" }         fields sorted by name;
//       a spec is a type name (bool i8 u8 i16 u16 i32 u32 f32 vec2 entity) or
//       { type = "f32", net = "raw"|"varint"|"fixed16", scale = 0.01,
//         replicated = true, interpolated = false }
//   ecs.id(name)                  component id, or nil
//   ecs.spawn([prefab])           entity, or nil at a cap
//   ecs.destroy(e)  ecs.alive(e)  ecs.prefab(e) → name or nil
//   ecs.add(e, comp [, init])     → component ref; init = { field = value, ... }
//   ecs.remove(e, comp)  ecs.has(e, comp)
//   ecs.get(e, comp)              → component ref, or nil
//   ecs.each(comp, ..., fn)       fn(e) for every entity with all of them
//   ecs.count(comp)  ecs.size()  ecs.capacity()
//   events.on(name, fn)  events.emit(name, ...)
//
// A component ref reads and writes the entity's fields by reflection
// (h.hp = h.hp - 1); it looks the entity up on every access, so it never
// dangles — it raises an error once the entity or component is gone.
// A vec2 field reads as a ref too (pos.p.x = 3 writes through) and accepts a
// ref, {x=, y=} or {x, y}. Integer fields are range-checked; unknown fields
// and wrong types are errors naming the component.
//
// Structural changes made while the world is iterating (inside ecs.each, or
// a C++ each/query) go through world.commands(): the game flushes them
// between systems. spawn still returns the entity at once.
//
// Event handlers run in registration order. A handler belongs to the file
// that defines its function; when Scripts hot-reloads that file, its
// handlers are dropped before the file re-runs and re-registers them.

#include "eng/ecs/World.h"

#include <string_view>

namespace eng {
class Scripts;
}

namespace eng::ecs {

class LuaEcs {
public:
    // Exposes `ecs` and `events` in `scripts`. Both must outlive this, and
    // this must outlive any script call that uses them.
    LuaEcs(Scripts& scripts, Registry& registry);
    ~LuaEcs();
    LuaEcs(const LuaEcs&) = delete;
    LuaEcs& operator=(const LuaEcs&) = delete;

    // The world scripts act on (nullptr: none; ecs calls then raise errors).
    void attach(World* world) { m_world = world; }
    World* world() const { return m_world; }

    // Calls every handler of `name` with the `nargs` values on top of the
    // stack (popped), each as its own Scripts::call. A failing handler is
    // logged and the rest still run; returns false if any failed.
    bool emit(std::string_view name, int nargs);

private:
    friend struct LuaEcsAccess;
    Scripts& m_scripts;
    Registry& m_registry;
    World* m_world = nullptr;
    int m_handlersRef = 0;  // registry ref: event name → { {fn=, src=}, ... }
};

}  // namespace eng::ecs
