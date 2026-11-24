// zig build test — eng::Scripts: sandbox, limits, modules, mods, hot reload,
// bytecode from eng-luac. argv[1] is the eng-luac output for tests/scripts.

#include "eng/script/Scripts.h"

#include <lua/lauxlib.h>
#include <lua/lua.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace eng;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// Runs `code` as a chunk in `env`; returns its first result as a string ("" on error/nil).
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

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: script_test <compiled-scripts-dir>\n");
        return 2;
    }
    const fs::path compiled = argv[1];
    const fs::path tmp = fs::temp_directory_path() / ("eng_script_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(tmp);
    const fs::path baseDir = tmp / "base", modDir = tmp / "mods" / "coolmod";

    write(baseDir / "util" / "math2.lua", "local M = {} function M.double(x) return x * 2 end return M");
    write(baseDir / "cycle_a.lua", "return require 'cycle_b'");
    write(baseDir / "cycle_b.lua", "return require 'cycle_a'");
    write(baseDir / "broken.lua", "error('boom')");
    write(modDir / "main.lua", "local m = require 'util.math2' return m.double(21)");
    write(modDir / "sneaky.luac", "\x1bLua not really");
    write(tmp / "outside.lua", "return 'escaped'");

    Scripts s({.memoryBytes = 8u << 20, .instructionsPerCall = 2'000'000});
    ScriptEnv base = s.addEnv("base", baseDir, ScriptTrust::Base);
    ScriptEnv mod = s.addEnv("coolmod", modDir, ScriptTrust::Mod);

    // ── Modules ──
    CHECK(eval(s, base, "return require('util.math2').double(4)") == "8");
    CHECK(eval(s, base, "return require('util/math2') == require('util.math2')") == "true");
    CHECK(eval(s, mod, "return require 'main'") == "42");  // falls back to base's util.math2
    bool ok = true;
    eval(s, base, "return require 'cycle_a'", &ok);
    CHECK(!ok && s.lastError().find("cyclic") != std::string::npos);
    eval(s, base, "return require 'broken'", &ok);
    CHECK(!ok);
    eval(s, base, "return require 'broken'", &ok);  // a failed module isn't cached as loading
    CHECK(!ok && s.lastError().find("boom") != std::string::npos);
    for (const char* bad : {"../outside", "..outside", "/etc/passwd", "a//b", "a.b.", ""}) {
        eval(s, base, std::string("return require '") + bad + "'", &ok);
        CHECK(!ok);
    }
    eval(s, mod, "return require 'sneaky'", &ok);  // mods never load bytecode
    CHECK(!ok && s.lastError().find("not found") != std::string::npos);
    CHECK(s.require(base, "util.math2"));
    CHECK(lua_istable(s.L(), -1));
    lua_settop(s.L(), 0);

    // ── Sandbox ──
    CHECK(eval(s, mod, "return io == nil and os == nil and debug == nil and package == nil and dofile == nil "
                       "and loadfile == nil and collectgarbage == nil") == "true");
    CHECK(eval(s, mod, "return string.dump == nil and ('').dump == nil and math.random == nil") == "true");
    CHECK(eval(s, mod, "return getmetatable('')") == "false");
    CHECK(eval(s, mod, "string.upper = nil return ('x'):upper()") == "X");  // per-env library copies
    CHECK(eval(s, base, "return string.upper('y')") == "Y");
    CHECK(eval(s, mod, "return _G == _ENV") == "true");
    eval(s, mod, "setmetatable({}, {__gc = function() end})", &ok);
    CHECK(!ok);
    CHECK(eval(s, mod, "return load('return 1 + 1')()") == "2");
    CHECK(eval(s, mod, "secret = 7 return load('return secret')()") == "7");  // load sees the caller's env
    CHECK(eval(s, mod, "return select(2, load('\\27Lua'))") .find("binary") != std::string::npos);
    CHECK(eval(s, base, "return secret") == "");  // env globals don't leak

    lua_pushinteger(s.L(), 99);
    s.exposeGlobal("API_VERSION");
    CHECK(eval(s, mod, "return API_VERSION") == "99");
    CHECK(eval(s, mod, "API_VERSION = 1 return API_VERSION") == "1");
    CHECK(eval(s, base, "return API_VERSION") == "99");  // shadowed in mod only

    // ── Limits ──
    eval(s, mod, "while true do end", &ok);
    CHECK(!ok && s.lastError().find("instruction limit") != std::string::npos);
    eval(s, mod, "while true do pcall(function() while true do end end) end", &ok);
    CHECK(!ok && s.lastError().find("instruction limit") != std::string::npos);
    eval(s, mod, "while true do xpcall(function() while true do end end, function() while true do end end) end", &ok);
    CHECK(!ok);
    eval(s, mod, "while true do coroutine.resume(coroutine.create(function() while true do end end)) end", &ok);
    CHECK(!ok && s.lastError().find("instruction limit") != std::string::npos);
    CHECK(eval(s, mod, "local n = 0 for i = 1, 1000 do n = n + i end return n") == "500500");  // budget resets
    eval(s, mod, "local t = {} for i = 1, 100 do t[i] = string.rep('x', 1 << 20) end", &ok);
    CHECK(!ok && s.lastError().find("memory") != std::string::npos);
    CHECK(eval(s, mod, "return #string.rep('x', 1000)") == "1000");  // still usable afterwards
    CHECK(eval(s, mod, "return pcall(function() return 1 end)") == "true");
    CHECK(eval(s, mod, "return coroutine.wrap(function() coroutine.yield(5) end)()") == "5");
    CHECK(eval(s, mod, "return select(2, pcall(coroutine.wrap(function() pcall(coroutine.yield, 3) end)))") == "3");

    // ── Determinism: pairs() order over string keys is the same in every state ──
    const std::string order = "local t = {} for i = 1, 200 do t['k' .. i] = i end "
                              "local out = {} for k in pairs(t) do out[#out + 1] = k end return table.concat(out, ',')";
    std::string first = eval(s, base, order);
    {
        Scripts other;
        std::vector<char> pad(12345);  // a different heap layout
        ScriptEnv e = other.addEnv("base", baseDir, ScriptTrust::Base);
        CHECK(eval(other, e, order) == first);
    }

    // ── Hot reload: module tables update in place ──
    write(baseDir / "hot.lua", "return { value = function() return 1 end, gone = true }");
    CHECK(eval(s, base, "held = require 'hot' return held.value()") == "1");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write(baseDir / "hot.lua", "return { value = function() return 2 end }");
    fs::last_write_time(baseDir / "hot.lua", fs::file_time_type::clock::now() + std::chrono::seconds(1));
    CHECK(s.reloadChanged() == 1);
    CHECK(eval(s, base, "return held.value()") == "2");
    CHECK(eval(s, base, "return held.gone == nil and held == require 'hot'") == "true");
    CHECK(s.reloadChanged() == 0);

    // ── Bytecode from eng-luac (release path) ──
    Scripts rel;
    ScriptEnv r = rel.addEnv("base", compiled, ScriptTrust::Base);
    CHECK(fs::exists(compiled / "greet.luac") && !fs::exists(compiled / "greet.lua"));
    CHECK(eval(rel, r, "return require('greet').hello('luac')") == "hello luac");
    eval(rel, r, "return require('greet').fail()", &ok);
    CHECK(!ok && rel.lastError().find("greet.lua:") != std::string::npos);  // debug info kept

    fs::remove_all(tmp);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("script_test: all checks passed\n");
    return 0;
}
