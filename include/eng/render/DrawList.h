#pragma once

// eng::DrawList — immediate 2D vector drawing in screen pixels on top of
// Renderer2D, shaped like Dear ImGui's ImDrawList so code written against
// that ports by renaming (AddLine → line, IM_COL32 → ENG_COL32, ImVec2 →
// glm::vec2). Shapes are anti-aliased the way ImGui does it: a one-pixel
// fringe fading to transparent around fills and strokes; axis-aligned
// rectangles without rounding stay hard-edged, as in ImGui.
//
// Everything is emitted into the renderer's stream at call time, so it
// layers with sprites in call order. Colors are packed 32-bit RGBA (red in
// the low byte), the same layout as ImGui's ImU32.

#include "eng/render/Renderer2D.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

#define ENG_COL32(R, G, B, A) \
    ((uint32_t(uint8_t(A)) << 24) | (uint32_t(uint8_t(B)) << 16) | (uint32_t(uint8_t(G)) << 8) | uint32_t(uint8_t(R)))

namespace eng {

using Color32 = uint32_t;

Color32 packColor(const glm::vec4& c);  // components 0–1, clamped
glm::vec4 unpackColor(Color32 c);

class DrawList {
public:
    explicit DrawList(Renderer2D& renderer) : m_r(renderer) {}

    // Font and size for text() calls that don't name one (font kNoFont / size 0).
    void setDefaultFont(FontId font, float pixelSize) {
        m_font = font;
        m_fontSize = pixelSize;
    }
    FontId defaultFont() const { return m_font; }
    float defaultFontSize() const { return m_fontSize; }

    void line(glm::vec2 a, glm::vec2 b, Color32 col, float thickness = 1.0f);
    void rect(glm::vec2 min, glm::vec2 max, Color32 col, float rounding = 0.0f, float thickness = 1.0f);
    void rectFilled(glm::vec2 min, glm::vec2 max, Color32 col, float rounding = 0.0f);
    void rectFilledMultiColor(glm::vec2 min, glm::vec2 max, Color32 upperLeft, Color32 upperRight, Color32 bottomRight,
                              Color32 bottomLeft);
    void quad(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4, Color32 col, float thickness = 1.0f);
    void quadFilled(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4, Color32 col);
    void triangle(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, Color32 col, float thickness = 1.0f);
    void triangleFilled(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, Color32 col);
    // segments 0 = automatic (by radius, like ImGui).
    void circle(glm::vec2 center, float radius, Color32 col, int segments = 0, float thickness = 1.0f);
    void circleFilled(glm::vec2 center, float radius, Color32 col, int segments = 0);
    void ellipseFilled(glm::vec2 center, glm::vec2 radii, Color32 col, float rotation = 0.0f, int segments = 0);
    void polyline(const glm::vec2* points, int count, Color32 col, bool closed, float thickness);
    void convexPolyFilled(const glm::vec2* points, int count, Color32 col);
    void concavePolyFilled(const glm::vec2* points, int count, Color32 col);  // ear clipping, not anti-aliased
    void bezierQuadratic(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, Color32 col, float thickness, int segments = 0);
    void bezierCubic(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4, Color32 col, float thickness, int segments = 0);

    // Path building, as ImGui's Path* (angles in radians, y-down).
    void pathClear() { m_path.clear(); }
    void pathLineTo(glm::vec2 p) { m_path.push_back(p); }
    void pathArcTo(glm::vec2 center, float radius, float aMin, float aMax, int segments = 0);
    void pathStroke(Color32 col, bool closed = false, float thickness = 1.0f);
    void pathFillConvex(Color32 col);

    // Text: top-left of the first line at `pos`. wrapWidth > 0 wraps on
    // spaces. font kNoFont / pixelSize 0 = the defaults.
    void text(FontId font, float pixelSize, glm::vec2 pos, Color32 col, std::string_view s, float wrapWidth = 0.0f);
    void text(glm::vec2 pos, Color32 col, std::string_view s) { text(kNoFont, 0.0f, pos, col, s); }
    glm::vec2 textSize(FontId font, float pixelSize, std::string_view s, float wrapWidth = 0.0f);
    glm::vec2 textSize(std::string_view s) { return textSize(kNoFont, 0.0f, s); }

    void image(TextureId texture, glm::vec2 min, glm::vec2 max, glm::vec2 uv0 = {0, 0}, glm::vec2 uv1 = {1, 1},
               Color32 tint = ENG_COL32(255, 255, 255, 255));

    // Clip everything until popClipRect (intersects the current clip).
    void pushClipRect(glm::vec2 min, glm::vec2 max);
    void popClipRect();

    Renderer2D& renderer() { return m_r; }

private:
    FontId font(FontId f) const { return f == kNoFont ? m_font : f; }
    float size(float s) const { return s > 0.0f ? s : m_fontSize; }
    // Lines of `s` broken to fit `wrapWidth` (whole string when <= 0).
    std::vector<std::string_view> wrapLines(FontId f, float px, std::string_view s, float wrapWidth);

    Renderer2D& m_r;
    FontId m_font = kNoFont;
    float m_fontSize = 16.0f;
    std::vector<glm::vec2> m_path, m_scratch;
};

}  // namespace eng
