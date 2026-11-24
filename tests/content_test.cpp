// eng::ecs::Content tests: mod discovery and manifests, load order and
// dependencies, failing packs, components from packs, prefab replace/patch
// merging, PrefabId order, init, and the join manifest (round trip + diff).
// Run with `zig build test`.
#include "eng/ecs/Content.h"

#include <lua/lauxlib.h>
#include <lua/lua.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
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
};

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static bool hasError(const Content& c, const std::string& needle) {
    for (const auto& e : c.errors())
        if (e.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  (no error mentioning '%s')\n", needle.c_str());
    return false;
}

static bool noError(const Content& c, const std::string& needle) {
    for (const auto& e : c.errors())
        if (e.find(needle) != std::string::npos) return false;
    return true;
}

static std::string eval(Scripts& s, ScriptEnv env, const std::string& code) {
    lua_State* L = s.L();
    if (luaL_loadbufferx(L, code.data(), code.size(), "=test", "t") != LUA_OK) {
        lua_pop(L, 1);
        return "";
    }
    s.pushEnv(env);
    lua_setupvalue(L, -2, 1);
    if (!s.call(0, 1)) return "";
    std::string out = lua_isnil(L, -1) ? "" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() /
                         ("eng_content_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(tmp);
    const fs::path base = tmp / "base", mods = tmp / "mods";

    write(base / "components.lua", "ecs.component('Frozen', { {'time', 'f32'}, {'stacks', 'u8'} })");
    write(base / "prefabs" / "hound.lua", "return { max = 2, Health = { hp = 3, regen = 0.5 }, Pos = { p = {1, 2} } }");
    write(base / "prefabs" / "enemies" / "bat.lua", "return { Health = { hp = 1 }, Pos = true }");
    write(base / "prefabs" / "broken.lua", "return { Healht = { hp = 1 } }");
    write(base / "init.lua", "events.on('ping', function() pinged = (pinged or 0) + 1 end)");

    write(mods / "cool" / "mod.lua", "return { id = 'coolmod', version = '1.0', name = 'Cool Mod' }");
    write(mods / "cool" / "components.lua", "ecs.component('Shiny', { glow = 'f32' })");
    write(mods / "cool" / "prefabs" / "hound.lua", "return { patch = true, Health = { hp = 9 }, Frozen = { stacks = 2 } }");
    write(mods / "cool" / "prefabs" / "wolf.lua", "return { Health = true, Shiny = { glow = 1.5 } }");
    write(mods / "cool" / "prefabs" / "ghost.lua", "return { patch = true, Health = { hp = 1 } }");
    write(mods / "replacer" / "mod.lua", "return { id = 'replacer', version = '2', requires = {'coolmod'} }");
    write(mods / "replacer" / "prefabs" / "enemies" / "bat.lua", "return { Pos = { p = {5, 5} } }");
    write(mods / "replacer" / "prefabs" / "hound.lua", "return { patch = true, max = 4, Frozen = false }");
    write(mods / "needy" / "mod.lua", "return { id = 'needy', version = '1', requires = {'nowhere'} }");
    write(mods / "broken" / "mod.lua", "return { id = 'broken', version = '1' }");
    write(mods / "broken" / "components.lua", "error('components exploded')");
    write(mods / "broken" / "prefabs" / "imp.lua", "return { Health = true }");
    write(mods / "badid" / "mod.lua", "return { id = 'Bad Id', version = '1' }");
    write(mods / "twin1" / "mod.lua", "return { id = 'twin', version = '1' }");
    write(mods / "twin2" / "mod.lua", "return { id = 'twin', version = '2' }");
    write(mods / "looper" / "mod.lua", "while true do end");
    write(mods / "sneaky" / "mod.lua", "print('hi') return { id = 'sneaky', version = '1' }");
    write(mods / "nomanifest" / "init.lua", "return 1");

    Scripts scripts({.memoryBytes = 16u << 20, .instructionsPerCall = 1'000'000});
    Registry reg;
    LuaEcs luaEcs(scripts, reg);
    reg.add<Pos>("Pos", {field("p", &Pos::p)});
    reg.add<Health>("Health", {field("hp", &Health::hp), field("regen", &Health::regen)}, Health{10, 0.0f});

    Content content(scripts, reg, luaEcs);
    content.setBase(base, "1.0.3");

    // ── Discovery ──
    std::vector<ModInfo> available = content.discover(mods);
    std::string ids;
    for (const auto& m : available) ids += m.id + " ";
    CHECK(ids == "broken coolmod needy replacer twin ");
    CHECK(available[1].name == "Cool Mod" && available[3].dependencies.size() == 1);
    CHECK(hasError(content, "Bad Id"));
    CHECK(hasError(content, "'twin' is in both twin1 and twin2"));
    CHECK(hasError(content, "looper/mod.lua"));
    CHECK(hasError(content, "sneaky/mod.lua"));  // no globals in a manifest, not even print

    // ── Load order ──
    CHECK(!content.enable(available, {"replacer", "coolmod", "replacer", "needy", "broken", "missing"}));
    CHECK(hasError(content, "'replacer' needs coolmod enabled before it"));
    CHECK(noError(content, "'replacer' is in the list twice"));  // the first was skipped, so the second is fine
    CHECK(hasError(content, "'needy' needs nowhere"));
    CHECK(hasError(content, "'missing' isn't installed"));
    CHECK(content.env("coolmod") != kNoScriptEnv && content.env("replacer") != kNoScriptEnv);
    CHECK(content.env("needy") == kNoScriptEnv);
    {
        ContentManifest m = content.manifest();
        std::string order;
        for (const auto& p : m.packs) order += p.id + " ";
        CHECK(order == "base coolmod replacer broken ");
    }

    // ── Components ──
    content.declareComponents();
    CHECK(content.failed("broken") && !content.failed("coolmod"));
    CHECK(hasError(content, "components exploded"));
    reg.finalize();
    CHECK(reg.find("Frozen") != kNoComponent && reg.find("Shiny") != kNoComponent);

    // ── Prefabs ──
    World world(reg, {.maxEntities = 64});
    luaEcs.attach(&world);
    content.loadPrefabs(world);
    CHECK(hasError(content, "base: prefabs/broken") && hasError(content, "Healht"));
    CHECK(hasError(content, "coolmod: prefabs/ghost: patches a prefab no earlier pack defines"));
    CHECK(world.findPrefab("broken") == kNoPrefab && world.findPrefab("ghost") == kNoPrefab);
    CHECK(world.findPrefab("imp") == kNoPrefab);  // its pack failed
    PrefabId bat = world.findPrefab("enemies/bat"), hound = world.findPrefab("hound"), wolf = world.findPrefab("wolf");
    CHECK(bat == 0 && hound == 1 && wolf == 2);  // sorted by name

    // hound: base, patched by coolmod (hp, + Frozen), then by replacer (max, - Frozen).
    Entity h = world.spawn(hound);
    CHECK(h != kNullEntity);
    if (h) {
        CHECK(world.get<Health>(h)->hp == 9 && world.get<Health>(h)->regen == 0.5f);  // regen kept from base
        CHECK(world.get<Pos>(h)->p == glm::vec2(1.0f, 2.0f));
        CHECK(!world.has(h, reg.find("Frozen")));
    }
    for (int i = 0; i < 3; ++i) world.spawn(hound);
    CHECK(world.prefabCount(hound) == 4);  // max = 4 from the replacer's patch
    // bat: replaced outright, so no Health.
    Entity b = world.spawn(bat);
    CHECK(b && world.get<Pos>(b)->p == glm::vec2(5.0f, 5.0f) && !world.has<Health>(b));
    Entity w = world.spawn(wolf);
    CHECK(w && world.get<Health>(w)->hp == 10);  // `true`: the C++ defaults
    CHECK(eval(scripts, content.env("base"), "return ecs.get(" + std::to_string(w) + ", 'Shiny').glow") == "1.5");

    // ── Init ──
    content.runInit();
    CHECK(eval(scripts, content.env("base"), "events.emit('ping') return pinged") == "1");

    // ── Manifest ──
    ContentManifest ours = content.manifest();
    CHECK(ours.schemaHash == reg.schemaHash() && ours.packs[0].version == "1.0.3" && ours.packs[0].hash != 0);
    ByteWriter bw;
    ours.write(bw);
    ByteReader br(bw.data());
    ContentManifest back;
    CHECK(back.read(br) && back.packs == ours.packs && back.schemaHash == ours.schemaHash);
    CHECK(ours.diff(back).empty());
    ByteReader truncated(bw.data().data(), bw.size() - 3);
    CHECK(!back.read(truncated) && back.packs.empty());

    ContentManifest host = ours;
    host.packs[1].version = "1.1";
    CHECK(ours.diff(host).size() == 1 && ours.diff(host)[0] == "coolmod: you have 1.0, the host has 1.1");
    host = ours;
    host.packs[1].hash ^= 1;
    CHECK(ours.diff(host)[0].find("files differ") != std::string::npos);
    host = ours;
    host.packs.push_back({"extra", "3", 1});
    CHECK(ours.diff(host)[0].find("extra 3: the host has it") != std::string::npos);
    host = ours;
    host.packs.pop_back();
    CHECK(ours.diff(host)[0].find("broken 1: you have it enabled") != std::string::npos);
    host = ours;
    std::swap(host.packs[1], host.packs[2]);
    CHECK(ours.diff(host)[0].find("load order") != std::string::npos);
    host = ours;
    host.schemaHash ^= 1;
    CHECK(ours.diff(host)[0].find("components differ") != std::string::npos);

    // A one-byte change anywhere in a pack changes its hash.
    uint64_t before = hashDirectory(mods / "cool");
    write(mods / "cool" / "prefabs" / "wolf.lua", "return { Health = true, Shiny = { glow = 1.6 } }");
    CHECK(hashDirectory(mods / "cool") != before);
    CHECK(hashDirectory(tmp / "nope") == 0);

    fs::remove_all(tmp);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("content_test: all checks passed\n");
    return 0;
}
