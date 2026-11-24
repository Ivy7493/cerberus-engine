// Private to the renderer: Renderer2D::Font's definition, shared by
// Renderer2D.cpp (which owns the fonts) and Text.cpp (which uses them).
#pragma once

#include "eng/render/Renderer2D.h"

#include <stb_truetype.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace eng {

// In StbTruetype.cpp (compiled optimized even in Debug).
void alphaToRgba(const uint8_t* alpha, uint8_t* rgba, size_t count);

// One rasterization of a font: every glyph of the baked ranges at one
// integer pixel size, in its own atlas texture.
struct Renderer2D::FontSize {
    struct Glyph {
        glm::vec2 off0{0.0f}, off1{0.0f};  // quad corners relative to the pen on the baseline, baked pixels
        glm::vec4 uv{0.0f};                // u0, v0, u1, v1
        float advance = 0.0f;
        int index = 0;                     // stb_truetype glyph index (for kerning)
    };
    int px = 0;
    float scale = 0.0f;  // font units → baked pixels
    TextureId texture = 0;
    FontMetrics metrics;
    std::unordered_map<uint32_t, Glyph> glyphs;
    Glyph fallback;  // '?', for codepoints outside the baked ranges
    // Kerning by glyph-index pair, in baked pixels: stb's GPOS lookup is far
    // too slow to repeat for every pair of every string every frame.
    mutable std::unordered_map<uint64_t, float> kerning;

    float kern(const stbtt_fontinfo& info, const Glyph& a, const Glyph& b) const {
        uint64_t key = (uint64_t(uint32_t(a.index)) << 32) | uint32_t(b.index);
        auto it = kerning.find(key);
        if (it != kerning.end()) return it->second;
        float k = float(stbtt_GetGlyphKernAdvance(&info, a.index, b.index)) * scale;
        kerning.emplace(key, k);
        return k;
    }

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
