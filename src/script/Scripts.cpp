#include "eng/script/Scripts.h"

#include "eng/core/Log.h"

#include <lua/lauxlib.h>
#include <lua/lua.h>
#include <lua/lualib.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace eng {

namespace {

constexpr int kHookEvery = 1000;  // VM instructions between budget checks

// loaded[name] while the module's chunk runs: a second require is a cycle.
char kLoadingSentinel;

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = std::move(ss).str();
    return true;
}

// "ai.hound" / "ai/hound" → "ai/hound"; empty if it could escape the root.
std::string normalizeModule(std::string_view name) {
    if (name.empty() || name.size() > 200) return {};
    std::string out(name);
    bool segmentStart = true;
    for (char& c : out) {
        if (c == '.') c = '/';
        if (c == '/') {
            if (segmentStart) return {};  // leading, trailing or doubled separator
            segmentStart = true;
            continue;
        }
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return {};
        segmentStart = false;
    }
    return segmentStart ? std::string{} : out;
}

// Whether `path` resolves (through any symlinks) to somewhere inside `root`.
bool insideRoot(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    fs::path r = fs::weakly_canonical(root, ec);
    if (ec) return false;
    fs::path p = fs::weakly_canonical(path, ec);
    if (ec) return false;
    auto [rootEnd, _] = std::mismatch(r.begin(), r.end(), p.begin(), p.end());
    return rootEnd == r.end();
}

int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) {
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// Shallow copy of the table at `idx` (absolute), pushed.
void copyTable(lua_State* L, int idx) {
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, -4);
    }
}

}  // namespace

Scripts& Scripts::self(lua_State* L) { return **static_cast<Scripts**>(lua_getextraspace(L)); }

void* Scripts::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* s = static_cast<Scripts*>(ud);
    if (nsize == 0) {
        if (ptr) {
            s->m_memUsed -= osize;
            std::free(ptr);
        }
        return nullptr;
    }
    size_t old = ptr ? osize : 0;  // with no block, osize is a type tag
    if (nsize > old && s->m_memUsed + (nsize - old) > s->m_limits.memoryBytes) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (!p) return nullptr;
    s->m_memUsed = s->m_memUsed - old + nsize;
    return p;
}

void Scripts::hook(lua_State* L, lua_Debug*) {
    Scripts& s = self(L);
    if (s.m_callDepth == 0 || s.m_limits.instructionsPerCall == 0) return;
    s.m_instructions += kHookEvery;
    if (s.m_instructions > s.m_limits.instructionsPerCall) {
        s.m_exhausted = true;
        luaL_error(L, "instruction limit exceeded (%d)", static_cast<int>(s.m_limits.instructionsPerCall));
    }
}

void Scripts::raiseIfExhausted(lua_State* L) {
    if (m_exhausted) luaL_error(L, "instruction limit exceeded");
}

Scripts::Scripts(ScriptLimits limits) : m_limits(limits) {
    m_L = lua_newstate(&Scripts::alloc, this);
    if (!m_L) {
        ENG_LOG("[script] couldn't create a Lua state");
        std::abort();
    }
    lua_State* L = m_L;
    *static_cast<Scripts**>(lua_getextraspace(L)) = this;  // threads inherit it
    lua_atpanic(L, [](lua_State* L) -> int {
        ENG_LOG("[script] unprotected Lua error: " << (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
        std::abort();
    });

    // The real libraries live in the (never exposed) global table; each
    // environment gets copies. No io, os, debug or package at all.
    const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const auto& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // string.dump is reachable through any string's metatable ("".dump), so
    // it goes from the real library; and the metatable itself stays hidden.
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    lua_pushliteral(L, "");
    lua_getmetatable(L, -1);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 2);

    lua_newtable(L);
    m_sharedRef = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_sethook(L, &Scripts::hook, LUA_MASKCOUNT, kHookEvery);
}

Scripts::~Scripts() {
    if (m_L) lua_close(m_L);
}

void Scripts::exposeGlobal(const char* name) {
    lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_sharedRef);
    lua_insert(m_L, -2);
    lua_setfield(m_L, -2, name);
    lua_pop(m_L, 1);
}

