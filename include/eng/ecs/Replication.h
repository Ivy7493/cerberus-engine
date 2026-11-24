#pragma once

// eng::ecs replication — server-authoritative snapshots of a World, delta
// encoded per client and split into packets that fit a UDP datagram, and a
// client that rebuilds them into a mirror World with interpolation. Phase 5
// of docs/ecs-lua-plan.md. Transport-agnostic: it makes and takes bytes; the
// game's protocol frames, sends (unreliable) and acks them.
//
// Server, once per tick:
//   server.capture(world, tick);                        // tick: increasing, nonzero
//   for (auto& c : clients)
//       for (auto& packet : server.encode(c.ackedTick)) transport.send(c.peer, packet, Unreliable);
// Client:
//   client.receive(bytes, size);                        // each packet, in any order
//   send client.ackTick() to the server (e.g. in its input messages)
//   client.apply(mirror, renderTick);                   // e.g. latestTick() + fraction - delay
//
// What replicates: components with at least one replicated field (just
// those fields), and field-less (tag) components, by presence. Components
// whose fields are all server-only don't exist on the wire; the mirror keeps
// whatever its prefabs gave it. Values go over the wire in their
// NetEncoding: Fixed16 floats/vec2s are quantized (round(v / scale), int16),
// so the client sees the quantized value; Varint integers are zigzag
// varints; everything else raw.
//
// Deltas: each packet set is a delta against a snapshot the client
// acknowledged (the server keeps the last `history`); with no usable
// baseline it's a full snapshot. A changed entity carries only its changed
// components and, in them, only the changed fields (a bit mask). Entities
// are mirrored at the same slot and generation (World::spawnAt), so Entity
// handles and Entity fields mean the same on both sides.
//
// Packets: a snapshot is split into parts of at most maxPacketBytes (an
// entity's record is never split, so one huge entity can make a part
// bigger). The client needs every part of a tick before it uses any;
// a lost part loses that snapshot (the next delta is still against the
// last acked one, so nothing desyncs). Malformed or hostile packets are
// rejected without touching anything.
//
// Both sides need the same Registry (compare schema hashes when joining —
// ContentManifest) and the same prefabs (Content defines them in the same
// order), and the mirror World the same capacity.

#include "eng/ecs/World.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace eng::ecs {

struct ReplicationConfig {
    uint32_t maxPacketBytes = 1200;  // below typical internet MTU after IP/UDP/transport headers
    uint32_t history = 32;           // snapshots kept for baselines (server) and interpolation (client)
};

namespace detail {
struct NetSchema;
struct Snapshot;
}

class SnapshotServer {
public:
    explicit SnapshotServer(const Registry& registry, ReplicationConfig config = {});
    ~SnapshotServer();

    // Records the world's replicated state as snapshot `tick`.
    void capture(const World& world, uint32_t tick);
    uint32_t latestTick() const;

    // The newest snapshot's packets for a client whose newest acknowledged
    // tick is `ackedTick` (0: none — a full snapshot). Empty before any capture.
    std::vector<std::vector<uint8_t>> encode(uint32_t ackedTick) const;

private:
    std::unique_ptr<detail::NetSchema> m_schema;
    ReplicationConfig m_config;
    std::vector<std::unique_ptr<detail::Snapshot>> m_history;  // oldest first
};

class SnapshotClient {
public:
    explicit SnapshotClient(const Registry& registry, ReplicationConfig config = {});
    ~SnapshotClient();

    // One packet. False if it's malformed (it's ignored); true otherwise,
    // including duplicates, stale ticks and parts still waiting for the rest.
    bool receive(const void* data, size_t size);

    uint32_t ackTick() const;     // newest complete snapshot: tell the server
    uint32_t latestTick() const;  // same thing; 0 before the first
    uint32_t oldestTick() const;  // oldest still held (for interpolation delay)

    // Makes `mirror` the server's state at `tick`: entities spawned/destroyed
    // to match, replicated components added/removed, fields written.
    // Between two held snapshots, interpolated fields (F32/Vec2) are blended;
    // everything else takes the earlier one. Clamps to the held range.
    // Not while the mirror is iterating.
    void apply(World& mirror, double tick) const;

    void reset();  // forget everything (reconnecting)

private:
    struct Pending;
    bool complete(Pending& p, uint32_t tick);

    std::unique_ptr<detail::NetSchema> m_schema;
    ReplicationConfig m_config;
    std::vector<std::unique_ptr<detail::Snapshot>> m_history;  // by tick, oldest first
    std::vector<std::unique_ptr<Pending>> m_pending;
};

}  // namespace eng::ecs
