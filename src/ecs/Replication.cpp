#include "eng/ecs/Replication.h"

#include "eng/core/Log.h"
#include "eng/net/ByteStream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace eng::ecs {

// ── Wire layout of a snapshot part ───────────────────────────────────────────
//
//   u32 tick, u32 baseTick (0: full), u16 part, u16 partCount
//   varU32 nRemoved, nRemoved × varU32 slot
//   varU32 nRecords, records:
//     varU32 slot, u8 spawned, [spawned: u16 generation, varU32 prefab + 1 (0: none)]
//     varU32 nOps, ops: varU32 component, u8 op (0 remove, 1 set),
//                       [set: ceil(fields / 8) mask bytes, then each set field's value]
//
// Removals in every part apply before any record (a slot can be removed and
// respawned with a new generation in the same tick).

namespace detail {

namespace {
constexpr size_t kHeaderBytes = 12;
constexpr uint32_t kMaxSlot = kMaxEntityCap - 1;
}  // namespace

struct NetField {
    uint16_t index;   // into ComponentInfo::fields
    FieldType type;
    uint32_t offset;  // in the component
    uint32_t wireAt;  // in the canonical wire bytes
    uint8_t wire;     // canonical size
    bool fixed16;     // F32/Vec2 quantized to int16s
    bool varint;      // integers as varints
    bool interpolated;
    float scale;
};

struct NetComponent {
    bool replicated = false;
    std::vector<NetField> fields;
    uint32_t wireSize = 0;
    std::vector<uint8_t> defaults;  // the component's defaults in wire form
};

struct NetEntity {
    uint16_t slot = 0, gen = 0;
    PrefabId prefab = kNoPrefab;
    std::vector<std::pair<ComponentId, std::vector<uint8_t>>> comps;  // sorted by id

    const std::vector<uint8_t>* comp(ComponentId c) const {
        for (const auto& [id, bytes] : comps)
            if (id == c) return &bytes;
        return nullptr;
    }
};

struct Snapshot {
    uint32_t tick = 0;
    std::vector<NetEntity> ents;  // sorted by slot

    const NetEntity* find(uint16_t slot) const {
        auto it = std::lower_bound(ents.begin(), ents.end(), slot, [](const NetEntity& e, uint16_t s) { return e.slot < s; });
        return it != ents.end() && it->slot == slot ? &*it : nullptr;
    }
};

namespace {

int16_t quantize(float v, float scale) {
    double s = std::isfinite(v) ? double(v) / scale : 0.0;
    s = std::clamp(s, -32768.0, 32767.0);
    return int16_t(std::lround(s));
}

bool isSigned(FieldType t) { return t == FieldType::I8 || t == FieldType::I16 || t == FieldType::I32; }
bool isInteger(FieldType t) {
    return t == FieldType::I8 || t == FieldType::U8 || t == FieldType::I16 || t == FieldType::U16 || t == FieldType::I32 ||
           t == FieldType::U32 || t == FieldType::Entity;
}

int64_t loadInteger(FieldType t, const uint8_t* p) {
    switch (t) {
        case FieldType::I8: { int8_t v; std::memcpy(&v, p, 1); return v; }
        case FieldType::U8: return p[0];
        case FieldType::I16: { int16_t v; std::memcpy(&v, p, 2); return v; }
        case FieldType::U16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        case FieldType::I32: { int32_t v; std::memcpy(&v, p, 4); return v; }
        default: { uint32_t v; std::memcpy(&v, p, 4); return v; }
    }
}

bool storeInteger(FieldType t, int64_t v, uint8_t* p) {
    auto put = [&](auto typed) {
        using T = decltype(typed);
        if (v < int64_t(std::numeric_limits<T>::min()) || v > int64_t(std::numeric_limits<T>::max())) return false;
        T x = T(v);
        std::memcpy(p, &x, sizeof x);
        return true;
    };
    switch (t) {
        case FieldType::I8: return put(int8_t{});
        case FieldType::U8: return put(uint8_t{});
        case FieldType::I16: return put(int16_t{});
        case FieldType::U16: return put(uint16_t{});
        case FieldType::I32: return put(int32_t{});
        default: return put(uint32_t{});
    }
}

void writeField(ByteWriter& w, const NetField& f, const uint8_t* canon) {
    if (!f.varint) {
        w.bytes(canon, f.wire);
        return;
    }
    int64_t v = loadInteger(f.type, canon);
    if (isSigned(f.type)) {
        auto s = int32_t(v);
        w.varU32((uint32_t(s) << 1) ^ uint32_t(s >> 31));  // zigzag
    } else {
        w.varU32(uint32_t(v));
    }
}

bool readField(ByteReader& r, const NetField& f, uint8_t* canon) {
    if (!f.varint) {
        if (r.remaining() < f.wire) return false;
        std::memcpy(canon, r.cursor(), f.wire);
        r.skip(f.wire);
        if (f.type == FieldType::Bool) canon[0] = canon[0] != 0;
        return true;
    }
    uint32_t u = r.varU32();
    if (!r.ok()) return false;
    int64_t v = isSigned(f.type) ? int64_t(int32_t((u >> 1) ^ (0u - (u & 1)))) : int64_t(u);
    return storeInteger(f.type, v, canon);
}

size_t varSize(uint32_t v) {
    size_t n = 1;
    while (v >= 0x80) v >>= 7, ++n;
    return n;
}

}  // namespace

struct NetSchema {
    const Registry& reg;
    std::vector<NetComponent> comps;     // by ComponentId
    std::vector<ComponentId> replicated;  // ascending

