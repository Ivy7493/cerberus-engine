#include "eng/ecs/LuaEcs.h"

#include "eng/script/Scripts.h"

#include <lua/lauxlib.h>
#include <lua/lua.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <vector>

namespace eng::ecs {

namespace {

char kSelfKey;  // registry[&kSelfKey] = this LuaEcs (lightuserdata), nil once destroyed
constexpr const char* kComponentMeta = "eng.ecs.Component";
constexpr const char* kVec2Meta = "eng.ecs.Vec2";

// A component of an entity, or (Vec2) one vec2 field of it.
struct Ref {
    Entity entity;
    ComponentId component;
    uint32_t offset;  // Vec2 refs: the field's offset
};

// C++ exceptions from World/Registry (logic_error, bad_alloc...) become Lua
// errors. Lua's own errors are a different exception type and pass through.
template <class F>
int protect(lua_State* L, F&& f) {
    std::string err;
    try {
        return f();
    } catch (const std::exception& e) {
        err = e.what();
    }
    return luaL_error(L, "%s", err.c_str());
}

}  // namespace

struct LuaEcsAccess {
    static LuaEcs& self(lua_State* L) {
        lua_rawgetp(L, LUA_REGISTRYINDEX, &kSelfKey);
        auto* p = static_cast<LuaEcs*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (!p) luaL_error(L, "the ECS bindings are gone");
        return *p;
    }
    static World& world(lua_State* L) {
        World* w = self(L).m_world;
        if (!w) luaL_error(L, "no world is attached to the scripts");
        return *w;
    }
    static Registry& registry(lua_State* L) { return self(L).m_registry; }

    // ── Arguments ──
    static Entity checkEntity(lua_State* L, int idx) {
        lua_Integer v = luaL_checkinteger(L, idx);
        if (v < 0 || v > lua_Integer(std::numeric_limits<uint32_t>::max())) luaL_argerror(L, idx, "not an entity");
        return static_cast<Entity>(v);
    }
    static ComponentId checkComponent(lua_State* L, int idx) {
        const Registry& reg = registry(L);
        if (!reg.finalized()) luaL_error(L, "components can't be used before the registry is finalized");
        if (lua_type(L, idx) == LUA_TNUMBER) {
            lua_Integer id = luaL_checkinteger(L, idx);
            if (id < 0 || size_t(id) >= reg.count()) luaL_argerror(L, idx, "no such component id");
            return ComponentId(id);
        }
        const char* name = luaL_checkstring(L, idx);
        ComponentId c = reg.find(name);
        if (c == kNoComponent) luaL_error(L, "no component named '%s'", name);
        return c;
    }

    // ── Fields ──
    static void pushRef(lua_State* L, Ref ref, const char* meta) {
        auto* r = static_cast<Ref*>(lua_newuserdatauv(L, sizeof(Ref), 0));
        *r = ref;
        luaL_setmetatable(L, meta);
    }
    static std::byte* resolve(lua_State* L, const Ref& ref) {
        void* p = world(L).get(ref.entity, ref.component);
        if (!p)
            luaL_error(L, "entity %d has no %s (destroyed, or the component was removed)", int(ref.entity),
                       registry(L).info(ref.component).name.c_str());
        return static_cast<std::byte*>(p);
    }

    template <class T>
    static void storeInt(lua_State* L, int idx, std::byte* at, const ComponentInfo& c, const FieldInfo& f) {
        lua_Integer v = luaL_checkinteger(L, idx);
        if (v < lua_Integer(std::numeric_limits<T>::min()) || v > lua_Integer(std::numeric_limits<T>::max()))
            luaL_error(L, "%s.%s: %d is out of range for %s", c.name.c_str(), f.name.c_str(), int(v), fieldTypeName(f.type));
        T t = static_cast<T>(v);
        std::memcpy(at, &t, sizeof(T));
    }
    template <class T>
    static void loadInt(lua_State* L, const std::byte* at) {
        T t;
        std::memcpy(&t, at, sizeof(T));
        lua_pushinteger(L, lua_Integer(t));
    }

