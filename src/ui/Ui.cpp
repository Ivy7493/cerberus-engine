#include "eng/ui/Ui.h"

#include "eng/core/Log.h"
#include "eng/platform/Input.h"
#include "eng/platform/Window.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace eng::ui {

namespace {

Context* s_current = nullptr;

constexpr size_t kChunkSize = 64 * 1024;
constexpr float kWheelPixels = 4.0f;  // × Clay's 10 per notch × UI scale ≈ 40px at 1080p

Clay_ElementId idOf(uint32_t id) {
    Clay_ElementId e{};
    e.id = id;
    return e;
}

glm::vec4 toVec4(const Clay_Color& c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}; }

Rect toRect(const Clay_BoundingBox& b) { return {{b.x, b.y}, {b.width, b.height}}; }

bool contains(const std::vector<uint32_t>& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); }

}  // namespace

Context* Context::current() { return s_current; }

Context::Context(Renderer2D& renderer, Window& window, int32_t maxElements) : m_renderer(renderer), m_window(window) {
    Clay_SetMaxElementCount(maxElements);
    uint32_t bytes = Clay_MinMemorySize();
    m_clayMemory = std::make_unique<char[]>(bytes);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(bytes, m_clayMemory.get());
    glm::vec2 size = renderer.screenSize();
    Clay_ErrorHandler onError{};
    onError.errorHandlerFunction = [](Clay_ErrorData e) {
        ENG_LOG("[ui] clay: " << std::string_view(e.errorText.chars, size_t(e.errorText.length)));
    };
    Clay_Initialize(arena, {size.x, size.y}, onError);
    Clay_SetMeasureTextFunction(
        [](Clay_StringSlice text, Clay_TextElementConfig* config, void* user) -> Clay_Dimensions {
            auto* self = static_cast<Context*>(user);
            glm::vec2 d = self->m_renderer.measureText(config->fontId, config->fontSize,
                                                       std::string_view(text.chars, size_t(text.length)),
                                                       config->letterSpacing);
            return {d.x, d.y};
        },
        this);
    s_current = this;
}

Context::~Context() {
    if (s_current == this) s_current = nullptr;
}

// ── Frame ────────────────────────────────────────────────────────────────────

void Context::begin(const Input& input, float dt) {
    s_current = this;
    m_input = &input;
    m_dt = dt;
    m_time += dt;
    m_chunkIndex = 0;
    m_chunkUsed = 0;
    m_customDraws.clear();
    m_images.clear();
    m_prevFocusOrder.swap(m_focusOrder);
    m_focusOrder.clear();
    m_prevScrollContainers.swap(m_scrollContainers);
    m_scrollContainers.clear();
    m_hoveredAny = false;
    m_editStarted = false;

    // Pointer: Clay hit-tests against last frame's layout here.
    m_mouse = {float(input.mouseX()), float(input.mouseY())};
    bool down = input.mouseDown(GLFW_MOUSE_BUTTON_LEFT);
    m_mousePressed = down && !m_mouseDown;
    m_mouseReleased = !down && m_mouseDown;
    m_mouseDown = down;
    glm::vec2 size = m_renderer.screenSize();
    Clay_SetLayoutDimensions({size.x, size.y});
    Clay_SetPointerState({m_mouse.x, m_mouse.y}, down);

    // The mouse moving or clicking hands control back from keys/pad.
    glm::vec2 moved = m_mouse - m_lastMouse;
    if ((m_lastMouse.x >= 0.0f && glm::dot(moved, moved) > 9.0f) || m_mousePressed) m_navVisible = false;
    m_lastMouse = m_mouse;
    if (m_mousePressed) {
        m_activeId = 0;  // a widget claims it during layout
        if (m_editingId && !Clay_PointerOver(idOf(m_editingId))) m_editingId = 0;
    }
    // A text field that vanished (screen changed) can't keep the keyboard.
    if (m_editingId && !contains(m_prevFocusOrder, m_editingId)) m_editingId = 0;

    processNavigation(dt);

    Clay_UpdateScrollContainers(false, {input.scrollX() * kWheelPixels * m_scale, input.scrollY() * kWheelPixels * m_scale},
                                dt);
    if (m_scrollToFocus) scrollFocusIntoView();
    Clay_BeginLayout();
}

