#include "eng/render/DrawList.h"

#include <algorithm>
#include <cmath>

namespace eng {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kAA = 1.0f;  // fringe width in pixels, as ImGui's AA size

glm::vec4 withAlpha(glm::vec4 c, float a) { return {c.r, c.g, c.b, a}; }

// ImGui's automatic circle tessellation: segments so the polygon strays at
// most ~0.3px from the true circle.
int circleSegments(float radius) {
    if (radius <= 0.0f) return 4;
    float err = std::min(0.3f, radius);
    int n = int(std::ceil(kPi / std::acos(1.0f - err / radius)));
    n = std::clamp(n, 4, 512);
    return (n + 1) & ~1;
}

glm::vec2 normalOf(glm::vec2 d) {
    float len2 = d.x * d.x + d.y * d.y;
    if (len2 <= 0.0f) return {0.0f, 0.0f};
    float inv = 1.0f / std::sqrt(len2);
    return {d.y * inv, -d.x * inv};
}

// Average of two unit normals, lengthened so offset edges stay parallel at
// corners (a miter), capped like ImGui so spikes stay bounded.
glm::vec2 miter(glm::vec2 n0, glm::vec2 n1) {
    glm::vec2 dm = (n0 + n1) * 0.5f;
    float d2 = dm.x * dm.x + dm.y * dm.y;
    if (d2 > 0.000001f) dm *= std::min(1.0f / d2, 100.0f);
    return dm;
}

float signedArea(const glm::vec2* p, int n) {
    float a = 0.0f;
    for (int i = 0, j = n - 1; i < n; j = i++) a += (p[j].x * p[i].y - p[i].x * p[j].y);
    return a * 0.5f;
}

}  // namespace

Color32 packColor(const glm::vec4& c) {
    auto ch = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return ENG_COL32(ch(c.r), ch(c.g), ch(c.b), ch(c.a));
}

glm::vec4 unpackColor(Color32 c) {
    return {float(c & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f, float((c >> 16) & 0xFF) / 255.0f,
            float((c >> 24) & 0xFF) / 255.0f};
}

// ── Primitives ───────────────────────────────────────────────────────────────

void DrawList::convexPolyFilled(const glm::vec2* p, int n, Color32 color) {
    if (n < 3 || (color >> 24) == 0) return;
    glm::vec4 c = unpackColor(color), t = withAlpha(c, 0.0f);
    // Edge normals must point out of the shape for the fringe to grow
    // outward; which way (dy, -dx) points depends on winding.
    float flip = signedArea(p, n) < 0.0f ? -1.0f : 1.0f;
    std::vector<glm::vec2>& edge = m_edge;
    edge.resize(size_t(n));
    for (int i = 0; i < n; ++i) edge[size_t(i)] = normalOf(p[(i + 1) % n] - p[i]) * -flip;
    std::vector<glm::vec2>& inner = m_inner;
    std::vector<glm::vec2>& outer = m_outer;
    inner.resize(size_t(n));
    outer.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        glm::vec2 dm = miter(edge[size_t((i + n - 1) % n)], edge[size_t(i)]) * (kAA * 0.5f);
        inner[size_t(i)] = p[i] - dm;
        outer[size_t(i)] = p[i] + dm;
    }
    for (int i = 1; i + 1 < n; ++i) m_r.drawTriangleColors({inner[0], inner[size_t(i)], inner[size_t(i + 1)]}, {c, c, c});
    for (int i = 0; i < n; ++i) {
        size_t j = size_t((i + 1) % n), k = size_t(i);
        m_r.drawQuadColors({inner[k], inner[j], outer[j], outer[k]}, {c, c, t, t});
    }
}