    static glm::vec2 toVec2(lua_State* L, int idx, const ComponentInfo& c, const FieldInfo& f) {
        idx = lua_absindex(L, idx);
        if (auto* r = static_cast<Ref*>(luaL_testudata(L, idx, kVec2Meta))) {
            glm::vec2 v;
            std::memcpy(&v, resolve(L, *r) + r->offset, sizeof v);
            return v;
        }
        if (lua_istable(L, idx)) {
            glm::vec2 v;
            for (int i = 0; i < 2; ++i) {
                if (lua_getfield(L, idx, i ? "y" : "x") == LUA_TNIL) {
                    lua_pop(L, 1);
                    lua_rawgeti(L, idx, i + 1);
                }
                int isnum = 0;
                v[i] = float(lua_tonumberx(L, -1, &isnum));
                lua_pop(L, 1);
                if (!isnum) luaL_error(L, "%s.%s: a vec2 needs numbers x and y (or [1] and [2])", c.name.c_str(), f.name.c_str());
            }
            return v;
        }
        luaL_error(L, "%s.%s: expected a vec2 ({x=, y=}, {x, y} or a vec2 field), got %s", c.name.c_str(), f.name.c_str(),
                   luaL_typename(L, idx));
        return {};
    }

    static void pushField(lua_State* L, const Ref& ref, const FieldInfo& f, const std::byte* base) {
        const std::byte* at = base + f.offset;
        switch (f.type) {
            case FieldType::Bool: lua_pushboolean(L, *reinterpret_cast<const bool*>(at)); break;
            case FieldType::I8: loadInt<int8_t>(L, at); break;
            case FieldType::U8: loadInt<uint8_t>(L, at); break;
            case FieldType::I16: loadInt<int16_t>(L, at); break;
            case FieldType::U16: loadInt<uint16_t>(L, at); break;
            case FieldType::I32: loadInt<int32_t>(L, at); break;
            case FieldType::U32: loadInt<uint32_t>(L, at); break;
            case FieldType::Entity: loadInt<uint32_t>(L, at); break;
            case FieldType::F32: {
                float v;
                std::memcpy(&v, at, sizeof v);
                lua_pushnumber(L, v);
                break;
            }
            case FieldType::Vec2: pushRef(L, {ref.entity, ref.component, f.offset}, kVec2Meta); break;
        }
    }

    // Writes the Lua value at `idx` into field `f` of the component bytes at `base`.
    static void storeField(lua_State* L, int idx, const ComponentInfo& c, const FieldInfo& f, std::byte* base) {
        std::byte* at = base + f.offset;
        switch (f.type) {
            case FieldType::Bool:
                if (!lua_isboolean(L, idx)) luaL_error(L, "%s.%s: expected a boolean, got %s", c.name.c_str(), f.name.c_str(), luaL_typename(L, idx));
                *reinterpret_cast<bool*>(at) = lua_toboolean(L, idx);
                break;
            case FieldType::I8: storeInt<int8_t>(L, idx, at, c, f); break;
            case FieldType::U8: storeInt<uint8_t>(L, idx, at, c, f); break;
            case FieldType::I16: storeInt<int16_t>(L, idx, at, c, f); break;
            case FieldType::U16: storeInt<uint16_t>(L, idx, at, c, f); break;
            case FieldType::I32: storeInt<int32_t>(L, idx, at, c, f); break;
            case FieldType::U32: storeInt<uint32_t>(L, idx, at, c, f); break;
            case FieldType::Entity: storeInt<uint32_t>(L, idx, at, c, f); break;
            case FieldType::F32: {
                int isnum = 0;
                float v = float(lua_tonumberx(L, idx, &isnum));
                if (!isnum || lua_type(L, idx) != LUA_TNUMBER)
                    luaL_error(L, "%s.%s: expected a number, got %s", c.name.c_str(), f.name.c_str(), luaL_typename(L, idx));
                std::memcpy(at, &v, sizeof v);
                break;
            }
            case FieldType::Vec2: {
                glm::vec2 v = toVec2(L, idx, c, f);
                std::memcpy(at, &v, sizeof v);
                break;
            }
        }
    }