void Context::end() {
    Clay_RenderCommandArray commands = Clay_EndLayout(m_dt);
    render(commands);
    if (m_mouseReleased) m_activeId = 0;
}

// ── Navigation ───────────────────────────────────────────────────────────────

void Context::processNavigation(float dt) {
    const Input& in = *m_input;
    m_navActivate = false;
    m_back = false;
    m_navX = 0;
    if (m_editingId) return;  // the text field owns the keys

    auto padPressed = [&](int button) {
        for (int i = 0; i < Input::kMaxGamepads; ++i)
            if (in.padPressed(i, button)) return true;
        return false;
    };
    bool shift = in.keyDown(GLFW_KEY_LEFT_SHIFT) || in.keyDown(GLFW_KEY_RIGHT_SHIFT);
    int dy = 0;
    if (in.keyTyped(GLFW_KEY_UP) || padPressed(GLFW_GAMEPAD_BUTTON_DPAD_UP) || (in.keyTyped(GLFW_KEY_TAB) && shift)) dy = -1;
    if (in.keyTyped(GLFW_KEY_DOWN) || padPressed(GLFW_GAMEPAD_BUTTON_DPAD_DOWN) || (in.keyTyped(GLFW_KEY_TAB) && !shift)) dy = 1;
    if (in.keyTyped(GLFW_KEY_LEFT) || padPressed(GLFW_GAMEPAD_BUTTON_DPAD_LEFT)) m_navX = -1;
    if (in.keyTyped(GLFW_KEY_RIGHT) || padPressed(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT)) m_navX = 1;

    // Left stick as a d-pad: one step on tilt, then auto-repeat while held.
    for (int axis = 0; axis < 2; ++axis) {
        float v = 0.0f;
        for (int i = 0; i < Input::kMaxGamepads; ++i) {
            float a = in.padAxis(i, axis == 0 ? GLFW_GAMEPAD_AXIS_LEFT_X : GLFW_GAMEPAD_AXIS_LEFT_Y);
            if (std::fabs(a) > std::fabs(v)) v = a;
        }
        int dir = v < -0.5f ? -1 : v > 0.5f ? 1 : 0;
        int step = 0;
        if (dir == 0) {
            m_stickDir[axis] = 0;
        } else if (dir != m_stickDir[axis]) {
            m_stickDir[axis] = dir;
            m_stickRepeat[axis] = 0.35f;
            step = dir;
        } else if ((m_stickRepeat[axis] -= dt) <= 0.0f) {
            m_stickRepeat[axis] = 0.1f;
            step = dir;
        }
        if (step && axis == 0) m_navX = step;
        if (step && axis == 1) dy = step;
    }

    bool activate = in.keyPressed(GLFW_KEY_ENTER) || in.keyPressed(GLFW_KEY_KP_ENTER) || in.keyPressed(GLFW_KEY_SPACE) ||
                    padPressed(GLFW_GAMEPAD_BUTTON_A);
    m_back = in.keyPressed(GLFW_KEY_ESCAPE) || padPressed(GLFW_GAMEPAD_BUTTON_B);
    if (dy || m_navX || activate) m_navVisible = true;

    const std::vector<uint32_t>& order = m_prevFocusOrder;
    if (order.empty()) return;
    bool focusValid = m_focusId && contains(order, m_focusId);
    // Keys/pad in use but nothing focused on this screen: start at the top
    // (a menu opened with the gamepad is ready to use).
    if (m_navVisible && !focusValid) {
        m_focusId = order.front();
        m_scrollToFocus = true;
        return;  // this press only placed the focus
    }
    if (dy && focusValid) {
        auto it = std::find(order.begin(), order.end(), m_focusId);
        int i = int(it - order.begin()), n = int(order.size());
        m_focusId = order[size_t((i + dy + n) % n)];  // wraps around
        m_scrollToFocus = true;
    }
    m_navActivate = activate && focusValid;
}