void DrawList::polyline(const glm::vec2* p, int n, Color32 color, bool closed, float thickness) {
    if (n < 2 || (color >> 24) == 0) return;
    glm::vec4 c = unpackColor(color);
    bool thick = thickness > kAA;
    if (!thick) c.a *= std::max(0.0f, thickness);  // hairlines fade rather than vanish
    glm::vec4 t = withAlpha(c, 0.0f);
    int segs = closed ? n : n - 1;

    std::vector<glm::vec2>& seg = m_seg;
    seg.resize(size_t(segs));
    for (int i = 0; i < segs; ++i) seg[size_t(i)] = normalOf(p[(i + 1) % n] - p[i]);
    std::vector<glm::vec2>& norm = m_norm;
    norm.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        if (!closed && i == 0) norm[0] = seg[0];
        else if (!closed && i == n - 1) norm[size_t(i)] = seg[size_t(segs - 1)];
        else norm[size_t(i)] = miter(seg[size_t((i + segs - 1) % segs)], seg[size_t(i % segs)]);
    }
    float half = thick ? (thickness - kAA) * 0.5f : 0.0f;
    for (int i = 0; i < segs; ++i) {
        size_t a = size_t(i), b = size_t((i + 1) % n);
        glm::vec2 na = norm[a], nb = norm[b];
        if (thick) {
            glm::vec2 ao = p[a] + na * (half + kAA), ai = p[a] + na * half, bi = p[b] + nb * half, bo = p[b] + nb * (half + kAA);
            glm::vec2 ao2 = p[a] - na * (half + kAA), ai2 = p[a] - na * half, bi2 = p[b] - nb * half, bo2 = p[b] - nb * (half + kAA);
            m_r.drawQuadColors({ao, bo, bi, ai}, {t, t, c, c});
            m_r.drawQuadColors({ai, bi, bi2, ai2}, {c, c, c, c});
            m_r.drawQuadColors({ai2, bi2, bo2, ao2}, {c, c, t, t});
        } else {
            glm::vec2 ao = p[a] + na * kAA, bo = p[b] + nb * kAA, ao2 = p[a] - na * kAA, bo2 = p[b] - nb * kAA;
            m_r.drawQuadColors({ao, bo, p[b], p[a]}, {t, t, c, c});
            m_r.drawQuadColors({p[a], p[b], bo2, ao2}, {c, c, t, t});
        }
    }
}

void DrawList::concavePolyFilled(const glm::vec2* p, int n, Color32 color) {
    if (n < 3 || (color >> 24) == 0) return;
    glm::vec4 c = unpackColor(color);
    // Ear clipping on a counter-clockwise copy (y-down: positive area).
    std::vector<int>& idx = m_idx;
    idx.resize(size_t(n));
    for (int i = 0; i < n; ++i) idx[size_t(i)] = i;
    if (signedArea(p, n) < 0.0f) std::reverse(idx.begin(), idx.end());
    auto cross = [](glm::vec2 a, glm::vec2 b, glm::vec2 q) { return (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x); };
    int guard = n * n;
    while (idx.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            glm::vec2 a = p[idx[(i + idx.size() - 1) % idx.size()]], b = p[idx[i]], q = p[idx[(i + 1) % idx.size()]];
            if (cross(a, b, q) <= 0.0f) continue;  // reflex corner
            bool inside = false;
            for (size_t k = 0; k < idx.size() && !inside; ++k) {
                glm::vec2 v = p[idx[k]];
                if (v == a || v == b || v == q) continue;
                inside = cross(a, b, v) >= 0.0f && cross(b, q, v) >= 0.0f && cross(q, a, v) >= 0.0f;
            }
            if (inside) continue;
            m_r.drawTriangleColors({a, b, q}, {c, c, c});
            idx.erase(idx.begin() + long(i));
            clipped = true;
            break;
        }
        if (!clipped) break;  // not a simple polygon; draw what's left as a fan
    }
    for (size_t i = 1; i + 1 < idx.size(); ++i) m_r.drawTriangleColors({p[idx[0]], p[idx[i]], p[idx[i + 1]]}, {c, c, c});
}

// ── Paths ────────────────────────────────────────────────────────────────────

void DrawList::pathArcTo(glm::vec2 center, float radius, float aMin, float aMax, int segments) {
    if (radius <= 0.0f) {
        m_path.push_back(center);
        return;
    }
    if (segments <= 0) {
        float frac = std::fabs(aMax - aMin) / (2.0f * kPi);
        segments = std::max(2, int(std::ceil(float(circleSegments(radius)) * frac)));
    }
    for (int i = 0; i <= segments; ++i) {
        float a = aMin + (aMax - aMin) * float(i) / float(segments);
        m_path.push_back(center + glm::vec2(std::cos(a), std::sin(a)) * radius);
    }
}

void DrawList::pathStroke(Color32 col, bool closed, float thickness) {
    polyline(m_path.data(), int(m_path.size()), col, closed, thickness);
    m_path.clear();
}

void DrawList::pathFillConvex(Color32 col) {
    convexPolyFilled(m_path.data(), int(m_path.size()), col);
    m_path.clear();
}

// ── Shapes ───────────────────────────────────────────────────────────────────

