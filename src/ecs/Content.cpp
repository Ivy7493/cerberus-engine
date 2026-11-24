#include "eng/ecs/Content.h"

#include "eng/core/Log.h"

#include <lua/lauxlib.h>
#include <lua/lua.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace eng::ecs {

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void fnv(uint64_t& h, const void* data, size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * kFnvPrime;
}

bool validModId(const std::string& id) {
    if (id.empty() || id.size() > 64 || id == "base") return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool readFile(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = std::move(ss).str();
    return true;
}

}  // namespace

uint64_t hashDirectory(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    std::vector<std::pair<std::string, fs::path>> files;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.emplace_back(fs::relative(it->path(), root).generic_string(), it->path());
    std::sort(files.begin(), files.end());
    uint64_t h = kFnvOffset;
    std::string data;
    for (const auto& [rel, path] : files) {
        fnv(h, rel.data(), rel.size() + 1);  // with its '\0': "ab"+"c" ≠ "a"+"bc"
        data.clear();
        readFile(path, data);
        uint64_t size = data.size();
        fnv(h, &size, sizeof size);
        fnv(h, data.data(), data.size());
    }
    return h;
}

// ── Manifest ─────────────────────────────────────────────────────────────────

void ContentManifest::write(ByteWriter& w) const {
    w.varU32(uint32_t(packs.size()));
    for (const auto& p : packs) {
        w.str(p.id);
        w.str(p.version);
        w.u64(p.hash);
    }
    w.u64(schemaHash);
}

bool ContentManifest::read(ByteReader& r) {
    *this = {};
    uint32_t n = r.varU32();
    if (n > 1024) return false;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        PackStamp p;
        p.id = r.str(64);
        p.version = r.str(64);
        p.hash = r.u64();
        packs.push_back(std::move(p));
    }
    schemaHash = r.u64();
    if (!r.ok()) *this = {};
    return r.ok();
}

std::vector<std::string> ContentManifest::diff(const ContentManifest& host) const {
    std::vector<std::string> out;
    auto find = [](const ContentManifest& m, const std::string& id) -> const PackStamp* {
        for (const auto& p : m.packs)
            if (p.id == id) return &p;
        return nullptr;
    };
    for (const auto& h : host.packs) {
        const PackStamp* o = find(*this, h.id);
        if (!o) out.push_back(h.id + " " + h.version + ": the host has it, you don't");
        else if (o->version != h.version) out.push_back(h.id + ": you have " + o->version + ", the host has " + h.version);
        else if (o->hash != h.hash) out.push_back(h.id + " " + h.version + ": your files differ from the host's");
    }
    for (const auto& o : packs)
        if (!find(host, o.id)) out.push_back(o.id + " " + o.version + ": you have it enabled, the host doesn't");
    if (out.empty()) {
        std::vector<std::string> ours, theirs;
        for (const auto& p : packs) ours.push_back(p.id);
        for (const auto& p : host.packs) theirs.push_back(p.id);
        if (ours != theirs) out.push_back("the same mods, in a different load order than the host's");
    }
    if (out.empty() && schemaHash != host.schemaHash)
        out.push_back("the components differ from the host's (a different game build?)");
    return out;
}

// ── Content ──────────────────────────────────────────────────────────────────

Content::Content(Scripts& scripts, Registry& registry, LuaEcs& luaEcs)
    : m_scripts(scripts), m_registry(registry), m_luaEcs(luaEcs) {}

void Content::error(const std::string& message) {
    ENG_LOG("[content] " << message);
    m_errors.push_back(message);
}

void Content::setBase(fs::path root, std::string version) {
    if (!m_packs.empty()) throw std::logic_error("Content: setBase twice");
    Pack base{"base", std::move(version), root, ScriptTrust::Base};
    base.env = m_scripts.addEnv("base", root, ScriptTrust::Base);
    base.hash = hashDirectory(root);
    m_packs.push_back(std::move(base));
}