ScriptEnv Scripts::addEnv(std::string name, fs::path root, ScriptTrust trust) {
    ScriptEnv id = static_cast<ScriptEnv>(m_envs.size());
    lua_newtable(m_L);
    int loadedRef = luaL_ref(m_L, LUA_REGISTRYINDEX);
    m_envs.push_back({std::move(name), std::move(root), trust, LUA_NOREF, loadedRef});
    buildEnvTable(id);
    if (trust == ScriptTrust::Base && m_baseEnv == kNoScriptEnv) m_baseEnv = id;
    return id;
}

void Scripts::pushEnv(ScriptEnv env) { lua_rawgeti(m_L, LUA_REGISTRYINDEX, m_envs.at(env).tableRef); }

void Scripts::buildEnvTable(ScriptEnv id) {
    lua_State* L = m_L;
    lua_newtable(L);
    int env = lua_gettop(L);
    lua_pushglobaltable(L);
    int g = lua_gettop(L);

    for (const char* name : {"assert", "error", "ipairs", "next", "pairs", "select", "tonumber", "tostring", "type",
                             "rawequal", "rawget", "rawset", "rawlen", "getmetatable", "_VERSION"}) {
        lua_getfield(L, g, name);
        lua_setfield(L, env, name);
    }
    lua_pushcfunction(L, &Scripts::luaPcall);
    lua_setfield(L, env, "pcall");
    lua_pushcfunction(L, &Scripts::luaXpcall);
    lua_setfield(L, env, "xpcall");
    lua_getfield(L, g, "setmetatable");
    lua_pushcclosure(L, &Scripts::luaSetmetatable, 1);
    lua_setfield(L, env, "setmetatable");
    lua_pushinteger(L, id);
    lua_pushcclosure(L, &Scripts::luaPrint, 1);
    lua_setfield(L, env, "print");
    lua_getfield(L, g, "load");
    lua_pushvalue(L, env);
    lua_pushcclosure(L, &Scripts::luaLoad, 2);
    lua_setfield(L, env, "load");
    lua_pushinteger(L, id);
    lua_pushcclosure(L, &Scripts::luaRequire, 1);
    lua_setfield(L, env, "require");

    for (const char* lib : {LUA_STRLIBNAME, LUA_TABLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME, LUA_COLIBNAME}) {
        lua_getfield(L, g, lib);
        copyTable(L, lua_gettop(L));
        lua_remove(L, -2);
        lua_setfield(L, env, lib);
    }
    lua_getfield(L, env, LUA_MATHLIBNAME);
    for (const char* name : {"random", "randomseed"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
    // resume/close catch errors from the coroutine: re-raise a spent budget.
    lua_getfield(L, env, LUA_COLIBNAME);
    for (const char* name : {"resume", "close"}) {
        lua_getfield(L, -1, name);
        lua_pushcclosure(L, &Scripts::luaResumeGuard, 1);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);

    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");

    lua_newtable(L);  // metatable: read-through to the shared API, and hidden
    lua_rawgeti(L, LUA_REGISTRYINDEX, m_sharedRef);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, env);

    lua_pop(L, 1);  // global table
    m_envs[id].tableRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

// ── Sandboxed base functions ─────────────────────────────────────────────────

int Scripts::luaFinishPcall(lua_State* L, int status, intptr_t extra) {
    self(L).raiseIfExhausted(L);
    if (status != LUA_OK && status != LUA_YIELD) {
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - static_cast<int>(extra);
}

int Scripts::luaPcall(lua_State* L) {
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, &Scripts::luaFinishPcall);
    return luaFinishPcall(L, status, 0);
}

// xpcall's message handler, guarded. When the instruction hook raises the
// limit error, Lua runs the message handler right there — inside the hook,
// where hooks are off — so a handler that loops would never be stopped.
// Once the budget is spent the script's handler is skipped and the error
// passes through as it is; for any other error it runs (hooks on, limited).
int Scripts::luaMessageGuard(lua_State* L) {
    if (self(L).m_exhausted) return 1;  // the error message, unchanged
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, 1);
    return 1;
}

int Scripts::luaXpcall(lua_State* L) {
    int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, &Scripts::luaMessageGuard, 1);
    lua_replace(L, 2);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);
    int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, &Scripts::luaFinishPcall);
    return luaFinishPcall(L, status, 2);
}

