#include "eng/ecs/Registry.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace eng::ecs {

size_t fieldTypeSize(FieldType t) {
    switch (t) {
        case FieldType::Bool:
        case FieldType::I8:
        case FieldType::U8: return 1;
        case FieldType::I16:
        case FieldType::U16: return 2;
        case FieldType::I32:
        case FieldType::U32:
        case FieldType::F32:
        case FieldType::Entity: return 4;
        case FieldType::Vec2: return 8;
    }
    return 0;
}

const char* fieldTypeName(FieldType t) {
    switch (t) {
        case FieldType::Bool: return "bool";
        case FieldType::I8: return "i8";
        case FieldType::U8: return "u8";
        case FieldType::I16: return "i16";
        case FieldType::U16: return "u16";
        case FieldType::I32: return "i32";
        case FieldType::U32: return "u32";
        case FieldType::F32: return "f32";
        case FieldType::Vec2: return "vec2";
        case FieldType::Entity: return "entity";
    }
    return "?";
}

const FieldInfo* ComponentInfo::field(std::string_view n) const {
    for (const FieldInfo& f : fields)
        if (f.name == n) return &f;
    return nullptr;
}

void Registry::addInfo(ComponentInfo info, std::type_index type) {
    if (m_finalized) throw std::logic_error("Registry: add after finalize (" + info.name + ")");
    if (info.name.empty()) throw std::invalid_argument("Registry: a component needs a name");
    for (const ComponentInfo& c : m_components)
        if (c.name == info.name) throw std::invalid_argument("Registry: component '" + info.name + "' added twice");
    for (const FieldInfo& f : info.fields)
        if (f.offset + fieldTypeSize(f.type) > info.size)
            throw std::invalid_argument("Registry: field '" + f.name + "' lies outside '" + info.name + "'");
    m_components.push_back(std::move(info));
    m_types.push_back(type);
}

void Registry::addSchema(std::string name, const std::vector<SchemaField>& fields) {
    ComponentInfo info;
    info.name = std::move(name);
    info.fromSchema = true;
    uint32_t offset = 0, align = 1;
    for (const SchemaField& sf : fields) {
        if (info.field(sf.name)) throw std::invalid_argument("Registry: field '" + sf.name + "' twice in '" + info.name + "'");
        // Each field aligned to its own size (vec2 to 4: two floats).
        uint32_t a = sf.type == FieldType::Vec2 ? 4u : uint32_t(fieldTypeSize(sf.type));
        offset = (offset + a - 1) / a * a;
        info.fields.push_back({sf.name, sf.type, offset, sf.options});
        offset += uint32_t(fieldTypeSize(sf.type));
        align = std::max(align, a);
    }
    info.size = std::max(1u, (offset + align - 1) / align * align);  // never zero-sized (tags hold one byte)
    info.align = align;
    info.defaults.assign(info.size, std::byte{0});
    addInfo(std::move(info), std::type_index(typeid(void)));
}

void Registry::finalize() {
    if (m_finalized) return;
    std::vector<size_t> order(m_components.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return m_components[a].name < m_components[b].name; });
    std::vector<ComponentInfo> components;
    std::vector<std::type_index> types;
    for (size_t i : order) {
        components.push_back(std::move(m_components[i]));
        types.push_back(m_types[i]);
    }
    m_components = std::move(components);
    m_types = std::move(types);
    if (m_components.size() >= kNoComponent) throw std::length_error("Registry: too many components");
    for (size_t i = 0; i < m_types.size(); ++i)
        if (m_types[i] != std::type_index(typeid(void))) m_byType.emplace(m_types[i], ComponentId(i));
    m_finalized = true;
}

ComponentId Registry::find(std::string_view name) const {
    for (size_t i = 0; i < m_components.size(); ++i)
        if (m_components[i].name == name) return ComponentId(i);
    return kNoComponent;
}

uint64_t Registry::schemaHash() const {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= static_cast<const unsigned char*>(p)[i];
            h *= 1099511628211ull;
        }
    };
    auto str = [&](const std::string& s) { mix(s.data(), s.size() + 1); };  // with the terminator, so "ab"+"c" ≠ "a"+"bc"
    for (const ComponentInfo& c : m_components) {
        str(c.name);
        mix(&c.size, sizeof c.size);
        for (const FieldInfo& f : c.fields) {
            str(f.name);
            uint8_t t = uint8_t(f.type), net = uint8_t(f.options.net), rep = f.options.replicated, interp = f.options.interpolated;
            mix(&t, 1);
            mix(&f.offset, sizeof f.offset);
            mix(&net, 1);
            mix(&f.options.scale, sizeof f.options.scale);
            mix(&rep, 1);
            mix(&interp, 1);
        }
    }
    return h;
}

}  // namespace eng::ecs
