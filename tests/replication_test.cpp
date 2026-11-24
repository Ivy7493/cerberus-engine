// eng::ecs replication tests: a randomized server world mirrored to a
// client through a lossy, delaying, reordering, duplicating channel (acks
// lossy too), compared field by field; packet sizes and splitting; empty
// deltas; slot reuse with new generations; interpolation; fuzzed packets;
// and a real UDP loopback run under LagTransport. Run with `zig build test`.
#include "eng/ecs/Replication.h"
#include "eng/net/LagTransport.h"
#include "eng/net/UdpTransport.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <random>
#include <thread>

using namespace eng;
using namespace eng::ecs;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

struct Pos {
    glm::vec2 p{0.0f};
};
struct Health {
    int32_t hp = 0;
    float regen = 0.0f;
    uint8_t armor = 0;
    int16_t temp = 0;
};
struct Brain {  // server-only: never on the wire
    uint32_t state = 0;
    float timer = 0.0f;
};
struct Boss {};  // a tag
struct Target {
    Entity who = kNullEntity;
};

constexpr float kStep = 1.0f / 64.0f;

static void registerAll(Registry& r) {
    r.add<Pos>("Pos", {field("p", &Pos::p, {.net = NetEncoding::Fixed16, .scale = kStep, .interpolated = true})});
    r.add<Health>("Health", {field("hp", &Health::hp, {.net = NetEncoding::Varint}), field("regen", &Health::regen),
                             field("armor", &Health::armor, {.net = NetEncoding::Varint}),
                             field("temp", &Health::temp, {.net = NetEncoding::Varint})},
                  Health{5, 0.0f, 0, 0});
    r.add<Brain>("Brain", {field("state", &Brain::state, {.replicated = false}), field("timer", &Brain::timer, {.replicated = false})});
    r.add<Boss>("Boss", {});
    r.add<Target>("Target", {field("who", &Target::who)});
    r.finalize();
}

static void definePrefabs(World& w) {
    PrefabId hound = w.definePrefab("hound", 200);
    w.setPrefabComponent(hound, Health{5, 0.5f, 1, 0});
    w.setPrefabComponent(hound, Pos{});
    w.setPrefabComponent(hound, Brain{7, 0.0f});
    PrefabId bat = w.definePrefab("bat", 60000);
    w.setPrefabComponent(bat, Pos{});
}

static float q(float v) { return float(std::lround(std::clamp(double(v) / kStep, -32768.0, 32767.0))) * kStep; }

// Whether `client` mirrors `server` (replicated data, quantized). Logs the first difference.
static bool same(World& server, World& client) {
    auto bad = [](const char* what, uint32_t slot) {
        std::fprintf(stderr, "  (mismatch at slot %u: %s)\n", slot, what);
        return false;
    };
    for (uint32_t slot = 0; slot < server.capacity(); ++slot) {
        Entity e = server.entityAt(uint16_t(slot));
        if (client.entityAt(uint16_t(slot)) != e) return bad("entity", slot);
        if (!e) continue;
        if (client.prefabOf(e) != server.prefabOf(e)) return bad("prefab", slot);
        if (server.has<Pos>(e) != client.has<Pos>(e)) return bad("Pos presence", slot);
        if (server.has<Pos>(e)) {
            glm::vec2 s = server.get<Pos>(e)->p, c = client.get<Pos>(e)->p;
            if (c.x != q(s.x) || c.y != q(s.y)) return bad("Pos value", slot);
        }
        if (server.has<Health>(e) != client.has<Health>(e)) return bad("Health presence", slot);
        if (server.has<Health>(e)) {
            const Health &s = *server.get<Health>(e), &c = *client.get<Health>(e);
            if (s.hp != c.hp || s.regen != c.regen || s.armor != c.armor || s.temp != c.temp) return bad("Health value", slot);
        }
        if (server.has<Boss>(e) != client.has<Boss>(e)) return bad("Boss", slot);
        if (server.has<Target>(e) != client.has<Target>(e)) return bad("Target presence", slot);
        if (server.has<Target>(e) && server.get<Target>(e)->who != client.get<Target>(e)->who) return bad("Target value", slot);
        // Brain never replicates: the mirror keeps the prefab's.
        if (client.has<Brain>(e) && client.get<Brain>(e)->state != 7) return bad("Brain leaked", slot);
    }
    return true;
}