bool Content::readManifest(const fs::path& file, ModInfo& out) {
    std::string where = file.parent_path().filename().string() + "/mod.lua";
    std::string code;
    if (!readFile(file, code)) {
        error(where + ": can't read it");
        return false;
    }
    lua_State* L = m_scripts.L();
    std::string chunkname = "@" + where;
    if (luaL_loadbufferx(L, code.data(), code.size(), chunkname.c_str(), "t") != LUA_OK) {
        error(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    lua_newtable(L);  // no globals at all: it's data
    lua_setupvalue(L, -2, 1);
    if (!m_scripts.call(0, 1)) {
        error(where + ": " + m_scripts.lastError());
        return false;
    }
    bool ok = true;
    auto fail = [&](const std::string& why) {
        error(where + ": " + why);
        ok = false;
    };
    auto str = [&](const char* key, std::string& dst, bool required) {
        int t = lua_getfield(L, -1, key);
        if (t == LUA_TSTRING) dst = lua_tostring(L, -1);
        else if (t != LUA_TNIL || required) fail(std::string("'") + key + "' should be a string");
        lua_pop(L, 1);
    };
    if (!lua_istable(L, -1)) {
        fail("should return a table");
    } else {
        str("id", out.id, true);
        str("version", out.version, true);
        str("name", out.name, false);
        if (ok && !validModId(out.id)) fail("id '" + out.id + "' should be lowercase letters, digits, _ or - (and not 'base')");
        if (out.version.size() > 64) fail("version is too long");
        if (out.name.empty()) out.name = out.id;
        if (lua_getfield(L, -1, "requires") == LUA_TTABLE) {
            for (lua_Integer i = 1; lua_rawgeti(L, -1, i) != LUA_TNIL; ++i) {
                if (lua_type(L, -1) == LUA_TSTRING) out.dependencies.emplace_back(lua_tostring(L, -1));
                else fail("requires should list mod ids");
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        } else if (!lua_isnil(L, -1)) {
            fail("requires should be a list of mod ids");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    out.root = file.parent_path();
    return ok;
}

std::vector<ModInfo> Content::discover(const fs::path& modsDir) {
    std::vector<ModInfo> mods;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return mods;
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(modsDir, ec))
        if (entry.is_directory(ec) && fs::is_regular_file(entry.path() / "mod.lua", ec)) dirs.push_back(entry.path());
    std::sort(dirs.begin(), dirs.end());  // so which of two duplicates wins doesn't depend on the filesystem
    for (const auto& dir : dirs) {
        ModInfo info;
        if (!readManifest(dir / "mod.lua", info)) continue;
        auto dup = std::find_if(mods.begin(), mods.end(), [&](const ModInfo& m) { return m.id == info.id; });
        if (dup != mods.end()) {
            error("mod id '" + info.id + "' is in both " + dup->root.filename().string() + " and " +
                  dir.filename().string() + "; using the first");
            continue;
        }
        mods.push_back(std::move(info));
    }
    std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) { return a.id < b.id; });
    return mods;
}

bool Content::enable(const std::vector<ModInfo>& available, const std::vector<std::string>& ids) {
    if (m_packs.empty()) throw std::logic_error("Content: enable before setBase");
    if (m_enabled) throw std::logic_error("Content: enable twice");
    m_enabled = true;
    bool all = true;
    for (const auto& id : ids) {
        auto it = std::find_if(available.begin(), available.end(), [&](const ModInfo& m) { return m.id == id; });
        if (it == available.end()) {
            error("mod '" + id + "' isn't installed; skipped");
            all = false;
            continue;
        }
        if (env(id) != kNoScriptEnv) {
            error("mod '" + id + "' is in the list twice; using the first");
            all = false;
            continue;
        }
        std::string missing;
        for (const auto& dep : it->dependencies)
            if (env(dep) == kNoScriptEnv) missing += (missing.empty() ? "" : ", ") + dep;
        if (!missing.empty()) {
            error("mod '" + id + "' needs " + missing + " enabled before it; skipped");
            all = false;
            continue;
        }
        Pack pack{it->id, it->version, it->root, ScriptTrust::Mod};
        pack.env = m_scripts.addEnv(it->id, it->root, ScriptTrust::Mod);
        pack.hash = hashDirectory(it->root);
        m_packs.push_back(std::move(pack));
    }
    return all;
}

ScriptEnv Content::env(std::string_view id) const {
    for (const auto& p : m_packs)
        if (p.id == id) return p.env;
    return kNoScriptEnv;
}

bool Content::failed(std::string_view id) const {
    for (const auto& p : m_packs)
        if (p.id == id) return p.failed;
    return false;
}

void Content::runFileIn(Pack& pack, const char* module) {
    if (pack.failed) return;
    std::error_code ec;
    // Only the pack's own file: a mod's require would fall back to base's.
    bool exists = fs::is_regular_file(pack.root / (std::string(module) + ".lua"), ec) ||
                  (pack.trust == ScriptTrust::Base && fs::is_regular_file(pack.root / (std::string(module) + ".luac"), ec));
    if (!exists) return;
    if (m_scripts.require(pack.env, module)) {
        lua_pop(m_scripts.L(), 1);
        return;
    }
    pack.failed = true;
    error(pack.id + ": " + module + ": " + m_scripts.lastError() + " (the pack is disabled)");
}

void Content::declareComponents() {
    if (m_registry.finalized()) throw std::logic_error("Content: declareComponents after Registry::finalize");
    for (auto& pack : m_packs) runFileIn(pack, "components");
}

void Content::runInit() {
    for (auto& pack : m_packs) runFileIn(pack, "init");
}

void Content::loadPrefabs(World& world) {
    if (!m_registry.finalized()) throw std::logic_error("Content: loadPrefabs before Registry::finalize");
    struct Merged {
        PrefabDef def;
        std::string from;
    };
    std::map<std::string, Merged> prefabs;  // sorted by name: the PrefabIds every peer agrees on
    lua_State* L = m_scripts.L();

    for (auto& pack : m_packs) {
        if (pack.failed) continue;
        fs::path dir = pack.root / "prefabs";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<std::string> names;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            fs::path ext = it->path().extension();
            if (ext != ".lua" && !(ext == ".luac" && pack.trust == ScriptTrust::Base)) continue;
            fs::path rel = fs::relative(it->path(), dir);
            names.push_back(rel.replace_extension().generic_string());
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());  // hound.lua + hound.luac

        for (const auto& name : names) {
            std::string where = pack.id + ": prefabs/" + name;
            if (!m_scripts.require(pack.env, "prefabs/" + name)) {
                error(where + ": " + m_scripts.lastError());
                continue;
            }
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                error(where + ": should return a table");
                continue;
            }
            PrefabDef def;
            if (!m_luaEcs.readPrefab(def)) {
                error(where + ": " + m_scripts.lastError());
                continue;
            }
            auto it = prefabs.find(name);
            if (!def.patch) {
                if (it != prefabs.end()) ENG_LOG("[content] " << pack.id << " replaces prefab '" << name << "' (from " << it->second.from << ")");
                prefabs[name] = {std::move(def), pack.id};
                continue;
            }
            if (it == prefabs.end()) {
                error(where + ": patches a prefab no earlier pack defines");
                continue;
            }
            // Patch: only the fields this file named, onto the earlier definition.
            PrefabDef& base = it->second.def;
            if (def.max) base.max = def.max;
            for (ComponentId c : def.removed)
                std::erase_if(base.components, [&](const PrefabDef::Component& x) { return x.id == c; });
            for (auto& comp : def.components) {
                auto existing = std::find_if(base.components.begin(), base.components.end(),
                                             [&](const PrefabDef::Component& x) { return x.id == comp.id; });
                if (existing == base.components.end()) {
                    base.components.push_back(std::move(comp));
                    continue;
                }
                const ComponentInfo& info = m_registry.info(comp.id);
                for (uint16_t fi : comp.fieldsSet) {
                    const FieldInfo& f = info.fields[fi];
                    std::memcpy(existing->bytes.data() + f.offset, comp.bytes.data() + f.offset, fieldTypeSize(f.type));
                }
            }
            std::sort(base.components.begin(), base.components.end(), [](auto& a, auto& b) { return a.id < b.id; });
            it->second.from += "+" + pack.id;
        }
    }

    for (const auto& [name, merged] : prefabs) {
        try {
            PrefabId p = world.definePrefab(name, merged.def.max);
            for (const auto& comp : merged.def.components) world.setPrefabComponent(p, comp.id, comp.bytes.data());
        } catch (const std::exception& e) {
            error("prefab '" + name + "': " + e.what());
        }
    }
}

ContentManifest Content::manifest() const {
    ContentManifest m;
    for (const auto& p : m_packs) m.packs.push_back({p.id, p.version, p.hash});
    m.schemaHash = m_registry.finalized() ? m_registry.schemaHash() : 0;
    return m;
}

}  // namespace eng::ecs
