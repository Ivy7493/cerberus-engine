# ECS + Lua plan

Status: phases 1 and 2 done (ECS core in `eng/ecs/`, tested by `tests/ecs_test.cpp`); phase 3, the Lua bridge, next (branch `ecs-lua`).

## Decisions

- **Core in C++** (`eng/ecs/`, `eng/script/`); zig is the build only.
- **Lua 5.4 from source** via `build.zig.zon`. A host-target `luac` is built and compiles game scripts
  to bytecode for release builds.
- **Mods supported** — as Lua text, sandboxed.
- **Components replicate** through the network protocol, driven by the component registry.
- **Caps:** total entity cap configurable per `World` (up to 65535 — slots are `uint16`); 8 per prefab
  by default (overridable per prefab).

## Runtime vs file-based

Both, in layers:

| Layer | Where | Delve today |
|---|---|---|
| Definitions / prefabs (what a Hound *is*) | Lua table files | `EnemyDef`, `ItemDef` |
| Placements (what's in a room) | Lua table files | `RoomTemplates.cpp` |
| Instances (this Hound, hp 2, at 3,4) | Runtime only, spawned from a prefab | `World` enemies |
| Snapshots (net, save, replay) | Serialized from runtime via the registry | `Protocol.h` |

```lua
-- prefabs/hound.lua
return {
  max      = 8,                 -- per-prefab cap (default 8)
  Sprite   = { sheet = "enemies", frame = 12 },
  Health   = { hp = 3 },
  Collider = { radius = 0.4 },
  Brain    = { script = "ai/hound" },
}
```

## ECS core

- **Entity = `uint32`**: 16-bit slot + 16-bit generation (stale handles detectable). Slot `0xFFFF` is
  reserved, so the cap is 1..65535. Fits a Lua integer, so Lua handles are plain numbers.
- **Configurable cap** (`WorldConfig::maxEntities`, default 256): everything sized from it at creation,
  no reallocation mid-game.
- **Storage: sparse sets** per component — a `uint16` slot→dense index array sized to the cap (128 KB at
  the 65535 max) + packed dense component data, so memory follows what's in use, not cap × size.
  Queries walk the smallest set and check the others. Dense order follows add/remove history, which is
  deterministic given a deterministic sim.
- **Caps:** `spawn` returns a null entity when the total cap or the prefab's `max` is reached (logged in
  dev). Spawning is server-only, so it is deterministic.
- **Not in the ECS:** bullets, particles, coins — high-count, short-lived; stay plain C++ arrays.
- **Command buffer:** spawn/destroy during a system run is deferred to between systems.
- **Component registry** (the core of everything): name, size, fields (name, offset, type, net
  encoding, replicated?, interpolated?). Drives Lua field access, prefab loading/validation,
  snapshots/net, saves, and an eng::ui inspector.

## Lua

- One `lua_State` per `World` (simtest can run worlds side by side; client prediction gets its own).
- **Dev:** load `.lua` from disk, hot-reload on change. **Release:** base game loads `.luac`
  (`luaL_loadbufferx` mode `"b"`). Keep debug info (no `luac -s`) so tracebacks stay useful.
- Bytecode depends on Lua version and `lua_Integer`/`lua_Number`/`Instruction` sizes — identical on our
  x86_64 Linux and Windows targets; `luac` is built from the same pinned sources as the runtime.
- **Determinism:** `luai_makeseed` defined to a constant (else string-hash seeds — and `pairs()` order —
  differ per state); `math.random` bound to the sim's `Rng`; no `os`/`io`/`debug` in sim states.
- **Rule: game state lives in components, never in Lua locals/tables/closures** — otherwise it isn't
  replicated, saved, or rolled back. Scripts are functions over components. Per-mod scratch tables exist
  for non-sim things (UI).
- Scripts are tagged `server` (AI; clients don't run it) or `shared`.

## Mods

- Two trust levels: base game `.luac` (mode `"b"`, from install dir); mods `.lua` **text only** (mode
  `"t"`) — malformed bytecode can corrupt memory.
- Sandbox per mod: own `_ENV` without `os`, `io`, `debug`, `string.dump`, `package.loadlib`,
  `collectgarbage`, binary `load`; a custom `require` limited to the mod's folder + the base API.
- Instruction-count hook aborts runaway callbacks; custom `lua_Alloc` caps memory. A faulting mod
  script is logged and disabled, the game continues.
- Mods can define components from a schema (`component "Frozen" { time = "f32q", stacks = "u8" }`),
  stored as bytes with the same reflection, so they replicate and save for free.
- Load order: base, then mods in a player-chosen list; later prefabs override earlier ones by name
  (conflicts logged).
- Multiplayer: join handshake sends a manifest (mod ids, versions, content hashes) + component schema
  hash; **mismatch refuses the join** with the list of differences. Server-pushed mods / Workshop later.

## Network

- Component ids assigned after mod load, **sorted by name** (never registration order); verified by the
  schema hash.
- Snapshots unreliable, delta-encoded against the client's last acked snapshot (server keeps a ring of
  ~32; falls back to a full snapshot):
  - spawned and despawned slots as lists (varint slots: 1 byte under 128, 2 under 16384). A bitmask
    of the whole cap would be 8 KB at 65535.
  - per changed entity: slot (varint), prefab id on spawn, changed-component mask, per component a changed-field
    mask + values (`u8`/varint/int16 fixed-point via `ByteWriter`)
- Keep packets ≲1200 bytes: `UdpTransport` relies on IP fragmentation for large messages (fine on
  loopback only). Full snapshots split by entity range; each piece applies independently.
- Client: remote entities interpolated from snapshot buffers; local player stays C++-predicted.

## Phases

1. **Build + script runtime:** Lua 5.4 in `build.zig.zon` (constant `luai_makeseed`), host `luac` +
   bytecode compile step for consumers, `eng/script` (states, dev/release loaders, sandbox, instruction
   and memory limits, tracebacks to `ENG_LOG`, hot reload).
2. **ECS core:** entities, configurable cap, sparse-set storage, prefab caps, command buffer, registry with net metadata.
3. **Lua bridge:** entity userdata, reflected field access, `spawn`/`destroy`/`query`/events,
   Lua-defined components.
4. **Prefabs + mods:** prefab loading/validation, mod discovery and load order, overrides, manifest and
   hashes.
5. **Net:** snapshot ring, delta encoding, split packets, client interpolation, schema/mod handshake;
   tested under `LagTransport`.
6. **Delve pilot:** one enemy type end to end, `delve_simtest` green.