    explicit NetSchema(const Registry& r) : reg(r) {
        if (!r.finalized()) throw std::logic_error("replication: the registry must be finalized first");
        comps.resize(r.count());
        for (size_t c = 0; c < r.count(); ++c) {
            const ComponentInfo& info = r.info(ComponentId(c));
            NetComponent& nc = comps[c];
            for (size_t i = 0; i < info.fields.size(); ++i) {
                const FieldInfo& fi = info.fields[i];
                if (!fi.options.replicated) continue;
                NetField f{};
                f.index = uint16_t(i);
                f.type = fi.type;
                f.offset = fi.offset;
                f.wireAt = nc.wireSize;
                bool floaty = fi.type == FieldType::F32 || fi.type == FieldType::Vec2;
                f.fixed16 = floaty && fi.options.net == NetEncoding::Fixed16 && fi.options.scale > 0.0f;
                f.varint = isInteger(fi.type) && fi.options.net == NetEncoding::Varint;
                f.interpolated = floaty && fi.options.interpolated;
                f.scale = fi.options.scale;
                f.wire = uint8_t(f.fixed16 ? (fi.type == FieldType::Vec2 ? 4 : 2) : fieldTypeSize(fi.type));
                nc.wireSize += f.wire;
                nc.fields.push_back(f);
            }
            // Field-less components are tags: their presence replicates.
            nc.replicated = !nc.fields.empty() || info.fields.empty();
            if (!nc.replicated) continue;
            nc.defaults.resize(nc.wireSize);
            toWire(nc, info.defaults.data(), nc.defaults.data());
            replicated.push_back(ComponentId(c));
        }
    }

    static void toWire(const NetComponent& nc, const std::byte* comp, uint8_t* out) {
        for (const NetField& f : nc.fields) {
            const std::byte* at = comp + f.offset;
            uint8_t* dst = out + f.wireAt;
            if (f.fixed16) {
                int n = f.type == FieldType::Vec2 ? 2 : 1;
                for (int k = 0; k < n; ++k) {
                    float v;
                    std::memcpy(&v, at + k * sizeof(float), sizeof v);
                    int16_t q = quantize(v, f.scale);
                    std::memcpy(dst + k * 2, &q, 2);
                }
            } else {
                std::memcpy(dst, at, f.wire);
                if (f.type == FieldType::Bool) dst[0] = dst[0] != 0;
            }
        }
    }