int Scripts::luaResumeGuard(lua_State* L) {
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    self(L).raiseIfExhausted(L);
    return lua_gettop(L);
}

int Scripts::luaSetmetatable(lua_State* L) {
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        bool hasGc = lua_rawget(L, 2) != LUA_TNIL;
        lua_pop(L, 1);
        if (hasGc) return luaL_error(L, "__gc metamethods are not allowed in scripts");
    }
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, 1);
    return 1;
}

int Scripts::luaPrint(lua_State* L) {
    Scripts& s = self(L);
    auto env = static_cast<size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    std::string line;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* str = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(str, len);
        lua_pop(L, 1);
    }
    ENG_LOG("[" << (env < s.m_envs.size() ? s.m_envs[env].name : "lua") << "] " << line);
    return 0;
}

// load(chunk [, chunkname [, mode [, env]]]): text only; env defaults to the caller's environment.
int Scripts::luaLoad(lua_State* L) {
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    if (n >= 2 && !lua_isnil(L, 2))
        lua_pushvalue(L, 2);
    else
        lua_pushliteral(L, "=(load)");
    lua_pushliteral(L, "t");
    if (n >= 4)
        lua_pushvalue(L, 4);
    else
        lua_pushvalue(L, lua_upvalueindex(2));
    lua_call(L, 4, LUA_MULTRET);
    return lua_gettop(L) - n;
}

// ── Modules ──────────────────────────────────────────────────────────────────

int Scripts::luaRequire(lua_State* L) {
    auto env = static_cast<ScriptEnv>(lua_tointeger(L, lua_upvalueindex(1)));
    std::string name = luaL_checkstring(L, 1);
    return self(L).requireIn(L, env, name);
}

bool Scripts::loadChunk(lua_State* L, ScriptEnv id, const std::string& module, fs::path& found) {
    const Env& env = m_envs[id];
    struct Candidate {
        const char* ext;
        const char* mode;
    };
    // Source first: in dev a stale .luac next to its .lua must not win.
    static constexpr Candidate base[] = {{".lua", "t"}, {".luac", "b"}};
    static constexpr Candidate mod[] = {{".lua", "t"}};
    const Candidate* begin = env.trust == ScriptTrust::Base ? std::begin(base) : std::begin(mod);
    const Candidate* end = env.trust == ScriptTrust::Base ? std::end(base) : std::end(mod);

    for (const Candidate* c = begin; c != end; ++c) {
        std::string rel = module + c->ext;
        fs::path path = env.root / rel;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        if (!insideRoot(env.root, path)) luaL_error(L, "module '%s' resolves outside %s", module.c_str(), env.name.c_str());
        std::string data;
        if (!readFile(path, data)) luaL_error(L, "couldn't read %s", path.string().c_str());
        // Base chunk names match eng-luac's ("@ai/hound.lua"), so tracebacks
        // read the same from source and bytecode.
        std::string chunkname = "@" + (env.trust == ScriptTrust::Base ? rel : env.name + "/" + rel);
        if (luaL_loadbufferx(L, data.data(), data.size(), chunkname.c_str(), c->mode) != LUA_OK) lua_error(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, env.tableRef);
        if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);  // a main chunk's only upvalue is _ENV
        found = path;
        return true;
    }
    return false;
}

int Scripts::requireIn(lua_State* L, ScriptEnv id, const std::string& rawName) {
    std::string name = normalizeModule(rawName);
    if (name.empty()) return luaL_error(L, "bad module name '%s'", rawName.c_str());
    const int loadedRef = m_envs[id].loadedRef;

    lua_rawgeti(L, LUA_REGISTRYINDEX, loadedRef);
    int loaded = lua_gettop(L);
    if (lua_getfield(L, loaded, name.c_str()) != LUA_TNIL) {
        if (lua_touserdata(L, -1) == &kLoadingSentinel) return luaL_error(L, "cyclic require of '%s'", name.c_str());
        lua_remove(L, loaded);
        return 1;
    }
    lua_pop(L, 1);

    fs::path path;
    if (!loadChunk(L, id, name, path)) {
        lua_pop(L, 1);
        if (m_envs[id].trust == ScriptTrust::Mod && m_baseEnv != kNoScriptEnv) return requireIn(L, m_baseEnv, name);
        return luaL_error(L, "module '%s' not found in %s", name.c_str(), m_envs[id].name.c_str());
    }
    lua_pushlightuserdata(L, &kLoadingSentinel);
    lua_setfield(L, loaded, name.c_str());

    lua_pushstring(L, name.c_str());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {  // so a failed module can be required again
        lua_pushnil(L);
        lua_setfield(L, loaded, name.c_str());
        return lua_error(L);
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, loaded, name.c_str());
    lua_remove(L, loaded);

    std::error_code ec;
    m_modules.push_back({id, name, path, fs::last_write_time(path, ec)});
    return 1;
}