    static const FieldInfo& checkField(lua_State* L, const ComponentInfo& c, int keyIdx) {
        if (lua_type(L, keyIdx) != LUA_TSTRING) luaL_error(L, "%s: field names are strings, got %s", c.name.c_str(), luaL_typename(L, keyIdx));
        const char* key = lua_tostring(L, keyIdx);
        const FieldInfo* f = c.field(key);
        if (!f) luaL_error(L, "%s has no field '%s'", c.name.c_str(), key);
        return *f;
    }

    // { field = value, ... } at `tidx` into component bytes.
    static void fillFromTable(lua_State* L, int tidx, const ComponentInfo& c, std::byte* bytes) {
        tidx = lua_absindex(L, tidx);
        luaL_checktype(L, tidx, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, tidx)) {
            storeField(L, lua_gettop(L), c, checkField(L, c, -2), bytes);
            lua_pop(L, 1);
        }
    }

    // ── Component refs ──
    static int componentIndex(lua_State* L) {
        auto* ref = static_cast<Ref*>(luaL_checkudata(L, 1, kComponentMeta));
        const ComponentInfo& c = registry(L).info(ref->component);
        const FieldInfo& f = checkField(L, c, 2);
        pushField(L, *ref, f, resolve(L, *ref));
        return 1;
    }
    static int componentNewindex(lua_State* L) {
        auto* ref = static_cast<Ref*>(luaL_checkudata(L, 1, kComponentMeta));
        const ComponentInfo& c = registry(L).info(ref->component);
        const FieldInfo& f = checkField(L, c, 2);
        storeField(L, 3, c, f, resolve(L, *ref));
        return 0;
    }
    static int componentTostring(lua_State* L) {
        auto* ref = static_cast<Ref*>(luaL_checkudata(L, 1, kComponentMeta));
        lua_pushfstring(L, "%s@%d", registry(L).info(ref->component).name.c_str(), int(ref->entity));
        return 1;
    }
    static int vecAxis(lua_State* L, int keyIdx) {
        const char* k = lua_tostring(L, keyIdx);
        if (k && k[1] == '\0' && (k[0] == 'x' || k[0] == 'y')) return k[0] == 'y';
        return luaL_error(L, "a vec2 has only x and y");
    }
    static int vecIndex(lua_State* L) {
        auto* ref = static_cast<Ref*>(luaL_checkudata(L, 1, kVec2Meta));
        int axis = vecAxis(L, 2);
        float v;
        std::memcpy(&v, resolve(L, *ref) + ref->offset + axis * sizeof(float), sizeof v);
        lua_pushnumber(L, v);
        return 1;
    }
    static int vecNewindex(lua_State* L) {
        auto* ref = static_cast<Ref*>(luaL_checkudata(L, 1, kVec2Meta));
        int axis = vecAxis(L, 2);
        float v = float(luaL_checknumber(L, 3));
        std::memcpy(resolve(L, *ref) + ref->offset + axis * sizeof(float), &v, sizeof v);
        return 0;
    }
    static int vecTostring(lua_State* L) {
        auto* ref = static_cast<Ref*>(luaL_checkudata(L, 1, kVec2Meta));
        glm::vec2 v;
        std::memcpy(&v, resolve(L, *ref) + ref->offset, sizeof v);
        lua_pushfstring(L, "(%f, %f)", double(v.x), double(v.y));
        return 1;
    }