// One random tick of changes to the server world.
static void mutate(World& w, std::mt19937& rng) {
    auto roll = [&](int n) { return int(rng() % uint32_t(n)); };
    auto randomLive = [&]() -> Entity {
        for (int tries = 0; tries < 8; ++tries) {
            Entity e = w.entityAt(uint16_t(roll(int(w.capacity()))));
            if (e) return e;
        }
        return kNullEntity;
    };
    int ops = roll(6);
    for (int i = 0; i < ops; ++i) {
        switch (roll(9)) {
            case 0: w.spawn(PrefabId(roll(2))); break;
            case 1: w.spawn(); break;
            case 2: if (Entity e = randomLive()) w.destroy(e); break;
            case 3: if (Entity e = randomLive(); e && w.has<Pos>(e)) w.get<Pos>(e)->p += glm::vec2(roll(200) - 100, roll(200) - 100) * 0.0137f; break;
            case 4:
                if (Entity e = randomLive()) {
                    Health& h = w.has<Health>(e) ? *w.get<Health>(e) : w.add<Health>(e);
                    h.hp = roll(100000) - 50000;
                    h.temp = int16_t(roll(65536) - 32768);
                    h.armor = uint8_t(roll(256));
                    h.regen = float(roll(1000)) * 0.01f;
                }
                break;
            case 5: if (Entity e = randomLive()) { if (w.has<Boss>(e)) w.remove<Boss>(e); else w.add<Boss>(e); } break;
            case 6: if (Entity e = randomLive()) w.add<Target>(e, Target{randomLive()}); break;
            case 7: if (Entity e = randomLive()) w.remove<Health>(e); break;
            case 8: if (Entity e = randomLive(); e && w.has<Brain>(e)) w.get<Brain>(e)->state = uint32_t(rng()); break;
        }
    }
}

