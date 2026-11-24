// Private to the renderer: Renderer2D::Font's definition, shared by
// Renderer2D.cpp (which owns the fonts) and Text.cpp (which uses them).
#pragma once

#include "eng/render/Renderer2D.h"

#include <stb_truetype.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace eng {

// One rasterization of a font: every glyph of the baked ranges at one
// integer pixel size, in its own atlas texture.
struct Renderer2D::FontSize {
    struct Glyph {
        glm::vec2 off0{0.0f}, off1{0.0f};  // quad corners relative to the pen on the baseline, baked pixels
        glm::vec4 uv{0.0f};                // u0, v0, u1, v1
        float advance = 0.0f;
    };
    int px = 0;
    float scale = 0.0f;  // font units → baked pixels
    TextureId texture = 0;
    FontMetrics metrics;
    std::unordered_map<uint32_t, Glyph> glyphs;
    Glyph fallback;  // '?', for codepoints outside the baked ranges

    const Glyph& glyph(uint32_t cp) const {
        auto it = glyphs.find(cp);
        return it != glyphs.end() ? it->second : fallback;
    }
};

struct Renderer2D::Font {
    std::string path;
    std::vector<unsigned char> ttf;  // stb_truetype reads from this for the font's lifetime
    stbtt_fontinfo info{};
    std::vector<std::unique_ptr<FontSize>> sizes;
};

}  // namespace eng
