#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Lua 5.4 is compiled as C++ (errors are exceptions, so they unwind C++
// frames in bindings): include <lua/lua.h> and <lua/lauxlib.h> directly,
// never lua.hpp, whose extern "C" would mismatch the symbols.
struct lua_State;
struct lua_Debug;

namespace eng {

struct ScriptLimits {
    // Whole lua_State. Past it, allocations fail and Lua raises "not enough memory".
    size_t memoryBytes = size_t(64) << 20;
    // Per top-level call() (nested calls share it); 0 = unlimited. Checked every
    // 1000 VM instructions. Once spent, the error can't be swallowed by pcall.
    uint64_t instructionsPerCall = 50'000'000;
};

enum class ScriptTrust : uint8_t {
    // The game's own scripts: source (.lua) in dev, bytecode (.luac) in release.
    Base,
    // Third-party: source only. Bytecode is never loaded from a mod — malformed
    // bytecode can corrupt memory.
    Mod,
};

using ScriptEnv = int;
constexpr ScriptEnv kNoScriptEnv = -1;

// One lua_State holding sandboxed environments (the base game, then one per
// mod), each with its own globals, module cache and require root.
//
// Scripts see: assert error ipairs next pairs select tonumber tostring type
// raw* getmetatable setmetatable pcall xpcall print require load (text only),
// and copies of string table math utf8 coroutine. Not: io os debug package
// dofile loadfile collectgarbage string.dump math.random/randomseed (the game
// binds its own deterministic RNG). setmetatable refuses __gc (finalizers run
// with hooks off, so they would escape the instruction limit).
//
// For determinism the string-hash seed is constant, so pairs() order over
// string/number keys is the same on every machine. Tables or functions used
// as keys hash by address: don't rely on pairs() order over those.
class Scripts {
public:
    explicit Scripts(ScriptLimits limits = {});
    ~Scripts();
    Scripts(const Scripts&) = delete;
    Scripts& operator=(const Scripts&) = delete;

    lua_State* L() const { return m_L; }

    // Pops the value on top of the stack and makes it a global every
    // environment can read (an environment can shadow it, not replace it).
    void exposeGlobal(const char* name);

    // A sandboxed environment rooted at `root`: require("ai.hound") loads
    // root/ai/hound.lua (or, for Base, hound.luac). A Mod's require falls back
    // to the first Base environment's modules — the same tables, so a mod can
    // patch base behavior. `name` labels errors and chunk names.
    ScriptEnv addEnv(std::string name, std::filesystem::path root, ScriptTrust trust);
    ScriptEnv baseEnv() const { return m_baseEnv; }

    // Pushes an environment's globals table.
    void pushEnv(ScriptEnv env);

    // require() from C++: on success pushes the module's value and returns
    // true; on failure logs, pushes nothing and returns false.
    bool require(ScriptEnv env, std::string_view module);

    // Like lua_pcall, with the instruction budget and a traceback. On error
    // logs it, keeps it in lastError(), pops it and returns false.
    bool call(int nargs, int nresults);

    // Re-runs modules whose file changed on disk (dev hot reload). Module
    // tables are updated in place, so references held elsewhere see the new
    // functions; modules that required a reloaded one aren't re-run.
    // Returns how many were reloaded.
    int reloadChanged();

    size_t memoryUsed() const { return m_memUsed; }
    const std::string& lastError() const { return m_lastError; }

private:
    struct Env {
        std::string name;
        std::filesystem::path root;
        ScriptTrust trust;
        int tableRef;   // registry ref: the globals table
        int loadedRef;  // registry ref: module name → value
    };
    struct Module {
        ScriptEnv env;
        std::string name;
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };

    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void hook(lua_State* L, lua_Debug* ar);
    static int luaRequire(lua_State* L);
    static int luaPcall(lua_State* L);
    static int luaXpcall(lua_State* L);
    static int luaMessageGuard(lua_State* L);
    static int luaFinishPcall(lua_State* L, int status, intptr_t ctx);
    static int luaResumeGuard(lua_State* L);
    static int luaSetmetatable(lua_State* L);
    static int luaPrint(lua_State* L);
    static int luaLoad(lua_State* L);
    static Scripts& self(lua_State* L);

    void buildEnvTable(ScriptEnv env);
    int requireIn(lua_State* L, ScriptEnv env, const std::string& module);
    bool loadChunk(lua_State* L, ScriptEnv env, const std::string& module,
                   std::filesystem::path& foundPath);
    void raiseIfExhausted(lua_State* L);

    lua_State* m_L = nullptr;
    ScriptLimits m_limits;
    size_t m_memUsed = 0;
    uint64_t m_instructions = 0;
    bool m_exhausted = false;
    int m_callDepth = 0;
    int m_sharedRef = 0;
    ScriptEnv m_baseEnv = kNoScriptEnv;
    std::vector<Env> m_envs;
    std::vector<Module> m_modules;
    std::string m_lastError;
};

}  // namespace eng
