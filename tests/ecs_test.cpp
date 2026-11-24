// eng::ecs tests: the registry (ids, schemas, hash), entities (caps,
// generations, slot reuse), prefabs, components, iteration, deferred
// commands and determinism. Run with `zig build test`.
#include "eng/ecs/World.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

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
struct Vel {
    glm::vec2 v{0.0f};
};
struct Health {
    int32_t hp = 0;
    float regen = 0.0f;
};

static void registerAll(Registry& r, bool reversed = false) {
    auto pos = [&] { r.add<Pos>("Pos", {field("p", &Pos::p, {.net = NetEncoding::Fixed16, .interpolated = true})}); };
    auto vel = [&] { r.add<Vel>("Vel", {field("v", &Vel::v)}); };
    auto hp = [&] { r.add<Health>("Health", {field("hp", &Health::hp, {.net = NetEncoding::Varint}), field("regen", &Health::regen)}, Health{5, 0.0f}); };
    auto frozen = [&] {
        r.addSchema("Frozen", {{"time", FieldType::F32}, {"stacks", FieldType::U8}, {"by", FieldType::Entity}});
    };
    if (reversed) frozen(), hp(), vel(), pos();
    else pos(), vel(), hp(), frozen();
    r.finalize();
}

static void testRegistry() {
    Registry r;
    registerAll(r);
    // Ids sorted by name.
    CHECK(r.find("Frozen") == 0 && r.find("Health") == 1 && r.find("Pos") == 2 && r.find("Vel") == 3);
    CHECK(r.id<Pos>() == 2 && r.id<Health>() == 1);
    CHECK(r.find("Nope") == kNoComponent);
    // Schema layout: f32 @0, u8 @4, entity aligned to 4 → @8, size 12.
    const ComponentInfo& fz = r.info(r.find("Frozen"));
    CHECK(fz.fromSchema && fz.size == 12 && fz.field("stacks")->offset == 4 && fz.field("by")->offset == 8);
    CHECK(r.info(r.id<Health>()).field("regen")->offset == 4);
    // The hash ignores registration order but not content.
    Registry r2;
    registerAll(r2, /*reversed=*/true);
    CHECK(r.schemaHash() == r2.schemaHash());
    Registry r3;
    r3.add<Pos>("Pos", {field("p", &Pos::p)});  // different net options
    r3.add<Vel>("Vel", {field("v", &Vel::v)});
    r3.add<Health>("Health", {field("hp", &Health::hp, {.net = NetEncoding::Varint}), field("regen", &Health::regen)});
    r3.addSchema("Frozen", {{"time", FieldType::F32}, {"stacks", FieldType::U8}, {"by", FieldType::Entity}});
    r3.finalize();
    CHECK(r.schemaHash() != r3.schemaHash());
    bool threw = false;
    try {
        Registry dup;
        dup.add<Pos>("Pos", {});
        dup.add<Vel>("Pos", {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

static void testEntities(const Registry& r) {
    World w(r, {.maxEntities = 3});
    Entity a = w.spawn(), b = w.spawn(), c = w.spawn();
    CHECK(a != kNullEntity && b != kNullEntity && c != kNullEntity);
    CHECK(w.size() == 3 && w.spawn() == kNullEntity);  // at the cap
    w.destroy(b);
    CHECK(!w.alive(b) && w.alive(a) && w.size() == 2);
    Entity d = w.spawn();  // b's slot is the only free one
    CHECK(entitySlot(d) == entitySlot(b) && entityGeneration(d) == entityGeneration(b) + 1);
    CHECK(!w.alive(b) && w.alive(d));  // the old handle stays stale
    w.destroy(b);                      // stale: no-op
    CHECK(w.alive(d) && w.size() == 3);
    CHECK(!w.alive(kNullEntity) && w.entityAt(entitySlot(d)) == d);

    // Freed slots are reused first-in, first-out.
    World f(r, {.maxEntities = 4});
    Entity e[4];
    for (Entity& x : e) x = f.spawn();
    f.destroy(e[2]);
    f.destroy(e[0]);
    CHECK(entitySlot(f.spawn()) == entitySlot(e[2]) && entitySlot(f.spawn()) == entitySlot(e[0]));
}

static void testPrefabsAndComponents(const Registry& r) {
    World w(r, {.maxEntities = 64, .defaultPrefabMax = 8});
    PrefabId hound = w.definePrefab("hound", 2);
    w.setPrefabComponent(hound, Pos{{1.0f, 2.0f}});
    w.setPrefabComponent(hound, r.id<Health>());  // its defaults: hp 5
    CHECK(w.findPrefab("hound") == hound && w.findPrefab("cat") == kNoPrefab);
    Entity h1 = w.spawn(hound), h2 = w.spawn(hound);
    CHECK(w.spawn(hound) == kNullEntity && w.prefabCount(hound) == 2);  // prefab cap
    CHECK(w.get<Pos>(h1)->p == glm::vec2(1.0f, 2.0f) && w.get<Health>(h2)->hp == 5);
    CHECK(w.prefabOf(h1) == hound && !w.has<Vel>(h1));
    w.get<Health>(h1)->hp = 1;
    CHECK(w.get<Health>(h2)->hp == 5);  // instances don't share
    w.destroy(h1);
    CHECK(w.prefabCount(hound) == 1 && w.spawn(hound) != kNullEntity);

    // add / overwrite / remove, and swap-remove keeping the rest intact.
    std::vector<Entity> es;
    for (int i = 0; i < 5; ++i) {
        Entity e = w.spawn();
        w.add(e, Vel{{float(i), 0.0f}});
        es.push_back(e);
    }
    w.add(es[2], Vel{{42.0f, 0.0f}});  // overwrite
    CHECK(w.get<Vel>(es[2])->v.x == 42.0f && w.count(r.id<Vel>()) == 5);
    w.remove<Vel>(es[1]);
    CHECK(!w.has<Vel>(es[1]) && w.count(r.id<Vel>()) == 4);
    CHECK(w.get<Vel>(es[0])->v.x == 0.0f && w.get<Vel>(es[2])->v.x == 42.0f && w.get<Vel>(es[3])->v.x == 3.0f &&
          w.get<Vel>(es[4])->v.x == 4.0f);
    w.destroy(es[4]);  // its components go with it
    CHECK(w.count(r.id<Vel>()) == 3 && w.get<Vel>(es[4]) == nullptr);

    // Schema components by id.
    ComponentId frozen = r.find("Frozen");
    auto* bytes = static_cast<unsigned char*>(w.add(es[0], frozen));
    CHECK(bytes && bytes[4] == 0);  // zero-initialized
    bytes[4] = 3;
    CHECK(static_cast<unsigned char*>(w.get(es[0], frozen))[4] == 3 && w.has(es[0], frozen));
}

static void testIteration(const Registry& r) {
    World w(r, {.maxEntities = 32});
    Entity a = w.spawn(), b = w.spawn(), c = w.spawn();
    w.add(a, Pos{{0, 0}});
    w.add(a, Vel{{1, 0}});
    w.add(b, Pos{{0, 0}});
    w.add(c, Pos{{0, 0}});
    w.add(c, Vel{{0, 2}});
    int n = 0;
    w.each<Pos, Vel>([&](Entity, Pos& p, Vel& v) {
        p.p += v.v;
        ++n;
    });
    CHECK(n == 2 && w.get<Pos>(a)->p == glm::vec2(1, 0) && w.get<Pos>(b)->p == glm::vec2(0, 0) && w.get<Pos>(c)->p == glm::vec2(0, 2));
    ComponentId ids[] = {r.id<Pos>()};
    n = 0;
    w.query(ids, [&](Entity) { ++n; });
    CHECK(n == 3);

    // Structural changes while iterating are refused; commands defer them.
    bool threw = false;
    try {
        w.each<Pos>([&](Entity, Pos&) { w.spawn(); });
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw && !w.iterating());
    Entity spawned = kNullEntity;
    w.each<Pos, Vel>([&](Entity e, Pos&, Vel&) {
        if (e == a) {
            spawned = w.commands().spawn();
            w.commands().add(spawned, Vel{{9, 9}});
            w.commands().destroy(c);
        }
    });
    CHECK(spawned != kNullEntity && w.alive(spawned) && !w.has<Vel>(spawned) && w.alive(c));  // nothing applied yet
    w.flush();
    CHECK(w.get<Vel>(spawned)->v == glm::vec2(9, 9) && !w.alive(c) && w.commands().empty());
}

// Two worlds fed the same calls hand out the same handles and iterate in the
// same order (what lets the server and replays agree).
static std::vector<Entity> script(World& w) {
    std::vector<Entity> log, live;
    PrefabId p = w.definePrefab("thing", 50);
    w.setPrefabComponent(p, Pos{});
    for (int step = 0; step < 300; ++step) {
        if (step % 3 != 2 || live.empty()) {
            Entity e = w.spawn(p);
            if (e) {
                if (step % 2) w.add(e, Vel{{float(step), 0}});
                live.push_back(e);
            }
        } else {
            size_t k = size_t(step * 7) % live.size();
            w.destroy(live[k]);
            live.erase(live.begin() + long(k));
        }
    }
    w.each<Pos, Vel>([&](Entity e, Pos&, Vel&) { log.push_back(e); });
    for (Entity e : live) log.push_back(e);
    return log;
}

static void testDeterminism(const Registry& r) {
    World a(r, {.maxEntities = 64}), b(r, {.maxEntities = 64});
    CHECK(script(a) == script(b));
}

int main() {
    testRegistry();
    Registry r;
    registerAll(r);
    testEntities(r);
    testPrefabsAndComponents(r);
    testIteration(r);
    testDeterminism(r);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ecs_test: all checks passed\n");
    return 0;
}