    // Wire bytes `a` (blended toward `b` by t for interpolated fields, if b) into the component.
    static void fromWire(const NetComponent& nc, const uint8_t* a, const uint8_t* b, float t, std::byte* comp) {
        for (const NetField& f : nc.fields) {
            std::byte* dst = comp + f.offset;
            if (f.type != FieldType::F32 && f.type != FieldType::Vec2) {
                std::memcpy(dst, a + f.wireAt, f.wire);
                continue;
            }
            int n = f.type == FieldType::Vec2 ? 2 : 1;
            for (int k = 0; k < n; ++k) {
                auto decode = [&](const uint8_t* src) {
                    if (f.fixed16) {
                        int16_t q;
                        std::memcpy(&q, src + f.wireAt + k * 2, 2);
                        return float(q) * f.scale;
                    }
                    float v;
                    std::memcpy(&v, src + f.wireAt + k * 4, 4);
                    return v;
                };
                float v = decode(a);
                if (b && f.interpolated) v += (decode(b) - v) * t;
                std::memcpy(dst + k * sizeof(float), &v, sizeof v);
            }
        }
    }
};

}  // namespace detail

using detail::NetComponent;
using detail::NetEntity;
using detail::NetField;
using detail::NetSchema;
using detail::Snapshot;

// ── Server ───────────────────────────────────────────────────────────────────

SnapshotServer::SnapshotServer(const Registry& registry, ReplicationConfig config)
    : m_schema(std::make_unique<NetSchema>(registry)), m_config(config) {
    if (m_config.history < 1) m_config.history = 1;
}
SnapshotServer::~SnapshotServer() = default;

uint32_t SnapshotServer::latestTick() const { return m_history.empty() ? 0 : m_history.back()->tick; }

void SnapshotServer::capture(const World& world, uint32_t tick) {
    if (tick == 0 || tick <= latestTick()) throw std::invalid_argument("SnapshotServer: ticks must be nonzero and increasing");
    auto snap = std::make_unique<Snapshot>();
    snap->tick = tick;
    for (uint32_t slot = 0; slot < world.capacity(); ++slot) {
        Entity e = world.entityAt(uint16_t(slot));
        if (e == kNullEntity) continue;
        NetEntity ne;
        ne.slot = uint16_t(slot);
        ne.gen = entityGeneration(e);
        ne.prefab = world.prefabOf(e);
        for (ComponentId c : m_schema->replicated) {
            const void* p = world.get(e, c);
            if (!p) continue;
            const NetComponent& nc = m_schema->comps[c];
            std::vector<uint8_t> wire(nc.wireSize);
            NetSchema::toWire(nc, static_cast<const std::byte*>(p), wire.data());
            ne.comps.emplace_back(c, std::move(wire));
        }
        snap->ents.push_back(std::move(ne));
    }
    m_history.push_back(std::move(snap));
    while (m_history.size() > m_config.history) m_history.erase(m_history.begin());
}

