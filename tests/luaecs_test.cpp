// eng::ecs::LuaEcs tests: components declared from Lua, entities and prefabs
// from scripts, reflected field access, deferred changes during ecs.each,
// read-only API tables, events (incl. hot reload) and limits inside
// callbacks. Run with `zig build test`.
#include "eng/ecs/LuaEcs.h"
#include "eng/script/Scripts.h"

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
    uint8_t armor = 0;
    bool boss = false;
    Entity lastHitBy = kNullEntity;
};

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// Runs `code` in `env`; its first result as a string ("" on error or nil).
static std::string eval(Scripts& s, ScriptEnv env, const std::string& code, bool* ok = nullptr) {
    lua_State* L = s.L();
    if (luaL_loadbufferx(L, code.data(), code.size(), "=test", "t") != LUA_OK) {
        std::fprintf(stderr, "  (load error: %s)\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        if (ok) *ok = false;
        return "";
    }
    s.pushEnv(env);
    lua_setupvalue(L, -2, 1);
    bool good = s.call(0, 1);
    if (ok) *ok = good;
    if (!good) return "";
    std::string out = lua_isnil(L, -1) ? "" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

static bool fails(Scripts& s, ScriptEnv env, const std::string& code, const char* expect) {
    bool ok = true;
    eval(s, env, code, &ok);
    if (ok) return false;
    if (s.lastError().find(expect) != std::string::npos) return true;
    std::fprintf(stderr, "  (expected '%s' in: %s)\n", expect, s.lastError().c_str());
    return false;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() /
                         ("eng_luaecs_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(tmp);
    const fs::path baseDir = tmp / "base", modDir = tmp / "mods" / "m";
    write(baseDir / "components.lua",
          "ecs.component('Frozen', { {'time', {type = 'f32', net = 'fixed16', scale = 0.01}}, {'stacks', 'u8'}, {'by', 'entity'} })\n"
          "ecs.component('Tag', { zeta = 'bool', alpha = 'i16' })\n");
    write(baseDir / "handlers.lua", "events.on('hit', function(e, dmg) hits = (hits or 0) + dmg end)\nreturn true");
    write(modDir / "x.lua", "return 1");

    Scripts s({.memoryBytes = 16u << 20, .instructionsPerCall = 2'000'000});
    ScriptEnv base = s.addEnv("base", baseDir, ScriptTrust::Base);
    ScriptEnv mod = s.addEnv("m", modDir, ScriptTrust::Mod);

    Registry reg;
    LuaEcs luaEcs(s, reg);
    reg.add<Pos>("Pos", {field("p", &Pos::p)});
    reg.add<Health>("Health", {field("hp", &Health::hp), field("armor", &Health::armor), field("boss", &Health::boss),
                               field("lastHitBy", &Health::lastHitBy)},
                    Health{10, 0, false, kNullEntity});

    // ── Components declared from Lua ──
    CHECK(s.require(base, "components"));
    lua_settop(s.L(), 0);
    CHECK(fails(s, base, "ecs.component('Frozen', {a = 'u8'})", "twice"));  // Registry's exception → Lua error
    CHECK(fails(s, base, "ecs.component('Bad', {a = 'u64'})", "unknown type"));
    CHECK(fails(s, base, "return ecs.get(1, 'Pos')", "before the registry is finalized"));
    reg.finalize();
    CHECK(fails(s, base, "return ecs.get(1, 'Pos')", "no world"));
    CHECK(fails(s, base, "ecs.component('Late', {a = 'u8'})", "finalized"));
    {
        const ComponentInfo& frozen = reg.info(reg.find("Frozen"));
        CHECK(frozen.fromSchema && frozen.fields.size() == 3 && frozen.fields[0].name == "time" &&
              frozen.fields[0].options.net == NetEncoding::Fixed16 && frozen.fields[2].type == FieldType::Entity);
        const ComponentInfo& tag = reg.info(reg.find("Tag"));
        CHECK(tag.fields.size() == 2 && tag.fields[0].name == "alpha" && tag.fields[1].name == "zeta");  // map form: sorted
    }

    World world(reg, {.maxEntities = 64});
    luaEcs.attach(&world);
    PrefabId hound = world.definePrefab("hound", 2);
    world.setPrefabComponent(hound, Health{3, 1, false, kNullEntity});
    world.setPrefabComponent(hound, Pos{{1.0f, 2.0f}});

    // ── Entities and prefabs ──
    CHECK(eval(s, base, "a = ecs.spawn('hound') b = ecs.spawn('hound') return ecs.spawn('hound') == nil") == "true");
    CHECK(eval(s, base, "return ecs.prefab(a) .. ecs.size() .. ecs.capacity()") == "hound264");
    CHECK(fails(s, base, "ecs.spawn('wolf')", "no prefab named 'wolf'"));
    CHECK(eval(s, base, "return ecs.get(a, 'Health').hp + ecs.get(a, 'Pos').p.y") == "5.0");
    CHECK(eval(s, base, "return ecs.id('Health') == ecs.id('Health') and ecs.has(a, ecs.id('Health'))") == "true");
    CHECK(eval(s, base, "return ecs.get(a, 'Frozen') == nil and ecs.id('Nope') == nil") == "true");

    // ── Fields ──
    CHECK(eval(s, base, "local h = ecs.get(a, 'Health') h.hp = h.hp - 1 h.boss = true h.lastHitBy = b return h.hp") == "2");
    {
        Entity a = world.entityAt(0);
        const Health* h = world.get<Health>(a);
        CHECK(h && h->hp == 2 && h->boss && h->lastHitBy == world.entityAt(1));
    }
    CHECK(eval(s, base, "local p = ecs.get(a, 'Pos') p.p.x = 7 p.p = {x = p.p.x, y = 8} return p.p.x + p.p.y") == "15.0");
    CHECK(eval(s, base, "ecs.get(b, 'Pos').p = ecs.get(a, 'Pos').p return ecs.get(b, 'Pos').p.y") == "8.0");
    CHECK(eval(s, base, "ecs.get(b, 'Pos').p = {3, 4} return tostring(ecs.get(b, 'Pos').p.x)") == "3.0");
    CHECK(fails(s, base, "ecs.get(a, 'Health').armor = 256", "out of range"));
    CHECK(fails(s, base, "ecs.get(a, 'Health').armor = -1", "out of range"));
    CHECK(fails(s, base, "ecs.get(a, 'Health').hp = 1.5", "integer"));
    CHECK(fails(s, base, "ecs.get(a, 'Health').boss = 1", "boolean"));
    CHECK(fails(s, base, "return ecs.get(a, 'Health').hpp", "Health has no field 'hpp'"));
    CHECK(fails(s, base, "ecs.get(a, 'Pos').p = 'up'", "expected a vec2"));
    CHECK(fails(s, base, "return ecs.get(a, 'Pos').p.z", "only x and y"));

    // ── add / remove, init tables, stale refs ──
    CHECK(eval(s, base, "c = ecs.spawn() local f = ecs.add(c, 'Frozen', {time = 1.5, stacks = 2, by = a}) "
                        "return f.time + f.stacks") == "3.5");
    CHECK(eval(s, base, "return ecs.add(c, 'Health').hp") == "10");  // C++ defaults
    CHECK(fails(s, base, "ecs.add(c, 'Frozen', {tiem = 1})", "Frozen has no field 'tiem'"));
    CHECK(eval(s, base, "held = ecs.get(c, 'Frozen') ecs.remove(c, 'Frozen') return ecs.has(c, 'Frozen')") == "false");
    CHECK(fails(s, base, "return held.time", "has no Frozen"));
    CHECK(eval(s, base, "ecs.destroy(c) return ecs.alive(c)") == "false");
    CHECK(fails(s, base, "ecs.add(c, 'Pos')", "isn't alive"));

    // ── ecs.each, with structural changes deferred to commands ──
    CHECK(eval(s, base, "local n = 0 ecs.each('Health', 'Pos', function(e) n = n + ecs.get(e, 'Health').hp end) return n") == "5");
    CHECK(eval(s, base, "spawned = {} ecs.each('Health', function(e) spawned[#spawned + 1] = ecs.spawn() ecs.destroy(e) end) "
                        "return #spawned .. tostring(ecs.alive(a))") == "2true");  // destroy waits for flush
    CHECK(!world.commands().empty());
    world.flush();
    CHECK(eval(s, base, "return tostring(ecs.alive(a)) .. ecs.size()") == "false2");
    CHECK(!world.iterating());

    // ── Errors and limits inside callbacks leave the world usable ──
    CHECK(eval(s, base, "d = ecs.spawn('hound') return d ~= nil") == "true");
    // ecs.add inside ecs.each is deferred: no ref (it'd point at nothing, or
    // at values the queued add then overwrites); init's values land at flush.
    CHECK(eval(s, base, "deferred = ecs.spawn() local r = 'x' ecs.each('Health', function(e) "
                        "if r == 'x' then r = ecs.add(deferred, 'Health', {hp = 7}) end end) "
                        "return tostring(r) .. tostring(ecs.has(deferred, 'Health'))") == "nilfalse");
    world.flush();
    CHECK(eval(s, base, "return ecs.get(deferred, 'Health').hp") == "7");
    CHECK(eval(s, base, "ecs.destroy(deferred)") == "");
    CHECK(fails(s, base, "ecs.each('Health', function(e) error('in callback') end)", "in callback"));
    CHECK(!world.iterating());
    CHECK(fails(s, mod, "ecs.each('Health', function(e) while true do end end)", "instruction limit"));
    CHECK(!world.iterating());
    CHECK(eval(s, base, "ecs.destroy(d) return ecs.alive(d)") == "false");  // not deferred: iteration really ended

    // ── The API tables are read-only for everyone ──
    CHECK(fails(s, mod, "ecs.spawn = function() end", "read-only"));
    CHECK(fails(s, mod, "events.on = nil", "read-only"));
    CHECK(fails(s, mod, "rawset(ecs, \"spawn\", function() end)", "read-only"));
    CHECK(fails(s, mod, "rawset(events, \"on\", nil)", "read-only"));
    CHECK(eval(s, mod, "return getmetatable(ecs)") == "false");
    CHECK(eval(s, base, "return type(ecs.spawn)") == "function");
    CHECK(eval(s, mod, "return getmetatable(ecs.get(spawned and 0 or 0, 'Pos') or {})") == "");

    // ── Events ──
    CHECK(s.require(base, "handlers"));
    lua_settop(s.L(), 0);
    CHECK(eval(s, mod, "events.on('hit', function(e, dmg) if dmg > 100 then error('mod handler broke') end end) return 1") == "1");
    CHECK(eval(s, base, "events.emit('hit', 0, 2) events.emit('nobody', 1) return hits") == "2");
    lua_pushinteger(s.L(), 0);
    lua_pushinteger(s.L(), 3);
    CHECK(luaEcs.emit("hit", 2));
    CHECK(eval(s, base, "return hits") == "5");
    lua_pushinteger(s.L(), 0);
    lua_pushinteger(s.L(), 1000);
    CHECK(!luaEcs.emit("hit", 2));  // the mod handler failed (logged) ...
    CHECK(eval(s, base, "return hits") == "1005");  // ... after the base one ran
    CHECK(lua_gettop(s.L()) == 0);

    // Hot reload replaces the file's handlers instead of adding a second set.
    write(baseDir / "handlers.lua", "events.on('hit', function(e, dmg) hits = (hits or 0) + 10 * dmg end)\nreturn true");
    fs::last_write_time(baseDir / "handlers.lua", fs::file_time_type::clock::now() + std::chrono::seconds(2));
    CHECK(s.reloadChanged() >= 1);
    CHECK(eval(s, base, "hits = 0 events.emit('hit', 0, 1) return hits") == "10");

    // ── Gone bindings fail cleanly ──
    {
        Scripts s2;
        ScriptEnv e2 = s2.addEnv("base", baseDir, ScriptTrust::Base);
        Registry r2;
        { LuaEcs gone(s2, r2); }
        CHECK(fails(s2, e2, "return ecs.size()", "bindings are gone"));
    }

    fs::remove_all(tmp);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("luaecs_test: all checks passed\n");
    return 0;
}