void DrawList::line(glm::vec2 a, glm::vec2 b, Color32 col, float thickness) {
    glm::vec2 pts[2] = {a + glm::vec2(0.5f), b + glm::vec2(0.5f)};  // pixel centers, as ImGui
    polyline(pts, 2, col, false, thickness);
}

static void roundedRectPath(std::vector<glm::vec2>& path, glm::vec2 a, glm::vec2 b, float r) {
    r = std::min(r, std::min(std::fabs(b.x - a.x), std::fabs(b.y - a.y)) * 0.5f);
    if (r <= 0.5f) {
        path.insert(path.end(), {a, {b.x, a.y}, b, {a.x, b.y}});
        return;
    }
    int seg = std::max(2, circleSegments(r) / 4);
    auto arc = [&](glm::vec2 c, float a0) {
        for (int i = 0; i <= seg; ++i) {
            float t = a0 + 0.5f * kPi * float(i) / float(seg);
            path.push_back(c + glm::vec2(std::cos(t), std::sin(t)) * r);
        }
    };
    arc({a.x + r, a.y + r}, kPi);
    arc({b.x - r, a.y + r}, 1.5f * kPi);
    arc({b.x - r, b.y - r}, 0.0f);
    arc({a.x + r, b.y - r}, 0.5f * kPi);
}

void DrawList::rect(glm::vec2 a, glm::vec2 b, Color32 col, float rounding, float thickness) {
    m_path.clear();
    roundedRectPath(m_path, a + glm::vec2(0.5f), b - glm::vec2(0.5f), rounding);
    pathStroke(col, true, thickness);
}

void DrawList::rectFilled(glm::vec2 a, glm::vec2 b, Color32 col, float rounding) {
    if ((col >> 24) == 0) return;
    if (rounding < 0.5f) {  // hard-edged, as ImGui's PrimRect
        glm::vec4 c = unpackColor(col);
        m_r.drawQuadColors({a, {b.x, a.y}, b, {a.x, b.y}}, {c, c, c, c});
        return;
    }
    m_path.clear();
    roundedRectPath(m_path, a, b, rounding);
    pathFillConvex(col);
}

void DrawList::rectFilledMultiColor(glm::vec2 a, glm::vec2 b, Color32 ul, Color32 ur, Color32 br, Color32 bl) {
    m_r.drawQuadColors({a, {b.x, a.y}, b, {a.x, b.y}}, {unpackColor(ul), unpackColor(ur), unpackColor(br), unpackColor(bl)});
}

void DrawList::quad(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4, Color32 col, float thickness) {
    glm::vec2 pts[4] = {p1, p2, p3, p4};
    polyline(pts, 4, col, true, thickness);
}

void DrawList::quadFilled(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4, Color32 col) {
    glm::vec2 pts[4] = {p1, p2, p3, p4};
    convexPolyFilled(pts, 4, col);
}

void DrawList::triangle(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, Color32 col, float thickness) {
    glm::vec2 pts[3] = {p1, p2, p3};
    polyline(pts, 3, col, true, thickness);
}

void DrawList::triangleFilled(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, Color32 col) {
    glm::vec2 pts[3] = {p1, p2, p3};
    convexPolyFilled(pts, 3, col);
}

void DrawList::circle(glm::vec2 c, float radius, Color32 col, int segments, float thickness) {
    if (radius <= 0.0f) return;
    m_path.clear();
    int n = segments > 2 ? segments : circleSegments(radius);
    float r = radius - 0.5f;  // as ImGui: the stroke sits inside the radius
    for (int i = 0; i < n; ++i) {
        float a = 2.0f * kPi * float(i) / float(n);
        m_path.push_back(c + glm::vec2(std::cos(a), std::sin(a)) * r);
    }
    pathStroke(col, true, thickness);
}

void DrawList::circleFilled(glm::vec2 c, float radius, Color32 col, int segments) {
    if (radius <= 0.0f) return;
    m_path.clear();
    int n = segments > 2 ? segments : circleSegments(radius);
    for (int i = 0; i < n; ++i) {
        float a = 2.0f * kPi * float(i) / float(n);
        m_path.push_back(c + glm::vec2(std::cos(a), std::sin(a)) * radius);
    }
    pathFillConvex(col);
}

void DrawList::ellipseFilled(glm::vec2 c, glm::vec2 radii, Color32 col, float rotation, int segments) {
    if (radii.x <= 0.0f || radii.y <= 0.0f) return;
    m_path.clear();
    int n = segments > 2 ? segments : circleSegments(std::max(radii.x, radii.y));
    float cr = std::cos(rotation), sr = std::sin(rotation);
    for (int i = 0; i < n; ++i) {
        float a = 2.0f * kPi * float(i) / float(n);
        glm::vec2 e{std::cos(a) * radii.x, std::sin(a) * radii.y};
        m_path.push_back(c + glm::vec2(e.x * cr - e.y * sr, e.x * sr + e.y * cr));
    }
    pathFillConvex(col);
}