std::vector<std::vector<uint8_t>> SnapshotServer::encode(uint32_t ackedTick) const {
    if (m_history.empty()) return {};
    const Snapshot& cur = *m_history.back();
    // The client already has the newest tick (we're sending faster than we
    // capture): nothing to send. A delta against itself would carry
    // baseTick == tick, which receive() rightly rejects as malformed.
    if (ackedTick == cur.tick) return {};
    const Snapshot* base = nullptr;
    if (ackedTick)
        for (const auto& s : m_history)
            if (s->tick == ackedTick && s->tick < cur.tick) base = s.get();

    std::vector<uint16_t> removed;
    if (base)
        for (const NetEntity& be : base->ents) {
            const NetEntity* ce = cur.find(be.slot);
            if (!ce || ce->gen != be.gen) removed.push_back(be.slot);
        }

    std::vector<std::vector<uint8_t>> records;
    ByteWriter ops;
    for (const NetEntity& ce : cur.ents) {
        const NetEntity* be = base ? base->find(ce.slot) : nullptr;
        if (be && be->gen != ce.gen) be = nullptr;
        bool spawn = !be;
        ops.clear();
        uint32_t nOps = 0;
        size_t i = 0, j = 0;
        static const std::vector<std::pair<ComponentId, std::vector<uint8_t>>> kNone;
        const auto& bc = be ? be->comps : kNone;
        while (i < ce.comps.size() || j < bc.size()) {
            bool takeCur = j >= bc.size() || (i < ce.comps.size() && ce.comps[i].first <= bc[j].first);
            bool takeBase = i >= ce.comps.size() || (j < bc.size() && bc[j].first <= ce.comps[i].first);
            if (takeCur && takeBase) {  // in both: the changed fields
                const NetComponent& nc = m_schema->comps[ce.comps[i].first];
                std::vector<uint8_t> mask((nc.fields.size() + 7) / 8, 0);
                bool any = false;
                for (size_t f = 0; f < nc.fields.size(); ++f) {
                    const NetField& nf = nc.fields[f];
                    if (std::memcmp(ce.comps[i].second.data() + nf.wireAt, bc[j].second.data() + nf.wireAt, nf.wire) != 0) {
                        mask[f / 8] |= uint8_t(1u << (f % 8));
                        any = true;
                    }
                }
                if (any) {
                    ops.varU32(ce.comps[i].first);
                    ops.u8(1);
                    ops.bytes(mask.data(), mask.size());
                    for (size_t f = 0; f < nc.fields.size(); ++f)
                        if (mask[f / 8] & (1u << (f % 8))) detail::writeField(ops, nc.fields[f], ce.comps[i].second.data() + nc.fields[f].wireAt);
                    ++nOps;
                }
                ++i, ++j;
            } else if (takeCur) {  // new: every field
                const NetComponent& nc = m_schema->comps[ce.comps[i].first];
                ops.varU32(ce.comps[i].first);
                ops.u8(1);
                std::vector<uint8_t> mask((nc.fields.size() + 7) / 8, 0);
                for (size_t f = 0; f < nc.fields.size(); ++f) mask[f / 8] |= uint8_t(1u << (f % 8));
                ops.bytes(mask.data(), mask.size());
                for (const NetField& nf : nc.fields) detail::writeField(ops, nf, ce.comps[i].second.data() + nf.wireAt);
                ++nOps;
                ++i;
            } else {  // gone
                ops.varU32(bc[j].first);
                ops.u8(0);
                ++nOps;
                ++j;
            }
        }
        if (!spawn && nOps == 0) continue;
        ByteWriter rec;
        rec.varU32(ce.slot);
        rec.u8(spawn ? 1 : 0);
        if (spawn) {
            rec.u16(ce.gen);
            rec.varU32(ce.prefab == kNoPrefab ? 0 : uint32_t(ce.prefab) + 1);
        }
        rec.varU32(nOps);
        rec.bytes(ops.data().data(), ops.size());
        records.push_back(std::move(rec.data()));
    }

    // Pack greedily. Even an empty delta is one part: the client acks it.
    struct Part {
        std::vector<uint16_t> removed;
        std::vector<size_t> records;
        size_t bytes = detail::kHeaderBytes + 6;  // + room for the two counts
        bool empty() const { return removed.empty() && records.empty(); }
    };
    std::vector<Part> parts(1);
    auto room = [&](size_t cost) {
        if (parts.back().bytes + cost > m_config.maxPacketBytes && !parts.back().empty()) parts.emplace_back();
        parts.back().bytes += cost;
    };
    for (uint16_t slot : removed) {
        room(detail::varSize(slot));
        parts.back().removed.push_back(slot);
    }
    for (size_t k = 0; k < records.size(); ++k) {
        room(records[k].size());
        parts.back().records.push_back(k);
    }
    if (parts.size() > 0xFFFF) throw std::length_error("SnapshotServer: a snapshot needs more than 65535 packets");

    std::vector<std::vector<uint8_t>> out;
    out.reserve(parts.size());
    for (size_t k = 0; k < parts.size(); ++k) {
        const Part& p = parts[k];
        ByteWriter w;
        w.u32(cur.tick);
        w.u32(base ? base->tick : 0);
        w.u16(uint16_t(k));
        w.u16(uint16_t(parts.size()));
        w.varU32(uint32_t(p.removed.size()));
        for (uint16_t slot : p.removed) w.varU32(slot);
        w.varU32(uint32_t(p.records.size()));
        for (size_t r : p.records) w.bytes(records[r].data(), records[r].size());
        out.push_back(std::move(w.data()));
    }
    return out;
}

// ── Client ───────────────────────────────────────────────────────────────────

struct SnapshotClient::Pending {
    uint32_t tick = 0, base = 0;
    uint16_t count = 0, have = 0;
    std::vector<std::vector<uint8_t>> parts;
    std::vector<uint8_t> got;
};