    // ── ecs.* ──
    static FieldType parseType(lua_State* L, const char* name, const char* field) {
        for (int t = int(FieldType::Bool); t <= int(FieldType::Entity); ++t)
            if (std::strcmp(fieldTypeName(FieldType(t)), name) == 0) return FieldType(t);
        luaL_error(L, "field '%s': unknown type '%s' (bool i8 u8 i16 u16 i32 u32 f32 vec2 entity)", field, name);
        return FieldType::I32;
    }
    // The field spec at `idx`: a type name or { type=, net=, scale=, replicated=, interpolated= }.
    static SchemaField parseSpec(lua_State* L, const std::string& name, int idx) {
        idx = lua_absindex(L, idx);
        SchemaField sf{name, FieldType::I32, {}};
        if (lua_type(L, idx) == LUA_TSTRING) {
            sf.type = parseType(L, lua_tostring(L, idx), name.c_str());
            return sf;
        }
        if (!lua_istable(L, idx)) luaL_error(L, "field '%s': expected a type name or a table", name.c_str());
        lua_getfield(L, idx, "type");
        if (lua_type(L, -1) != LUA_TSTRING) luaL_error(L, "field '%s': missing type", name.c_str());
        sf.type = parseType(L, lua_tostring(L, -1), name.c_str());
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "net") != LUA_TNIL) {
            std::string net = luaL_checkstring(L, -1);
            if (net == "raw") sf.options.net = NetEncoding::Raw;
            else if (net == "varint") sf.options.net = NetEncoding::Varint;
            else if (net == "fixed16") sf.options.net = NetEncoding::Fixed16;
            else luaL_error(L, "field '%s': net must be raw, varint or fixed16", name.c_str());
        }
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "scale") != LUA_TNIL) sf.options.scale = float(luaL_checknumber(L, -1));
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "replicated") != LUA_TNIL) sf.options.replicated = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "interpolated") != LUA_TNIL) sf.options.interpolated = lua_toboolean(L, -1);
        lua_pop(L, 1);
        return sf;
    }
    static int component(lua_State* L) {
        return protect(L, [&] {
            std::string name = luaL_checkstring(L, 1);
            luaL_checktype(L, 2, LUA_TTABLE);
            Registry& reg = registry(L);
            if (reg.finalized()) luaL_error(L, "component '%s' declared too late: the registry is finalized", name.c_str());
            std::vector<SchemaField> fields;
            lua_Integer n = luaL_len(L, 2);
            if (n > 0) {  // { {"name", spec}, ... }: in this order
                for (lua_Integer i = 1; i <= n; ++i) {
                    lua_rawgeti(L, 2, i);
                    if (!lua_istable(L, -1)) luaL_error(L, "component '%s': entry %d should be {name, type}", name.c_str(), int(i));
                    lua_rawgeti(L, -1, 1);
                    std::string fname = luaL_checkstring(L, -1);
                    lua_rawgeti(L, -2, 2);
                    fields.push_back(parseSpec(L, fname, -1));
                    lua_pop(L, 3);
                }
            } else {  // { name = spec, ... }: sorted by name, so the layout doesn't depend on hashing
                std::vector<std::string> names;
                lua_pushnil(L);
                while (lua_next(L, 2)) {
                    if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "component '%s': field names are strings", name.c_str());
                    names.emplace_back(lua_tostring(L, -2));
                    lua_pop(L, 1);
                }
                std::sort(names.begin(), names.end());
                for (const auto& fname : names) {
                    lua_getfield(L, 2, fname.c_str());
                    fields.push_back(parseSpec(L, fname, -1));
                    lua_pop(L, 1);
                }
            }
            reg.addSchema(name, fields);
            return 0;
        });
    }
    static int id(lua_State* L) {
        const Registry& reg = registry(L);
        ComponentId c = reg.finalized() ? reg.find(luaL_checkstring(L, 1)) : kNoComponent;
        if (c == kNoComponent) lua_pushnil(L);
        else lua_pushinteger(L, c);
        return 1;
    }
    static int spawn(lua_State* L) {
        return protect(L, [&] {
            World& w = world(L);
            PrefabId p = kNoPrefab;
            if (!lua_isnoneornil(L, 1)) {
                const char* name = luaL_checkstring(L, 1);
                p = w.findPrefab(name);
                if (p == kNoPrefab) luaL_error(L, "no prefab named '%s'", name);
            }
            Entity e = w.iterating() ? w.commands().spawn(p) : w.spawn(p);
            if (e == kNullEntity) lua_pushnil(L);
            else lua_pushinteger(L, e);
            return 1;
        });
    }
    static int destroy(lua_State* L) {
        return protect(L, [&] {
            World& w = world(L);
            Entity e = checkEntity(L, 1);
            if (w.iterating()) w.commands().destroy(e);
            else w.destroy(e);
            return 0;
        });
    }
    static int alive(lua_State* L) {
        lua_pushboolean(L, world(L).alive(checkEntity(L, 1)));
        return 1;
    }
    static int prefab(lua_State* L) {
        World& w = world(L);
        PrefabId p = w.prefabOf(checkEntity(L, 1));
        if (p == kNoPrefab) lua_pushnil(L);
        else lua_pushstring(L, w.prefabName(p).c_str());
        return 1;
    }
    static int add(lua_State* L) {
        return protect(L, [&] {
            World& w = world(L);
            Entity e = checkEntity(L, 1);
            ComponentId c = checkComponent(L, 2);
            const ComponentInfo& info = registry(L).info(c);
            if (!w.alive(e)) luaL_error(L, "ecs.add(%s): entity %d isn't alive", info.name.c_str(), int(e));
            std::vector<std::byte> bytes = info.defaults;
            if (!lua_isnoneornil(L, 3)) fillFromTable(L, 3, info, bytes.data());
            if (w.iterating()) w.commands().add(e, c, bytes.data());
            else w.add(e, c, bytes.data());
            pushRef(L, {e, c, 0}, kComponentMeta);
            return 1;
        });
    }
    static int remove(lua_State* L) {
        return protect(L, [&] {
            World& w = world(L);
            Entity e = checkEntity(L, 1);
            ComponentId c = checkComponent(L, 2);
            if (w.iterating()) w.commands().remove(e, c);
            else w.remove(e, c);
            return 0;
        });
    }
    static int has(lua_State* L) {
        lua_pushboolean(L, world(L).has(checkEntity(L, 1), checkComponent(L, 2)));
        return 1;
    }
    static int get(lua_State* L) {
        Entity e = checkEntity(L, 1);
        ComponentId c = checkComponent(L, 2);
        if (!world(L).has(e, c)) lua_pushnil(L);
        else pushRef(L, {e, c, 0}, kComponentMeta);
        return 1;
    }
    static int each(lua_State* L) {
        return protect(L, [&] {
            int n = lua_gettop(L);
            luaL_checktype(L, n, LUA_TFUNCTION);
            if (n < 2) luaL_error(L, "ecs.each needs at least one component before the function");
            std::vector<ComponentId> ids;
            for (int i = 1; i < n; ++i) ids.push_back(checkComponent(L, i));
            world(L).query(ids, [&](Entity e) {
                lua_pushvalue(L, n);
                lua_pushinteger(L, e);
                lua_call(L, 1, 0);
            });
            return 0;
        });
    }
    static int count(lua_State* L) {
        lua_pushinteger(L, lua_Integer(world(L).count(checkComponent(L, 1))));
        return 1;
    }
    static int size(lua_State* L) {
        lua_pushinteger(L, world(L).size());
        return 1;
    }
    static int capacity(lua_State* L) {
        lua_pushinteger(L, world(L).capacity());
        return 1;
    }

    // ── events.* ──
    static void pushHandlers(lua_State* L) { lua_rawgeti(L, LUA_REGISTRYINDEX, self(L).m_handlersRef); }
    static int on(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_Debug ar;
        lua_pushvalue(L, 2);
        lua_getinfo(L, ">S", &ar);  // the file that defines the handler
        pushHandlers(L);
        if (lua_getfield(L, -1, name) == LUA_TNIL) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, name);
        }
        lua_createtable(L, 0, 2);
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "fn");
        lua_pushstring(L, ar.source);
        lua_setfield(L, -2, "src");
        lua_rawseti(L, -2, lua_Integer(lua_rawlen(L, -2)) + 1);
        return 0;
    }
    static int emitLua(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        int nargs = lua_gettop(L) - 1;
        pushHandlers(L);
        if (lua_getfield(L, -1, name) != LUA_TTABLE) return 0;
        int list = lua_gettop(L);
        lua_Integer n = lua_Integer(lua_rawlen(L, list));  // handlers added meanwhile wait for the next emit
        for (lua_Integer i = 1; i <= n; ++i) {
            if (lua_rawgeti(L, list, i) != LUA_TTABLE) {
                lua_pop(L, 1);
                continue;
            }
            lua_getfield(L, -1, "fn");
            lua_remove(L, -2);
            for (int a = 2; a <= nargs + 1; ++a) lua_pushvalue(L, a);
            lua_call(L, nargs, 0);
        }
        return 0;
    }
    // Drops every handler defined in `chunkname` (before that file re-runs).
    static void dropHandlersFrom(LuaEcs& self, lua_State* L, const std::string& chunkname) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, self.m_handlersRef);
        int all = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, all)) {
            int list = lua_gettop(L);
            lua_Integer n = lua_Integer(lua_rawlen(L, list)), kept = 0;
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, list, i);
                lua_getfield(L, -1, "src");
                bool drop = lua_isstring(L, -1) && chunkname == lua_tostring(L, -1);
                lua_pop(L, 1);
                if (drop) lua_pop(L, 1);
                else lua_rawseti(L, list, ++kept);
            }
            for (lua_Integer i = kept + 1; i <= n; ++i) {
                lua_pushnil(L);
                lua_rawseti(L, list, i);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
};

