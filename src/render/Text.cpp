// Text rendering for Renderer2D: TrueType fonts via stb_truetype, one glyph
// atlas per (font, integer pixel size), glyphs drawn as ordinary sprites so
// text batches and clips like everything else.
#include "Font.h"

#include "eng/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <fstream>
#include <iterator>
#include <string_view>

namespace eng {

namespace {

// What gets rasterized for each size. Latin-1 plus the punctuation, arrows
// and math signs game text uses; anything else draws as '?'.
struct CodepointRange {
    int first, count;
};
constexpr CodepointRange kRanges[] = {
    {0x0020, 0x5F},  // ASCII printable
    {0x00A0, 0x60},  // Latin-1 supplement (°, ±, ×, ÷, accented letters)
    {0x2010, 0x18},  // dashes, curly quotes, bullet, ellipsis
    {0x2122, 1},     // ™
    {0x2190, 4},     // ← ↑ → ↓
    {0x2212, 1},     // − (minus)
    {0x2248, 1},     // ≈
    {0x2260, 6},     // ≠ ≡ ≢ ≣ ≤ ≥
};

constexpr int kMaxBakePx = 400;  // past this a zoomed-in world label isn't worth an atlas

// The pixel sizes atlases are baked at: every size to 24 (ordinary UI text
// is drawn at exactly its size, crisp), then rungs at most 25% apart. Text
// at any other size draws from the next rung up, scaled down (bilinear).
// Baking costs milliseconds (a GPU upload stall included), so text whose
// size animates or follows the camera zoom must not bake a new atlas for
// every pixel size it passes through.
constexpr int kRungs[] = {28, 32, 40, 48, 64, 80, 96, 128, 160, 200, 256, 320, 400};

int bakeSize(float px) {
    int p = int(std::ceil(px - 0.01f));
    if (p <= 24) return std::max(p, 1);
    for (int r : kRungs)
        if (r >= p) return r;
    return kMaxBakePx;
}

constexpr int kMaxAtlas = 4096;

// Next codepoint from UTF-8 at `i` (advanced past it). Malformed bytes
// decode as U+FFFD one byte at a time, so bad input can't stall the loop.
uint32_t nextCodepoint(std::string_view s, size_t& i) {
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = byte(i);
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + len > s.size()) {
        ++i;
        return 0xFFFD;
    }
    uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1Fu) : len == 3 ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < len; ++k) {
        unsigned char cc = byte(i + k);
        if ((cc >> 6) != 0x2) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    i += len;
    return cp;
}

FontMetrics metricsFor(const stbtt_fontinfo& info, float scale) {
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    FontMetrics m;
    m.ascent = float(ascent) * scale;
    m.descent = float(-descent) * scale;
    m.lineHeight = float(ascent - descent + gap) * scale;
    return m;
}

}  // namespace

FontId Renderer2D::loadFont(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ENG_LOG("[render] failed to open font '" << path << "'");
        return kNoFont;
    }
    auto font = std::make_unique<Font>();
    font->path = path;
    font->ttf.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    int offset = font->ttf.empty() ? -1 : stbtt_GetFontOffsetForIndex(font->ttf.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font->info, font->ttf.data(), offset)) {
        ENG_LOG("[render] '" << path << "' is not a font stb_truetype can read");
        return kNoFont;
    }
    m_fonts.push_back(std::move(font));
    return static_cast<FontId>(m_fonts.size() - 1);
}

const Renderer2D::FontSize* Renderer2D::fontAtSize(FontId id, int px) {
    if (id >= m_fonts.size()) return nullptr;
    Font& font = *m_fonts[id];
    px = std::clamp(px, 1, kMaxBakePx);
    for (const auto& s : font.sizes)
        if (s->px == px) return s.get();

    auto size = std::make_unique<FontSize>();
    size->px = px;
    size->scale = stbtt_ScaleForPixelHeight(&font.info, float(px));
    size->metrics = metricsFor(font.info, size->scale);

    // Pack every range, doubling the atlas until it fits.
    constexpr size_t kRangeCount = std::size(kRanges);
    std::vector<stbtt_packedchar> packed[kRangeCount];
    stbtt_pack_range ranges[kRangeCount]{};
    for (size_t r = 0; r < kRangeCount; ++r) {
        packed[r].resize(size_t(kRanges[r].count));
        ranges[r].font_size = float(px);
        ranges[r].first_unicode_codepoint_in_range = kRanges[r].first;
        ranges[r].num_chars = kRanges[r].count;
        ranges[r].chardata_for_range = packed[r].data();
    }
    // Large glyphs don't need sub-pixel placement, so they skip the 2x
    // horizontal oversampling (half the atlas area to rasterize and upload).
    int oversample = px < 32 ? 2 : 1;
    // Start near the size it will need (glyph count × cell area, with
    // slack) rather than failing up from 256.
    int glyphCount = 0;
    for (const auto& r : kRanges) glyphCount += r.count;
    float area = float(glyphCount) * float(px) * float(px) * 0.75f * float(oversample);
    int dim = 256;
    while (dim < kMaxAtlas && float(dim) * float(dim) < area) dim *= 2;
    // Plain buffers, not std::vector: a vector's zero-fill loop over 4–64MB
    // runs unoptimized in Debug builds and was most of a bake's time.
    std::unique_ptr<unsigned char[]> alpha;
    for (;; dim *= 2) {
        alpha.reset(new unsigned char[size_t(dim) * size_t(dim)]);
        std::memset(alpha.get(), 0, size_t(dim) * size_t(dim));
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, alpha.get(), dim, dim, 0, 1, nullptr)) break;
        // 2x horizontally + bilinear sampling = sub-pixel glyph placement, so
        // small text keeps even spacing instead of each glyph rounding to a
        // whole pixel (what Dear ImGui does too). Rows stay pixel-aligned.
        stbtt_PackSetOversampling(&pc, unsigned(oversample), 1);
        bool ok = stbtt_PackFontRanges(&pc, font.ttf.data(), 0, ranges, int(kRangeCount)) != 0;
        stbtt_PackEnd(&pc);
        if (ok || dim >= kMaxAtlas) {
            if (!ok) ENG_LOG("[render] font '" << font.path << "' at " << px << "px overflows a " << dim << " atlas");
            break;
        }
    }

    size_t pixels = size_t(dim) * size_t(dim);
    std::unique_ptr<uint8_t[]> rgba(new uint8_t[pixels * 4]);
    alphaToRgba(alpha.get(), rgba.get(), pixels);
    size->texture = createTexture(rgba.get(), uint32_t(dim), uint32_t(dim), /*linear=*/true);

    float inv = 1.0f / float(dim);
    for (size_t r = 0; r < kRangeCount; ++r) {
        for (int i = 0; i < kRanges[r].count; ++i) {
            const stbtt_packedchar& c = packed[r][size_t(i)];
            FontSize::Glyph g;
            g.off0 = {c.xoff, c.yoff};
            g.off1 = {c.xoff2, c.yoff2};
            g.uv = {c.x0 * inv, c.y0 * inv, c.x1 * inv, c.y1 * inv};
            g.advance = c.xadvance;
            g.index = stbtt_FindGlyphIndex(&font.info, kRanges[r].first + i);
            size->glyphs.emplace(uint32_t(kRanges[r].first + i), g);
        }
    }
    size->fallback = size->glyph('?');

    font.sizes.push_back(std::move(size));
    return font.sizes.back().get();
}

