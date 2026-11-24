#pragma once

// eng::ecs::Registry — what components exist and what is in them. Everything
// that touches components generically (the Lua bridge, prefab loading, net
// snapshots, saves, an inspector) goes through these descriptions rather
// than through C++ types. See docs/ecs-lua-plan.md.
//
//   struct Health { int32_t hp; float regen; };
//   registry.add<Health>("Health", {field("hp", &Health::hp),
//                                   field("regen", &Health::regen, {.net = NetEncoding::Fixed16, .scale = 0.01f})});
//   registry.finalize();  // ids are assigned here, sorted by name
//
// Components are plain bytes: trivially copyable, no pointers to anything
// owned (store Entity handles, ids, indices). Lua/mod components declared
// from a schema (addSchema) get the same reflection, so they replicate and
// save like C++ ones.

#include "eng/ecs/Entity.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace eng::ecs {

using ComponentId = uint16_t;
constexpr ComponentId kNoComponent = 0xFFFF;

enum class FieldType : uint8_t { Bool, I8, U8, I16, U16, I32, U32, F32, Vec2, Entity };
size_t fieldTypeSize(FieldType t);
const char* fieldTypeName(FieldType t);

// How a field goes over the wire (when replicated).
enum class NetEncoding : uint8_t {
    Raw,      // its bytes as they are
    Varint,   // integers: small values in fewer bytes (zigzag for signed)
    Fixed16,  // floats (and vec2 components) as int16 of value / scale
};

struct FieldOptions {
    NetEncoding net = NetEncoding::Raw;
    float scale = 1.0f / 256.0f;  // Fixed16: the value of one step
    bool replicated = true;       // sent in snapshots
    bool interpolated = false;    // clients blend it between snapshots (F32 / Vec2)
};

struct FieldInfo {
    std::string name;
    FieldType type = FieldType::I32;
    uint32_t offset = 0;
    FieldOptions options;
};

struct ComponentInfo {
    std::string name;
    uint32_t size = 0;
    uint32_t align = 1;
    std::vector<FieldInfo> fields;
    std::vector<std::byte> defaults;  // a new component's bytes (zeroes unless given)
    bool fromSchema = false;          // declared at runtime (Lua/mods), not a C++ type
    const FieldInfo* field(std::string_view name) const;
};

namespace detail {
template <class M> constexpr FieldType fieldTypeOf() {
    if constexpr (std::is_same_v<M, bool>) return FieldType::Bool;
    else if constexpr (std::is_same_v<M, int8_t>) return FieldType::I8;
    else if constexpr (std::is_same_v<M, uint8_t>) return FieldType::U8;
    else if constexpr (std::is_same_v<M, int16_t>) return FieldType::I16;
    else if constexpr (std::is_same_v<M, uint16_t>) return FieldType::U16;
    else if constexpr (std::is_same_v<M, int32_t>) return FieldType::I32;
    else if constexpr (std::is_same_v<M, uint32_t>) return FieldType::U32;
    else if constexpr (std::is_same_v<M, float>) return FieldType::F32;
    else if constexpr (std::is_same_v<M, glm::vec2>) return FieldType::Vec2;
    else if constexpr (std::is_same_v<M, Entity>) return FieldType::Entity;
    else static_assert(sizeof(M) == 0, "unsupported component field type");
}
}  // namespace detail

// A reflected member of component C: field("hp", &Health::hp).
template <class C, class M>
FieldInfo field(std::string name, M C::*member, FieldOptions options = {}) {
    static_assert(std::is_standard_layout_v<C>, "components must be standard-layout");
    alignas(C) static unsigned char probe[sizeof(C)];
    const auto* c = reinterpret_cast<const C*>(probe);
    auto offset = static_cast<uint32_t>(reinterpret_cast<const unsigned char*>(&(c->*member)) - probe);
    return {std::move(name), detail::fieldTypeOf<M>(), offset, options};
}

// A field of a schema component (addSchema): its offset is laid out for it.
struct SchemaField {
    std::string name;
    FieldType type;
    FieldOptions options = {};
};

class Registry {
public:
    // A C++ component. Reflect the fields that scripts, the network or saves
    // need; unreflected bytes are still stored, copied and destroyed with it.
    // Ids exist only after finalize(): look them up then (id<T>() / find()).
    template <class T>
    void add(std::string name, std::initializer_list<FieldInfo> fields, const T& defaults = T{}) {
        static_assert(std::is_trivially_copyable_v<T>, "components are stored as bytes: trivially copyable only");
        ComponentInfo info;
        info.name = std::move(name);
        info.size = sizeof(T);
        info.align = alignof(T);
        info.fields.assign(fields.begin(), fields.end());
        info.defaults.resize(sizeof(T));
        std::memcpy(info.defaults.data(), &defaults, sizeof(T));
        addInfo(std::move(info), std::type_index(typeid(T)));
    }
    // A component declared at runtime (Lua / mods): fields laid out in order,
    // each aligned to its size. Zero-initialized.
    void addSchema(std::string name, const std::vector<SchemaField>& fields);

    // Ids are assigned here, sorted by name (never by registration order, so
    // the same set of components gets the same ids on every machine). No
    // components can be added after.
    void finalize();
    bool finalized() const { return m_finalized; }

    size_t count() const { return m_components.size(); }
    const ComponentInfo& info(ComponentId id) const { return m_components[id]; }
    ComponentId find(std::string_view name) const;  // kNoComponent if missing
    template <class T>
    ComponentId id() const {
        auto it = m_byType.find(std::type_index(typeid(T)));
        return it == m_byType.end() ? kNoComponent : it->second;
    }

    // FNV-1a over every component's name, size and fields (name, type,
    // offset, net encoding, replication). Peers must agree on it.
    uint64_t schemaHash() const;

private:
    void addInfo(ComponentInfo info, std::type_index type);

    std::vector<ComponentInfo> m_components;
    std::vector<std::type_index> m_types;  // parallel to m_components (typeid(void) for schemas)
    std::unordered_map<std::type_index, ComponentId> m_byType;
    bool m_finalized = false;
};

}  // namespace eng::ecs