void Context::scrollFocusIntoView() {
    m_scrollToFocus = false;
    Clay_ElementData focus = Clay_GetElementData(idOf(m_focusId));
    if (!focus.found) return;
    const Clay_BoundingBox& f = focus.boundingBox;
    // The innermost known scroll container whose content holds the focus.
    Clay_ScrollContainerData best{};
    Clay_BoundingBox bestBox{};
    float bestArea = 0.0f;
    for (uint32_t c : m_prevScrollContainers) {
        Clay_ScrollContainerData data = Clay_GetScrollContainerData(idOf(c));
        Clay_ElementData box = Clay_GetElementData(idOf(c));
        if (!data.found || !box.found || !data.scrollPosition) continue;
        const Clay_BoundingBox& b = box.boundingBox;
        float contentTop = b.y + data.scrollPosition->y;
        bool inside = f.x >= b.x - 1.0f && f.x + f.width <= b.x + b.width + 1.0f && f.y >= contentTop - 1.0f &&
                      f.y + f.height <= contentTop + data.contentDimensions.height + 1.0f;
        float area = b.width * b.height;
        if (inside && (bestArea == 0.0f || area < bestArea)) {
            best = data;
            bestBox = b;
            bestArea = area;
        }
    }
    if (bestArea == 0.0f) return;
    float margin = px(theme.padding);
    float& scroll = best.scrollPosition->y;
    if (f.y < bestBox.y + margin) scroll += bestBox.y + margin - f.y;
    else if (f.y + f.height > bestBox.y + bestBox.height - margin) scroll -= f.y + f.height - (bestBox.y + bestBox.height - margin);
    float lowest = -std::max(0.0f, best.contentDimensions.height - bestBox.height);
    scroll = std::clamp(scroll, lowest, 0.0f);
}

void Context::setFocus(Clay_ElementId id) {
    m_focusId = id.id;
    m_scrollToFocus = true;
}

void Context::startEditing(Clay_ElementId id) {
    m_editingId = id.id;
    m_focusId = id.id;
    m_editStarted = true;
}

Context::Interaction Context::interact(Clay_ElementId id, bool focusable) {
    Interaction r;
    if (focusable) m_focusOrder.push_back(id.id);
    r.hovered = Clay_PointerOver(id);
    if (r.hovered) m_hoveredAny = true;
    if (r.hovered && m_mousePressed) {
        m_activeId = id.id;
        if (focusable) m_focusId = id.id;
    }
    r.held = m_activeId == id.id && m_mouseDown;
    r.clicked = m_activeId == id.id && m_mouseReleased && r.hovered;
    r.focused = focusable && m_focusId == id.id;
    r.activated = r.focused && m_navActivate;
    return r;
}

// ── Sizes, strings, element configs ─────────────────────────────────────────

void Context::setScale(float scale) { m_scale = std::max(0.25f, scale); }

float Context::px(float authored) const { return std::round(authored * m_scale); }

uint16_t Context::fontPx(float authored) const {
    return uint16_t(std::clamp(std::lround(authored * m_scale), 1L, 400L));
}

Clay_TextElementConfig Context::textConfig(const TextStyle& style) const {
    float size = style.size > 0.0f ? style.size
                 : style.title     ? theme.titleFontSize
                 : style.small     ? theme.smallFontSize
                                   : theme.fontSize;
    FontId font = style.font != kNoFont                         ? style.font
                  : style.title && theme.titleFont != kNoFont ? theme.titleFont
                                                                : theme.font;
    glm::vec4 c = style.color.r >= 0.0f ? style.color : style.dim ? theme.textDim : theme.text;
    Clay_TextElementConfig config{};
    config.textColor = color(c);
    config.fontId = uint16_t(font == kNoFont ? 0xFFFF : font);
    config.fontSize = fontPx(size);
    config.wrapMode = style.wrap;
    config.textAlignment = style.align;
    return config;
}

Clay_String Context::str(std::string_view s) {
    Clay_String out{};
    if (s.empty()) {
        out.chars = "";
        return out;
    }
    if (m_chunkIndex < m_chunks.size() && m_chunkUsed + s.size() > m_chunks[m_chunkIndex].size) {
        ++m_chunkIndex;
        m_chunkUsed = 0;
    }
    if (m_chunkIndex == m_chunks.size()) m_chunks.push_back({});
    Chunk& chunk = m_chunks[m_chunkIndex];
    if (chunk.size < s.size()) {  // unused this frame (we're at its start), so safe to replace
        chunk.size = std::max(kChunkSize, s.size());
        chunk.data = std::make_unique<char[]>(chunk.size);
    }
    char* dst = chunk.data.get() + m_chunkUsed;
    std::memcpy(dst, s.data(), s.size());
    m_chunkUsed += s.size();
    out.length = int32_t(s.size());
    out.chars = dst;
    return out;
}