bool Scripts::require(ScriptEnv env, std::string_view module) {
    lua_pushinteger(m_L, env);
    lua_pushcclosure(m_L, &Scripts::luaRequire, 1);
    lua_pushlstring(m_L, module.data(), module.size());
    return call(1, 1);
}

bool Scripts::call(int nargs, int nresults) {
    lua_State* L = m_L;
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    if (m_callDepth++ == 0) {
        m_instructions = 0;
        m_exhausted = false;
    }
    int status = lua_pcall(L, nargs, nresults, base);
    if (--m_callDepth == 0) m_exhausted = false;
    lua_remove(L, base);
    if (status != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        m_lastError = msg ? msg : "(non-string error)";
        ENG_LOG("[script] " << m_lastError);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

int Scripts::reloadChanged() {
    lua_State* L = m_L;
    int reloaded = 0;
    for (size_t i = 0; i < m_modules.size(); ++i) {  // by index: a reload may require new modules
        std::error_code ec;
        auto mtime = fs::last_write_time(m_modules[i].path, ec);
        if (ec || mtime == m_modules[i].mtime) continue;
        m_modules[i].mtime = mtime;
        const ScriptEnv id = m_modules[i].env;
        const std::string name = m_modules[i].name;
        const fs::path path = m_modules[i].path;
        const Env& env = m_envs[id];

        std::string data;
        if (!readFile(path, data)) continue;
        bool binary = path.extension() == ".luac";
        std::string chunkname = "@" + (env.trust == ScriptTrust::Base ? fs::relative(path, env.root).generic_string()
                                                                       : env.name + "/" + fs::relative(path, env.root).generic_string());
        if (luaL_loadbufferx(L, data.data(), data.size(), chunkname.c_str(), binary ? "b" : "t") != LUA_OK) {
            m_lastError = lua_tostring(L, -1);
            ENG_LOG("[script] reload failed: " << m_lastError);
            lua_pop(L, 1);
            continue;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, env.tableRef);
        if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
        lua_pushstring(L, name.c_str());
        if (!call(1, 1)) continue;
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        int fresh = lua_gettop(L);

        lua_rawgeti(L, LUA_REGISTRYINDEX, m_envs[id].loadedRef);
        int loaded = lua_gettop(L);
        lua_getfield(L, loaded, name.c_str());
        int old = lua_gettop(L);
        if (lua_istable(L, old) && lua_istable(L, fresh)) {
            // In place: drop what's gone, then copy everything new over.
            lua_pushnil(L);
            while (lua_next(L, old)) {
                lua_pop(L, 1);
                lua_pushvalue(L, -1);
                if (lua_rawget(L, fresh) == LUA_TNIL) {
                    lua_pushvalue(L, -2);
                    lua_pushnil(L);
                    lua_rawset(L, old);  // clearing a field mid-traversal is allowed
                }
                lua_pop(L, 1);
            }
            lua_pushnil(L);
            while (lua_next(L, fresh)) {
                lua_pushvalue(L, -2);
                lua_insert(L, -2);
                lua_rawset(L, old);
            }
        } else {
            lua_pushvalue(L, fresh);
            lua_setfield(L, loaded, name.c_str());
        }
        lua_settop(L, fresh - 1);
        ++reloaded;
        ENG_LOG("[script] reloaded " << env.name << ":" << name);
    }
    return reloaded;
}

}  // namespace eng