void Renderer2D::prewarmFont(FontId font, float upToPixelSize) {
    if (font >= m_fonts.size()) return;
    int top = bakeSize(upToPixelSize);
    for (int px = 6; px <= std::min(top, 24); ++px) fontAtSize(font, px);
    for (int r : kRungs)
        if (r <= top) fontAtSize(font, r);
}

FontMetrics Renderer2D::fontMetrics(FontId id, float pixelSize) {
    if (id >= m_fonts.size()) return {};
    const Font& font = *m_fonts[id];
    return metricsFor(font.info, stbtt_ScaleForPixelHeight(&font.info, pixelSize));
}

glm::vec2 Renderer2D::measureText(FontId id, float pixelSize, std::string_view text, float letterSpacing) {
    if (id >= m_fonts.size() || pixelSize <= 0.0f) return {0.0f, 0.0f};
    // Measured at the size it will be drawn at (screen pixels), then scaled
    // back if that was rounded, so layout agrees with drawText.
    int px = bakeSize(pixelSize);
    const FontSize* size = fontAtSize(id, px);
    const stbtt_fontinfo& info = m_fonts[id]->info;
    float k = pixelSize / float(px);

    float widest = 0.0f, pen = 0.0f;
    int lines = 1;
    const FontSize::Glyph* prev = nullptr;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = nextCodepoint(text, i);
        if (cp == '\n') {
            widest = std::max(widest, pen);
            pen = 0.0f;
            prev = nullptr;
            ++lines;
            continue;
        }
        const FontSize::Glyph& g = size->glyph(cp);
        if (prev) pen += size->kern(info, *prev, g) + letterSpacing / k;
        pen += g.advance;
        prev = &g;
    }
    widest = std::max(widest, pen);
    return {widest * k, float(lines) * size->metrics.lineHeight * k};
}

void Renderer2D::drawText(FontId id, float pixelSize, const glm::vec2& topLeft, const glm::vec4& color,
                          std::string_view text, Space space, float letterSpacing) {
    if (id >= m_fonts.size() || pixelSize <= 0.0f || color.a <= 0.0f || text.empty()) return;
    // Rasterize at the on-screen size; world text scales back to world units.
    float pixelsPerUnit = space == Space::World ? m_pixelsPerUnit : 1.0f;
    int px = bakeSize(pixelSize * pixelsPerUnit);
    const FontSize* size = fontAtSize(id, px);
    const stbtt_fontinfo& info = m_fonts[id]->info;
    float k = pixelSize / float(px);  // drawing units per baked pixel
    // Screen text sits on whole-pixel rows (crisp horizontals); x stays
    // fractional, which the oversampled, bilinear atlas handles.
    auto snapped = [&](float v) { return space == Space::Screen ? std::round(v) : v; };

    float lineTop = topLeft.y;
    float pen = 0.0f;  // baked pixels from the line start
    const FontSize::Glyph* prev = nullptr;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = nextCodepoint(text, i);
        if (cp == '\n') {
            lineTop += size->metrics.lineHeight * k;
            pen = 0.0f;
            prev = nullptr;
            continue;
        }
        const FontSize::Glyph& g = size->glyph(cp);
        if (prev) pen += size->kern(info, *prev, g) + letterSpacing / k;
        if (g.off1.x > g.off0.x) {  // spaces have no quad
            glm::vec2 origin{topLeft.x + pen * k, snapped(lineTop + size->metrics.ascent * k)};
            glm::vec2 p0 = origin + g.off0 * k, p1 = origin + g.off1 * k;
            Sprite s;
            s.texture = size->texture;
            s.pos = (p0 + p1) * 0.5f;
            s.size = p1 - p0;
            s.uv = g.uv;
            s.color = color;
            s.space = space;
            draw(s);
        }
        pen += g.advance;
        prev = &g;
    }
}

}  // namespace eng