SnapshotClient::SnapshotClient(const Registry& registry, ReplicationConfig config)
    : m_schema(std::make_unique<NetSchema>(registry)), m_config(config) {
    if (m_config.history < 2) m_config.history = 2;
}
SnapshotClient::~SnapshotClient() = default;

uint32_t SnapshotClient::ackTick() const { return latestTick(); }
uint32_t SnapshotClient::latestTick() const { return m_history.empty() ? 0 : m_history.back()->tick; }
uint32_t SnapshotClient::oldestTick() const { return m_history.empty() ? 0 : m_history.front()->tick; }

void SnapshotClient::reset() {
    m_history.clear();
    m_pending.clear();
}

bool SnapshotClient::receive(const void* data, size_t size) {
    ByteReader r(data, size);
    uint32_t tick = r.u32(), base = r.u32();
    uint16_t index = r.u16(), count = r.u16();
    if (!r.ok() || tick == 0 || count == 0 || index >= count || base >= tick) return false;

    for (const auto& s : m_history)
        if (s->tick == tick) return true;  // already have it
    if (!m_history.empty() && m_history.size() >= m_config.history && tick < oldestTick()) return true;  // too old to keep

    Pending* p = nullptr;
    for (auto& q : m_pending)
        if (q->tick == tick) p = q.get();
    if (p && (p->base != base || p->count != count)) {  // parts of a different encoding of this tick
        *p = Pending{};
    } else if (!p) {
        if (m_pending.size() >= 8) m_pending.erase(m_pending.begin());  // the oldest started
        m_pending.push_back(std::make_unique<Pending>());
        p = m_pending.back().get();
    }
    if (p->count == 0) {
        p->tick = tick;
        p->base = base;
        p->count = count;
        p->parts.assign(count, {});
        p->got.assign(count, 0);
    }
    if (p->got[index]) return true;
    p->got[index] = 1;
    p->parts[index].assign(r.cursor(), r.cursor() + r.remaining());
    if (++p->have < p->count) return true;

    bool ok = complete(*p, tick);
    std::erase_if(m_pending, [&](const std::unique_ptr<Pending>& q) { return q->tick <= tick; });
    return ok;
}

bool SnapshotClient::complete(Pending& p, uint32_t tick) {
    const Snapshot* base = nullptr;
    if (p.base) {
        for (const auto& s : m_history)
            if (s->tick == p.base) base = s.get();
        if (!base) return true;  // its baseline is gone: this one is lost, the next will come
    }
    std::map<uint16_t, NetEntity> work;
    if (base)
        for (const NetEntity& e : base->ents) work.emplace(e.slot, e);

    // Removals from every part first, then the records.
    std::vector<ByteReader> readers;
    for (const auto& part : p.parts) {
        ByteReader r(part);
        uint32_t n = r.varU32();
        if (!r.ok() || n > kMaxEntityCap) return false;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t slot = r.varU32();
            if (!r.ok() || slot > detail::kMaxSlot || !work.erase(uint16_t(slot))) return false;
        }
        readers.push_back(r);
    }
    const auto& comps = m_schema->comps;
    for (ByteReader& r : readers) {
        uint32_t n = r.varU32();
        if (!r.ok() || n > kMaxEntityCap) return false;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t slot = r.varU32();
            uint8_t spawned = r.u8();
            if (!r.ok() || slot > detail::kMaxSlot || spawned > 1) return false;
            NetEntity* e = nullptr;
            if (spawned) {
                uint16_t gen = r.u16();
                uint32_t prefabPlus = r.varU32();
                if (!r.ok() || gen == 0 || prefabPlus > kNoPrefab) return false;
                NetEntity fresh;
                fresh.slot = uint16_t(slot);
                fresh.gen = gen;
                fresh.prefab = prefabPlus ? PrefabId(prefabPlus - 1) : kNoPrefab;
                e = &(work[uint16_t(slot)] = std::move(fresh));
            } else {
                auto it = work.find(uint16_t(slot));
                if (it == work.end()) return false;
                e = &it->second;
            }
            uint32_t nOps = r.varU32();
            if (!r.ok() || nOps > 2 * comps.size()) return false;
            for (uint32_t k = 0; k < nOps; ++k) {
                uint32_t c = r.varU32();
                uint8_t op = r.u8();
                if (!r.ok() || c >= comps.size() || !comps[c].replicated || op > 1) return false;
                auto it = std::find_if(e->comps.begin(), e->comps.end(), [&](const auto& x) { return x.first == c; });
                if (op == 0) {
                    if (it == e->comps.end()) return false;
                    e->comps.erase(it);
                    continue;
                }
                const NetComponent& nc = comps[c];
                if (it == e->comps.end()) {
                    auto pos = std::lower_bound(e->comps.begin(), e->comps.end(), ComponentId(c),
                                                [](const auto& x, ComponentId id) { return x.first < id; });
                    it = e->comps.insert(pos, {ComponentId(c), nc.defaults});
                }
                size_t maskBytes = (nc.fields.size() + 7) / 8;
                if (r.remaining() < maskBytes) return false;
                std::vector<uint8_t> mask(r.cursor(), r.cursor() + maskBytes);
                r.skip(maskBytes);
                for (size_t f = nc.fields.size(); f < maskBytes * 8; ++f)
                    if (mask[f / 8] & (1u << (f % 8))) return false;  // bits past the last field
                for (size_t f = 0; f < nc.fields.size(); ++f)
                    if ((mask[f / 8] & (1u << (f % 8))) && !detail::readField(r, nc.fields[f], it->second.data() + nc.fields[f].wireAt))
                        return false;
            }
        }
        if (!r.ok() || !r.atEnd()) return false;
    }

    auto snap = std::make_unique<Snapshot>();
    snap->tick = tick;
    snap->ents.reserve(work.size());
    for (auto& [slot, e] : work) snap->ents.push_back(std::move(e));
    auto pos = std::lower_bound(m_history.begin(), m_history.end(), tick,
                                [](const std::unique_ptr<Snapshot>& s, uint32_t t) { return s->tick < t; });
    m_history.insert(pos, std::move(snap));
    while (m_history.size() > m_config.history) m_history.erase(m_history.begin());
    return true;
}