void DrawList::bezierQuadratic(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, Color32 col, float thickness, int segments) {
    if (segments <= 0) segments = std::clamp(int((glm::length(p2 - p1) + glm::length(p3 - p2)) * 0.25f), 4, 64);
    m_path.clear();
    for (int i = 0; i <= segments; ++i) {
        float t = float(i) / float(segments), u = 1.0f - t;
        m_path.push_back(p1 * (u * u) + p2 * (2.0f * u * t) + p3 * (t * t));
    }
    pathStroke(col, false, thickness);
}

void DrawList::bezierCubic(glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, glm::vec2 p4, Color32 col, float thickness, int segments) {
    if (segments <= 0)
        segments = std::clamp(int((glm::length(p2 - p1) + glm::length(p3 - p2) + glm::length(p4 - p3)) * 0.25f), 4, 64);
    m_path.clear();
    for (int i = 0; i <= segments; ++i) {
        float t = float(i) / float(segments), u = 1.0f - t;
        m_path.push_back(p1 * (u * u * u) + p2 * (3.0f * u * u * t) + p3 * (3.0f * u * t * t) + p4 * (t * t * t));
    }
    pathStroke(col, false, thickness);
}

// ── Text, images, clipping ───────────────────────────────────────────────────

std::vector<std::string_view> DrawList::wrapLines(FontId f, float px, std::string_view s, float wrapWidth) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string_view para = s.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (wrapWidth <= 0.0f) {
            lines.push_back(para);
        } else {
            // Greedy: keep adding words while the line fits.
            size_t lineStart = 0;
            while (lineStart < para.size() || (lineStart == 0 && para.empty())) {
                size_t end = lineStart, best = std::string_view::npos;
                while (true) {
                    size_t sp = para.find(' ', end);
                    size_t candidate = sp == std::string_view::npos ? para.size() : sp;
                    if (m_r.measureText(f, px, para.substr(lineStart, candidate - lineStart)).x > wrapWidth &&
                        best != std::string_view::npos)
                        break;
                    best = candidate;
                    if (sp == std::string_view::npos) break;
                    end = sp + 1;
                }
                lines.push_back(para.substr(lineStart, best - lineStart));
                lineStart = best + 1;
                if (para.empty()) break;
            }
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return lines;
}

void DrawList::text(FontId f, float px, glm::vec2 pos, Color32 col, std::string_view s, float wrapWidth) {
    if ((col >> 24) == 0 || s.empty()) return;
    f = font(f);
    px = size(px);
    glm::vec4 c = unpackColor(col);
    if (wrapWidth <= 0.0f) {
        m_r.drawText(f, px, pos, c, s);
        return;
    }
    float lh = m_r.fontMetrics(f, px).lineHeight;
    for (std::string_view line : wrapLines(f, px, s, wrapWidth)) {
        m_r.drawText(f, px, pos, c, line);
        pos.y += lh;
    }
}

glm::vec2 DrawList::textSize(FontId f, float px, std::string_view s, float wrapWidth) {
    f = font(f);
    px = size(px);
    if (wrapWidth <= 0.0f) return m_r.measureText(f, px, s);
    float w = 0.0f;
    auto lines = wrapLines(f, px, s, wrapWidth);
    for (std::string_view line : lines) w = std::max(w, m_r.measureText(f, px, line).x);
    return {w, float(lines.size()) * m_r.fontMetrics(f, px).lineHeight};
}

void DrawList::image(TextureId texture, glm::vec2 a, glm::vec2 b, glm::vec2 uv0, glm::vec2 uv1, Color32 tint) {
    if ((tint >> 24) == 0) return;
    Sprite s;
    s.texture = texture;
    s.pos = (a + b) * 0.5f;
    s.size = b - a;
    s.uv = {uv0.x, uv0.y, uv1.x, uv1.y};
    s.color = unpackColor(tint);
    s.space = Space::Screen;
    m_r.draw(s);
}

void DrawList::pushClipRect(glm::vec2 a, glm::vec2 b) { m_r.pushClip({a, b - a}); }

void DrawList::popClipRect() { m_r.popClip(); }

}  // namespace eng