LuaEcs::LuaEcs(Scripts& scripts, Registry& registry) : m_scripts(scripts), m_registry(registry) {
    using A = LuaEcsAccess;
    lua_State* L = scripts.L();
    lua_pushlightuserdata(L, this);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kSelfKey);

    lua_newtable(L);
    m_handlersRef = luaL_ref(L, LUA_REGISTRYINDEX);

    const luaL_Reg componentMeta[] = {{"__index", A::componentIndex},
                                      {"__newindex", A::componentNewindex},
                                      {"__tostring", A::componentTostring},
                                      {nullptr, nullptr}};
    const luaL_Reg vecMeta[] = {{"__index", A::vecIndex}, {"__newindex", A::vecNewindex}, {"__tostring", A::vecTostring}, {nullptr, nullptr}};
    for (auto [name, fns] : {std::pair{kComponentMeta, componentMeta}, std::pair{kVec2Meta, vecMeta}}) {
        luaL_newmetatable(L, name);
        luaL_setfuncs(L, fns, 0);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);
    }

    const luaL_Reg ecs[] = {
        {"component", A::component}, {"id", A::id},       {"spawn", A::spawn},   {"destroy", A::destroy},
        {"alive", A::alive},         {"prefab", A::prefab}, {"add", A::add},     {"remove", A::remove},
        {"has", A::has},             {"get", A::get},     {"each", A::each},     {"count", A::count},
        {"size", A::size},           {"capacity", A::capacity}, {nullptr, nullptr},
    };
    luaL_newlib(L, ecs);
    Scripts::makeReadOnly(L);
    scripts.exposeGlobal("ecs");

    const luaL_Reg events[] = {{"on", A::on}, {"emit", A::emitLua}, {nullptr, nullptr}};
    luaL_newlib(L, events);
    Scripts::makeReadOnly(L);
    scripts.exposeGlobal("events");

    scripts.addReloadListener([this](const std::string& chunkname) {
        LuaEcsAccess::dropHandlersFrom(*this, m_scripts.L(), chunkname);
    });
}

LuaEcs::~LuaEcs() {
    lua_State* L = m_scripts.L();
    lua_pushnil(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kSelfKey);
    luaL_unref(L, LUA_REGISTRYINDEX, m_handlersRef);
}

bool LuaEcs::emit(std::string_view name, int nargs) {
    lua_State* L = m_scripts.L();
    int args = lua_gettop(L) - nargs + 1;
    bool ok = true;
    lua_rawgeti(L, LUA_REGISTRYINDEX, m_handlersRef);
    lua_pushlstring(L, name.data(), name.size());
    if (lua_rawget(L, -2) == LUA_TTABLE) {
        int list = lua_gettop(L);
        lua_Integer n = lua_Integer(lua_rawlen(L, list));
        for (lua_Integer i = 1; i <= n; ++i) {
            if (lua_rawgeti(L, list, i) != LUA_TTABLE) {
                lua_pop(L, 1);
                continue;
            }
            lua_getfield(L, -1, "fn");
            lua_remove(L, -2);
            for (int a = 0; a < nargs; ++a) lua_pushvalue(L, args + a);
            ok = m_scripts.call(nargs, 0) && ok;
        }
    }
    lua_settop(L, args - 1);
    return ok;
}

}  // namespace eng::ecs