Clay_String Context::fmt(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n < 0) return str({});
    if (size_t(n) < sizeof(buf)) return str(std::string_view(buf, size_t(n)));
    std::string big(size_t(n) + 1, '\0');
    va_start(args, format);
    std::vsnprintf(big.data(), big.size(), format, args);
    va_end(args);
    return str(std::string_view(big.data(), size_t(n)));
}

Clay_ImageElementConfig Context::image(TextureId texture, const glm::vec4& tint) {
    m_images.push_back({texture, tint});
    Clay_ImageElementConfig c{};
    c.imageData = reinterpret_cast<void*>(uintptr_t(m_images.size()));  // index + 1: never null, so Clay sees an image
    return c;
}

Clay_CustomElementConfig Context::custom(std::function<void(Renderer2D&, const Rect&)> draw) {
    m_customDraws.push_back(std::move(draw));
    Clay_CustomElementConfig c{};
    c.customData = reinterpret_cast<void*>(uintptr_t(m_customDraws.size()));  // index + 1
    return c;
}

Clay_ClipElementConfig Context::scrollY() {
    // Called while Clay evaluates this element's declaration, so the open
    // element is the container itself.
    m_scrollContainers.push_back(Clay_GetOpenElementId());
    Clay_ClipElementConfig c{};
    c.vertical = true;
    c.childOffset = Clay_GetScrollOffset();
    return c;
}

// ── Rendering ────────────────────────────────────────────────────────────────

void Context::render(Clay_RenderCommandArray commands) {
    Renderer2D& r = m_renderer;
    for (int32_t i = 0; i < commands.length; ++i) {
        const Clay_RenderCommand& cmd = *Clay_RenderCommandArray_Get(&commands, i);
        Rect box = toRect(cmd.boundingBox);
        switch (cmd.commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            const Clay_RectangleRenderData& d = cmd.renderData.rectangle;
            const Clay_CornerRadius& c = d.cornerRadius;
            r.drawRoundedRect(box, {c.topLeft, c.topRight, c.bottomRight, c.bottomLeft}, toVec4(d.backgroundColor));
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            const Clay_BorderRenderData& d = cmd.renderData.border;
            const Clay_CornerRadius& c = d.cornerRadius;
            r.drawRoundedBorder(box, {c.topLeft, c.topRight, c.bottomRight, c.bottomLeft},
                                {float(d.width.left), float(d.width.top), float(d.width.right), float(d.width.bottom)},
                                toVec4(d.color));
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            const Clay_TextRenderData& d = cmd.renderData.text;
            // One command per wrapped line; center the font's line box in it
            // (they differ when the element sets a lineHeight).
            FontMetrics m = r.fontMetrics(d.fontId, d.fontSize);
            float y = box.pos.y + (box.size.y - m.lineHeight) * 0.5f;
            r.drawText(d.fontId, d.fontSize, {box.pos.x, y}, toVec4(d.textColor),
                       std::string_view(d.stringContents.chars, size_t(d.stringContents.length)), Space::Screen,
                       float(d.letterSpacing));
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
            auto index = reinterpret_cast<uintptr_t>(cmd.renderData.image.imageData);
            if (index < 1 || index > m_images.size()) break;
            Sprite s;
            s.texture = m_images[index - 1].texture;
            s.color = m_images[index - 1].tint;
            s.pos = box.pos + box.size * 0.5f;
            s.size = box.size;
            s.space = Space::Screen;
            r.draw(s);
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
            r.pushClip(box);
            break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
            r.popClip();
            break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
            auto index = reinterpret_cast<uintptr_t>(cmd.renderData.custom.customData);
            if (index >= 1 && index <= m_customDraws.size()) m_customDraws[index - 1](r, box);
            break;
        }
        default:  // overlay colors aren't used by eng::ui
            break;
        }
    }
}

}  // namespace eng::ui
