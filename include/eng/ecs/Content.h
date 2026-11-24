#pragma once

// eng::ecs::Content — loads the game's scripted content: the base game, then
// mods in the player's order. Phase 4 of docs/ecs-lua-plan.md.
//
// A pack (the base game, or one mod) is a directory; all files optional:
//
//   mod.lua          mods only: return { id = "coolmod", version = "1.2.0",
//                                        name = "Cool Mod", requires = {"other"} }
//                    (pure data: runs with no globals at all)
//   components.lua   ecs.component(...) declarations
//   prefabs/**.lua   one prefab per file, named by its path: prefabs/enemies/hound.lua
//                    is "enemies/hound". return { max = 8, Health = { hp = 3 }, Pos = true }
//   init.lua         everything else at startup (events.on handlers, ...)
//
// Startup, in this order:
//
//   Content content(scripts, registry, luaEcs);
//   content.setBase(baseDir, "1.0.3");
//   content.enable(content.discover(modsDir), playerOrder);
//   content.declareComponents();        // every pack's components.lua
//   registry.finalize();
//   World world(registry, ...);  luaEcs.attach(&world);
//   content.loadPrefabs(world);         // every pack's prefabs, merged
//   content.runInit();                  // every pack's init.lua
//
// Each stage runs pack by pack in load order. A later pack's prefab with the
// same name replaces the earlier one (logged), or with `patch = true` sets
// only the fields it names (`Comp = false` removes a component; `max` if
// given). Prefabs are defined in the world sorted by name, so PrefabIds are
// the same on every peer with the same content.
//
// Errors don't stop loading: they're logged and collected in errors() (for a
// mods menu). A pack whose components.lua or init.lua fails is marked failed
// and skips its later stages; a bad prefab file skips just that prefab.
//
// manifest() is what peers compare when joining: packs in order with
// version and a hash of every file in them, plus the registry's schema hash.
// A mismatch should refuse the join with diff()'s reasons.

#include "eng/ecs/LuaEcs.h"
#include "eng/net/ByteStream.h"
#include "eng/script/Scripts.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eng::ecs {

struct ModInfo {
    std::string id;       // [a-z0-9_-], unique; not "base"
    std::string version;
    std::string name;     // display name (defaults to id)
    std::vector<std::string> dependencies;  // mod.lua `requires`: ids that must be enabled earlier
    std::filesystem::path root;
};

struct PackStamp {
    std::string id;
    std::string version;
    uint64_t hash = 0;  // every file in the pack: relative paths and bytes
    bool operator==(const PackStamp&) const = default;
};

struct ContentManifest {
    std::vector<PackStamp> packs;  // base first, then mods in load order
    uint64_t schemaHash = 0;

    void write(ByteWriter& w) const;
    bool read(ByteReader& r);  // false if malformed
    // Why `ours` can't join `host` (one line each); empty when they match.
    std::vector<std::string> diff(const ContentManifest& host) const;
};

// FNV-1a over every regular file under `root`, in sorted relative-path order
// (path, then size, then bytes). 0 if root isn't a directory.
uint64_t hashDirectory(const std::filesystem::path& root);

class Content {
public:
    // All three must outlive this.
    Content(Scripts& scripts, Registry& registry, LuaEcs& luaEcs);

    // The base game's directory and version (for the manifest). First.
    void setBase(std::filesystem::path root, std::string version);

    // Every <modsDir>/<folder> with a valid mod.lua, sorted by id. Bad or
    // duplicate manifests are logged, recorded in errors() and left out.
    std::vector<ModInfo> discover(const std::filesystem::path& modsDir);

    // Enables mods from `available` in the order of `ids` (the player's
    // list). Unknown or repeated ids, and mods whose requires aren't enabled
    // before them, are skipped (logged). Returns false if any were. Once,
    // after setBase.
    bool enable(const std::vector<ModInfo>& available, const std::vector<std::string>& ids);

    void declareComponents();       // before Registry::finalize
    void loadPrefabs(World& world); // after it; defines the prefabs in `world`
    void runInit();

    ContentManifest manifest() const;  // after Registry::finalize
    const std::vector<std::string>& errors() const { return m_errors; }

    // The script environment of a pack ("base" or a mod id), or kNoScriptEnv.
    ScriptEnv env(std::string_view id) const;
    bool failed(std::string_view id) const;

private:
    struct Pack {
        std::string id, version;
        std::filesystem::path root;
        ScriptTrust trust;
        ScriptEnv env = kNoScriptEnv;
        uint64_t hash = 0;
        bool failed = false;
    };
    void runFileIn(Pack& pack, const char* module);
    void error(const std::string& message);
    bool readManifest(const std::filesystem::path& file, ModInfo& out);

    Scripts& m_scripts;
    Registry& m_registry;
    LuaEcs& m_luaEcs;
    std::vector<Pack> m_packs;  // [0] = base
    bool m_enabled = false;
    std::vector<std::string> m_errors;
};

}  // namespace eng::ecs