int main() {
    Registry reg;
    registerAll(reg);

    // ── A random world through a hostile channel ──
    {
        World server(reg, {.maxEntities = 300}), client(reg, {.maxEntities = 300});
        definePrefabs(server);
        definePrefabs(client);
        SnapshotServer ss(reg);
        SnapshotClient sc(reg);
        std::mt19937 rng(42);
        struct InFlight {
            uint32_t due;
            std::vector<uint8_t> bytes;
        };
        std::deque<InFlight> toClient;
        std::deque<std::pair<uint32_t, uint32_t>> acks;  // (due, tick)
        uint32_t serverAck = 0, matches = 0, mismatches = 0, maxPacket = 0;
        bool allAccepted = true;
        auto run = [&](uint32_t from, uint32_t to, bool hostile) {
            for (uint32_t tick = from; tick <= to; ++tick) {
                if (hostile || tick % 3 == 0) mutate(server, rng);
                ss.capture(server, tick);
                for (auto& pkt : ss.encode(serverAck)) {
                    maxPacket = std::max(maxPacket, uint32_t(pkt.size()));
                    if (hostile && rng() % 100 < 20) continue;  // lost
                    uint32_t delay = hostile ? uint32_t(rng() % 4) : 0;
                    toClient.push_back({tick + delay, pkt});
                    if (hostile && rng() % 100 < 5) toClient.push_back({tick + uint32_t(rng() % 4), pkt});  // duplicated
                }
                std::stable_sort(toClient.begin(), toClient.end(), [](auto& a, auto& b) { return a.due < b.due; });
                if (hostile) std::shuffle(toClient.begin(), toClient.begin() + std::min<size_t>(toClient.size(), 3), rng);
                while (!toClient.empty() && toClient.front().due <= tick) {
                    allAccepted = sc.receive(toClient.front().bytes.data(), toClient.front().bytes.size()) && allAccepted;
                    toClient.pop_front();
                }
                if (!hostile || rng() % 100 >= 20) acks.push_back({tick + (hostile ? uint32_t(rng() % 4) : 0u), sc.ackTick()});
                while (!acks.empty() && acks.front().first <= tick) {
                    serverAck = std::max(serverAck, acks.front().second);
                    acks.pop_front();
                }
                sc.apply(client, sc.latestTick());
                if (sc.latestTick() == tick) ++(same(server, client) ? matches : mismatches);  // current: must be exact
            }
        };
        run(1, 600, true);
        CHECK(allAccepted);
        CHECK(mismatches == 0);
        CHECK(matches > 50);  // ticks that arrived whole within the same tick (delays are 0-3 ticks)
        std::fprintf(stderr, "  hostile channel: %u ticks current on arrival, all exact (%u wrong), %zu entities, max packet %u bytes\n",
                     matches, mismatches, size_t(server.size()), maxPacket);
        CHECK(maxPacket <= 1200);
        run(601, 610, false);  // a clean channel: converges at once
        CHECK(sc.latestTick() == 610);
        CHECK(same(server, client));

        // Nothing changed: an empty delta is just the header and two counts.
        ss.capture(server, 611);
        auto empty = ss.encode(610);
        CHECK(empty.size() == 1 && empty[0].size() == 14);
        CHECK(sc.receive(empty[0].data(), empty[0].size()) && sc.ackTick() == 611);
        // Sending again before the next capture: the client has tick 611, so
        // there's nothing to send (not a base == tick delta it would reject).
        CHECK(ss.encode(611).empty());
    }

    // ── A big full snapshot splits; every part is needed ──
    {
        World server(reg, {.maxEntities = 4000}), client(reg, {.maxEntities = 4000});
        definePrefabs(server);
        definePrefabs(client);
        for (int i = 0; i < 3000; ++i) {
            Entity e = server.spawn(1);
            server.get<Pos>(e)->p = glm::vec2(float(i % 97), float(i % 89)) * 0.25f;
        }
        SnapshotServer ss(reg);
        SnapshotClient sc(reg);
        ss.capture(server, 1);
        auto parts = ss.encode(0);
        CHECK(parts.size() > 10);
        bool small = true;
        for (auto& p : parts) small = small && p.size() <= 1200;
        CHECK(small);
        for (size_t i = 1; i < parts.size(); ++i) CHECK(sc.receive(parts[i].data(), parts[i].size()));
        CHECK(sc.latestTick() == 0);  // part 0 still missing
        CHECK(sc.receive(parts[0].data(), parts[0].size()) && sc.latestTick() == 1);
        sc.apply(client, 1);
        CHECK(same(server, client) && client.size() == 3000);

        // Destroying most of them: removals and records spread over parts too.
        for (uint32_t s = 0; s < 2500; ++s) server.destroy(server.entityAt(uint16_t(s)));
        for (int i = 0; i < 100; ++i) server.spawn(0);
        ss.capture(server, 2);
        auto delta = ss.encode(1);
        for (auto it = delta.rbegin(); it != delta.rend(); ++it) CHECK(sc.receive(it->data(), it->size()));  // backwards
        sc.apply(client, 2);
        CHECK(sc.latestTick() == 2 && same(server, client));
    }

    // ── A slot reused within one tick gets a new generation on the client too ──
    {
        World server(reg, {.maxEntities = 2}), client(reg, {.maxEntities = 2});
        definePrefabs(server);
        definePrefabs(client);
        SnapshotServer ss(reg);
        SnapshotClient sc(reg);
        Entity a = server.spawn(0);
        server.spawn(1);
        ss.capture(server, 1);
        for (auto& p : ss.encode(0)) sc.receive(p.data(), p.size());
        sc.apply(client, 1);
        server.get<Health>(a)->hp = 99;
        client.get<Brain>(a)->state = 7;
        server.destroy(a);
        Entity c = server.spawn(0);  // the only free slot: a's, new generation
        CHECK(entitySlot(c) == entitySlot(a) && c != a);
        ss.capture(server, 2);
        for (auto& p : ss.encode(1)) sc.receive(p.data(), p.size());
        sc.apply(client, 2);
        CHECK(same(server, client) && !client.alive(a) && client.alive(c) && client.get<Health>(c)->hp == 5);
    }

    // ── Interpolation ──
    {
        World server(reg, {.maxEntities = 8}), client(reg, {.maxEntities = 8});
        definePrefabs(server);
        definePrefabs(client);
        SnapshotServer ss(reg);
        SnapshotClient sc(reg);
        Entity e = server.spawn(0);
        server.get<Pos>(e)->p = {0.0f, 2.0f};
        ss.capture(server, 10);
        for (auto& p : ss.encode(0)) sc.receive(p.data(), p.size());
        server.get<Pos>(e)->p = {8.0f, 2.0f};
        server.get<Health>(e)->hp = 3;
        ss.capture(server, 12);
        for (auto& p : ss.encode(10)) sc.receive(p.data(), p.size());
        sc.apply(client, 11.0);  // halfway
        CHECK(client.get<Pos>(e)->p == glm::vec2(4.0f, 2.0f) && client.get<Health>(e)->hp == 5);  // hp: the earlier one
        sc.apply(client, 11.5);
        CHECK(client.get<Pos>(e)->p.x == 6.0f);
        sc.apply(client, 99.0);  // past the newest: clamps
        CHECK(client.get<Pos>(e)->p.x == 8.0f && client.get<Health>(e)->hp == 3);
        sc.apply(client, 1.0);  // before the oldest
        CHECK(client.get<Pos>(e)->p.x == 0.0f);
        CHECK(sc.oldestTick() == 10 && sc.latestTick() == 12);
    }

    // ── Fuzz: corrupted packets never crash, and the client still works after ──
    {
        World server(reg, {.maxEntities = 64}), client(reg, {.maxEntities = 64});
        definePrefabs(server);
        definePrefabs(client);
        std::mt19937 rng(7);
        for (int i = 0; i < 40; ++i) mutate(server, rng);
        SnapshotServer ss(reg);
        ss.capture(server, 1);
        auto full = ss.encode(0);
        mutate(server, rng);
        ss.capture(server, 2);
        auto delta = ss.encode(1);
        int rejected = 0;
        for (int round = 0; round < 3000; ++round) {
            SnapshotClient sc(reg);
            for (auto& p : full) sc.receive(p.data(), p.size());
            std::vector<uint8_t> pkt = (round & 1 ? delta : full)[rng() % (round & 1 ? delta : full).size()];
            int edits = 1 + int(rng() % 4);
            for (int k = 0; k < edits && !pkt.empty(); ++k) {
                switch (rng() % 3) {
                    case 0: pkt[rng() % pkt.size()] = uint8_t(rng()); break;
                    case 1: pkt.resize(rng() % pkt.size()); break;
                    case 2: pkt.insert(pkt.begin() + long(rng() % pkt.size()), uint8_t(rng())); break;
                }
            }
            if (!sc.receive(pkt.data(), pkt.size())) ++rejected;
            World scratch(reg, {.maxEntities = 64});
            definePrefabs(scratch);
            sc.apply(scratch, 2.0);  // whatever got in must still apply safely
        }
        CHECK(rejected > 1000);
        SnapshotClient sc(reg);
        CHECK(!sc.receive(nullptr, 0));
        for (auto& p : full) CHECK(sc.receive(p.data(), p.size()));
        for (auto& p : delta) CHECK(sc.receive(p.data(), p.size()));
        sc.apply(client, 2);
        CHECK(same(server, client));
    }

    // ── Real UDP on loopback, under LagTransport (80ms RTT, 20ms jitter, 10% loss) ──
    {
        World server(reg, {.maxEntities = 128}), client(reg, {.maxEntities = 128});
        definePrefabs(server);
        definePrefabs(client);
        SnapshotServer ss(reg);
        SnapshotClient sc(reg);
        const uint16_t port = 47000 + uint16_t(std::chrono::steady_clock::now().time_since_epoch().count() % 2000);
        LagTransport host(std::make_unique<UdpTransport>(port), 80, 20, 10.0f);
        LagTransport guest(std::make_unique<UdpTransport>(0), 80, 20, 10.0f);
        const PeerId hostPeer = UdpTransport::resolve("127.0.0.1", port);
        PeerId guestPeer = 0;
        uint32_t serverAck = 0;
        std::mt19937 rng(3);
        std::vector<TransportEvent> events;
        auto ackMsg = [](uint32_t tick) {
            std::vector<uint8_t> m(4);
            std::memcpy(m.data(), &tick, 4);
            return m;
        };
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        uint32_t tick = 0;
        bool converged = false;
        while (std::chrono::steady_clock::now() < deadline) {
            ++tick;
            if (tick < 90) mutate(server, rng);  // ~1.5 s of changes, then still
            ss.capture(server, tick);
            if (guestPeer)
                for (auto& p : ss.encode(serverAck)) host.send(guestPeer, p, Delivery::Unreliable);
            guest.send(hostPeer, ackMsg(sc.ackTick()), Delivery::Unreliable);  // also the client's hello

            events.clear();
            host.poll(events);
            for (auto& ev : events)
                if (ev.type == TransportEvent::Type::Data && ev.data.size() == 4) {
                    guestPeer = ev.peer;
                    uint32_t a;
                    std::memcpy(&a, ev.data.data(), 4);
                    serverAck = std::max(serverAck, a);
                }
            events.clear();
            guest.poll(events);
            for (auto& ev : events)
                if (ev.type == TransportEvent::Type::Data) CHECK(sc.receive(ev.data.data(), ev.data.size()));
            sc.apply(client, sc.latestTick());
            if (sc.latestTick() > 90 && same(server, client)) {  // the server has been still since tick 89
                converged = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        CHECK(converged);
        std::fprintf(stderr, "  UDP + LagTransport: converged after %u ticks, %zu entities\n", tick, size_t(server.size()));
    }

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("replication_test: all checks passed\n");
    return 0;
}