void SnapshotClient::apply(World& world, double tick) const {
    if (m_history.empty()) return;
    // A: the newest held at or before `tick` (else the oldest); B: the one after it.
    size_t ai = 0;
    for (size_t i = 0; i < m_history.size(); ++i)
        if (double(m_history[i]->tick) <= tick) ai = i;
    const Snapshot& a = *m_history[ai];
    const Snapshot* b = ai + 1 < m_history.size() && tick > double(a.tick) ? m_history[ai + 1].get() : nullptr;
    float t = b ? float((tick - double(a.tick)) / double(b->tick - a.tick)) : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);

    for (uint32_t slot = 0; slot < world.capacity(); ++slot) {
        Entity e = world.entityAt(uint16_t(slot));
        if (e == kNullEntity) continue;
        const NetEntity* ne = a.find(uint16_t(slot));
        if (!ne || ne->gen != entityGeneration(e)) world.destroy(e);
    }
    for (const NetEntity& ne : a.ents) {
        if (ne.slot >= world.capacity()) {
            ENG_LOG("[replication] slot " << ne.slot << " is past this world's capacity of " << world.capacity());
            continue;
        }
        Entity e = makeEntity(ne.slot, ne.gen);
        if (!world.alive(e)) {
            PrefabId prefab = ne.prefab;
            if (prefab != kNoPrefab && prefab >= world.prefabDefinitions()) {
                ENG_LOG("[replication] unknown prefab " << prefab << " (different content than the server?)");
                prefab = kNoPrefab;
            }
            world.spawnAt(e, prefab);
        }
        const NetEntity* nb = b ? b->find(ne.slot) : nullptr;
        if (nb && nb->gen != ne.gen) nb = nullptr;
        for (ComponentId c : m_schema->replicated) {
            const std::vector<uint8_t>* wa = ne.comp(c);
            if (!wa) {
                if (world.has(e, c)) world.remove(e, c);
                continue;
            }
            void* bytes = world.get(e, c);
            if (!bytes) bytes = world.add(e, c);
            const std::vector<uint8_t>* wb = nb ? nb->comp(c) : nullptr;
            NetSchema::fromWire(m_schema->comps[c], wa->data(), wb ? wb->data() : nullptr, t, static_cast<std::byte*>(bytes));
        }
    }
}

}  // namespace eng::ecs
