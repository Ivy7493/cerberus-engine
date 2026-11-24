// Rounded rectangles and borders for UI (what Clay's RECTANGLE and BORDER
// render commands need). Plain triangles on the white texture, so they batch
// with everything else; no anti-aliasing.
#include "eng/render/Renderer2D.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace eng {

namespace {

constexpr float kHalfPi = 1.57079632679f;

// Arc segments for a corner of radius r (pixels): smooth at UI sizes without
// spending triangles on tiny corners.
int segmentsFor(float r) { return r <= 0.0f ? 0 : std::clamp(int(r * 0.35f) + 2, 2, 12); }

// Corners in clockwise order (y-down): top-left, top-right, bottom-right,
// bottom-left. `sign` points from the corner into the rectangle; `start` is
// the angle where that corner's arc begins.
struct Corner {
    glm::vec2 point;
    glm::vec2 sign;
    float start;
    float radius;
};

std::array<Corner, 4> corners(const Rect& r, const CornerRadii& radii) {
    // CSS-style clamp: no radius beyond half the shorter side.
    float limit = 0.5f * std::min(r.size.x, r.size.y);
    auto c = [&](float v) { return std::clamp(v, 0.0f, limit); };
    glm::vec2 a = r.pos, b = r.pos + r.size;
    return {{
        {{a.x, a.y}, {1, 1}, 2.0f * kHalfPi, c(radii.topLeft)},
        {{b.x, a.y}, {-1, 1}, 3.0f * kHalfPi, c(radii.topRight)},
        {{b.x, b.y}, {-1, -1}, 0.0f, c(radii.bottomRight)},
        {{a.x, b.y}, {1, -1}, kHalfPi, c(radii.bottomLeft)},
    }};
}

// Points along one corner's arc, centered at `center` with radii (rx, ry);
// zero radii collapse to the single point `center`.
void arc(std::vector<glm::vec2>& out, glm::vec2 center, float rx, float ry, float start, int segments) {
    if (segments == 0) {
        out.push_back(center);
        return;
    }
    for (int i = 0; i <= segments; ++i) {
        float t = start + kHalfPi * float(i) / float(segments);
        out.push_back(center + glm::vec2(std::cos(t) * rx, std::sin(t) * ry));
    }
}

}  // namespace

void Renderer2D::drawRoundedRect(const Rect& r, const CornerRadii& radii, const glm::vec4& color, Space space) {
    if (r.size.x <= 0.0f || r.size.y <= 0.0f || color.a <= 0.0f) return;
    auto cs = corners(r, radii);
    if (cs[0].radius == 0 && cs[1].radius == 0 && cs[2].radius == 0 && cs[3].radius == 0) {
        drawRect(r.pos + r.size * 0.5f, r.size, color, space);
        return;
    }
    std::vector<glm::vec2> outline;
    for (const Corner& c : cs) arc(outline, c.point + c.sign * c.radius, c.radius, c.radius, c.start, segmentsFor(c.radius));
    // Convex, so a fan from the center covers it.
    glm::vec2 center = r.pos + r.size * 0.5f;
    for (size_t i = 0; i < outline.size(); ++i) {
        glm::vec2 q[4] = {center, outline[i], outline[(i + 1) % outline.size()], center};
        pushQuad(q, color, space);
    }
}

void Renderer2D::drawRoundedBorder(const Rect& r, const CornerRadii& radii, const EdgeWidths& w,
                                   const glm::vec4& color, Space space) {
    if (r.size.x <= 0.0f || r.size.y <= 0.0f || color.a <= 0.0f) return;
    auto cs = corners(r, radii);
    // Each corner's inner arc shares its outer arc's segment count, so outer
    // and inner outlines pair up point for point and the ring is quads.
    // Inner arc: centered where the edges' inner lines meet the rounding,
    // radii shrunk by the adjoining widths (a sharp corner if they exceed it).
    float wx[4] = {w.left, w.right, w.right, w.left};
    float wy[4] = {w.top, w.top, w.bottom, w.bottom};
    std::vector<glm::vec2> outer, inner;
    for (int i = 0; i < 4; ++i) {
        const Corner& c = cs[i];
        int seg = segmentsFor(c.radius);
        arc(outer, c.point + c.sign * c.radius, c.radius, c.radius, c.start, seg);
        float ox = std::max(c.radius, wx[i]), oy = std::max(c.radius, wy[i]);
        glm::vec2 innerCenter = c.point + c.sign * glm::vec2(ox, oy);
        arc(inner, innerCenter, ox - wx[i], oy - wy[i], c.start, seg);
    }
    for (size_t i = 0; i < outer.size(); ++i) {
        size_t j = (i + 1) % outer.size();
        glm::vec2 q[4] = {outer[i], outer[j], inner[j], inner[i]};
        pushQuad(q, color, space);
    }
}

}  // namespace eng
