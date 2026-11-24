// eng::ui's widgets: Clay elements plus Context::interact() for pointer and
// keyboard/gamepad input. Declarations are filled field by field rather than
// with CLAY({...}) so C++'s designated-initializer ordering never bites.
#include "eng/platform/Input.h"
#include "eng/platform/Window.h"
#include "eng/ui/Ui.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace eng::ui {

namespace {

Clay_ElementId childId(Clay_ElementId parent, Clay_String name) { return Clay__HashString(name, parent.id); }

void open(Clay_ElementId id, const Clay_ElementDeclaration& d) {
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(d);
}
void openAuto(const Clay_ElementDeclaration& d) {
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}
void close() { Clay__CloseElement(); }

Clay_BorderElementConfig border(const glm::vec4& c, float width) {
    Clay_BorderElementConfig b{};
    auto w = uint16_t(std::max(0.0f, width));
    b.color = color(c);
    b.width = {w, w, w, w, 0};
    return b;
}

// Focus ring when keys/pad are driving, else `fallback` (may be width 0).
Clay_BorderElementConfig focusBorder(const Context& ui, bool focused, const glm::vec4& fallback, float fallbackWidth) {
    if (focused && ui.navVisible()) return border(ui.theme.accent, ui.px(ui.theme.focusWidth));
    return border(fallback, fallbackWidth);
}

void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

void popUtf8(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // continuation bytes
    if (!s.empty()) s.pop_back();
}

// Shared by sliderFloat/sliderInt: `t` is the value's position in [0, 1].
bool slider(Clay_ElementId id, std::string_view label, float& t, float step, Clay_String valueText) {
    Context& ui = *Context::current();
    const Theme& th = ui.theme;
    auto it = ui.interact(id);
    Clay_ElementId track = childId(id, CLAY_STRING("track"));
    float before = t;
    if (ui.isActive(id) && it.held) {
        Clay_ElementData td = Clay_GetElementData(track);
        if (td.found && td.boundingBox.width > 0.0f)
            t = (ui.mouse().x - td.boundingBox.x) / td.boundingBox.width;
    }
    if (it.focused && ui.navHorizontal()) t += float(ui.navHorizontal()) * step;
    t = std::clamp(t, 0.0f, 1.0f);

    Clay_ElementDeclaration row{};
    row.layout.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)};
    row.layout.padding = {uint16_t(ui.px(4)), uint16_t(ui.px(4)), uint16_t(ui.px(4)), uint16_t(ui.px(4))};
    row.layout.childGap = uint16_t(ui.px(th.gap * 1.5f));
    row.layout.childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER};
    row.cornerRadius = radius(ui.px(th.radius));
    row.border = focusBorder(ui, it.focused, {}, 0.0f);
    open(id, row);
    if (!label.empty()) text(label);

    Clay_ElementDeclaration bar{};
    bar.layout.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(ui.px(10))};
    bar.backgroundColor = color(it.hovered || it.held ? th.widgetHover : th.field);
    bar.cornerRadius = radius(ui.px(5));
    open(track, bar);
    {
        Clay_ElementDeclaration fill{};
        fill.layout.sizing = {CLAY_SIZING_PERCENT(t), CLAY_SIZING_GROW(0)};
        fill.backgroundColor = color(th.accent);
        fill.cornerRadius = radius(ui.px(5));
        openAuto(fill);
        close();
    }
    close();

    Clay_ElementDeclaration value{};
    value.layout.sizing = {CLAY_SIZING_FIXED(ui.px(72)), CLAY_SIZING_FIT(0)};
    value.layout.childAlignment = {CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER};
    openAuto(value);
    TextStyle vs;
    vs.dim = !(it.focused || it.held);
    vs.wrap = CLAY_TEXT_WRAP_NONE;
    Clay__OpenTextElement(valueText, ui.textConfig(vs));
    close();
    close();
    return t != before;
}

}  // namespace

void text(std::string_view s, const TextStyle& style) {
    Context& ui = *Context::current();
    Clay__OpenTextElement(ui.str(s), ui.textConfig(style));
}

bool button(Clay_ElementId id, std::string_view label, const ButtonOptions& o) {
    Context& ui = *Context::current();
    const Theme& th = ui.theme;
    auto it = ui.interact(id, !o.disabled);
    bool custom = o.color.r >= 0.0f;
    glm::vec4 base = custom ? o.color : th.widget;
    glm::vec4 hover = custom ? glm::mix(o.color, glm::vec4(1.0f, 0.95f, 0.85f, o.color.a), 0.18f) : th.widgetHover;
    glm::vec4 press = custom ? o.color * glm::vec4(0.8f, 0.8f, 0.8f, 1.0f) : th.widgetActive;
    glm::vec4 bg = o.disabled                                      ? base * glm::vec4(1, 1, 1, 0.5f)
                   : it.held                                       ? press
                   : it.hovered || (it.focused && ui.navVisible()) ? hover
                                                                   : base;
    Clay_SizingAxis width = o.grow ? CLAY_SIZING_GROW(0) : o.width > 0.0f ? CLAY_SIZING_FIXED(ui.px(o.width)) : CLAY_SIZING_FIT(0);
    auto padX = uint16_t(ui.px(o.small ? 10 : 18)), padY = uint16_t(ui.px(o.padY > 0.0f ? o.padY : o.small ? 4 : 8));

    Clay_ElementDeclaration d{};
    d.layout.sizing = {width, CLAY_SIZING_FIT(0)};
    d.layout.padding = {padX, padX, padY, padY};
    d.layout.childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER};
    d.backgroundColor = color(bg);
    d.cornerRadius = radius(ui.px(th.radius));
    d.border = focusBorder(ui, it.focused, {}, 0.0f);
    open(id, d);
    TextStyle ts;
    ts.small = o.small;
    ts.dim = o.disabled;
    if (o.textColor.r >= 0.0f) ts.color = o.disabled ? o.textColor * glm::vec4(1, 1, 1, 0.5f) : o.textColor;
    ts.align = CLAY_TEXT_ALIGN_CENTER;
    ts.wrap = CLAY_TEXT_WRAP_NONE;
    text(label, ts);
    close();
    return !o.disabled && (it.clicked || it.activated);
}

bool checkbox(Clay_ElementId id, std::string_view label, bool& value) {
    Context& ui = *Context::current();
    const Theme& th = ui.theme;
    auto it = ui.interact(id);
    bool toggled = it.clicked || it.activated;
    if (toggled) value = !value;

    Clay_ElementDeclaration row{};
    row.layout.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)};
    row.layout.padding = {uint16_t(ui.px(4)), uint16_t(ui.px(8)), uint16_t(ui.px(4)), uint16_t(ui.px(4))};
    row.layout.childGap = uint16_t(ui.px(th.gap * 1.25f));
    row.layout.childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER};
    row.cornerRadius = radius(ui.px(th.radius));
    row.border = focusBorder(ui, it.focused, {}, 0.0f);
    open(id, row);

    float box = ui.px(th.fontSize);
    Clay_ElementDeclaration b{};
    b.layout.sizing = {CLAY_SIZING_FIXED(box), CLAY_SIZING_FIXED(box)};
    auto inset = uint16_t(ui.px(4));
    b.layout.padding = {inset, inset, inset, inset};
    b.backgroundColor = color(th.field);
    b.cornerRadius = radius(ui.px(4));
    b.border = border(it.hovered ? th.accent : th.widgetHover, std::max(1.0f, ui.px(1.5f)));
    openAuto(b);
    if (value) {
        Clay_ElementDeclaration mark{};
        mark.layout.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)};
        mark.backgroundColor = color(th.accent);
        mark.cornerRadius = radius(ui.px(2));
        openAuto(mark);
        close();
    }
    close();
    if (!label.empty()) text(label);
    close();
    return toggled;
}

bool sliderFloat(Clay_ElementId id, std::string_view label, float& value, float min, float max, const char* format) {
    Context& ui = *Context::current();
    float range = max - min;
    float t = range != 0.0f ? (value - min) / range : 0.0f;
    if (!slider(id, label, t, 0.05f, ui.fmt(format, double(value)))) return false;
    value = min + t * range;
    return true;
}

bool sliderInt(Clay_ElementId id, std::string_view label, int& value, int min, int max, const char* format) {
    Context& ui = *Context::current();
    int range = max - min;
    float t = range != 0 ? float(value - min) / float(range) : 0.0f;
    float step = range > 0 ? 1.0f / float(std::min(range, 20)) : 0.0f;  // whole numbers; ≤ 20 presses end to end
    if (!slider(id, label, t, step, ui.fmt(format, value))) return false;
    int v = min + int(std::lround(t * float(range)));
    if (v == value) return false;
    value = v;
    return true;
}

bool textField(Clay_ElementId id, std::string& value, std::string_view hint, size_t maxBytes) {
    Context& ui = *Context::current();
    const Theme& th = ui.theme;
    const Input& in = ui.input();
    auto it = ui.interact(id);
    if (!ui.isEditing(id) && (it.clicked || it.activated)) ui.startEditing(id);
    bool editing = ui.isEditing(id);
    bool changed = false;

    if (editing && !ui.editingJustStarted()) {
        auto insert = [&](uint32_t cp) {
            if (cp < 32 || cp == 127) return;  // control characters
            std::string bytes;
            appendUtf8(bytes, cp);
            if (value.size() + bytes.size() > maxBytes) return;
            value += bytes;
            changed = true;
        };
        for (uint32_t cp : in.typedChars()) insert(cp);
        bool ctrl = in.keyDown(GLFW_KEY_LEFT_CONTROL) || in.keyDown(GLFW_KEY_RIGHT_CONTROL) ||
                    in.keyDown(GLFW_KEY_LEFT_SUPER) || in.keyDown(GLFW_KEY_RIGHT_SUPER);
        if (ctrl && in.keyTyped(GLFW_KEY_V)) {
            if (const char* clip = glfwGetClipboardString(ui.window().handle())) {
                for (const char* p = clip; *p; ++p) {  // byte-wise: keeps valid UTF-8 as-is
                    auto c = static_cast<unsigned char>(*p);
                    if (c < 32 || value.size() + 1 > maxBytes) break;  // stop at a newline / the cap
                    value += char(c);
                    changed = true;
                }
                // Don't leave a codepoint cut in half by the cap.
                size_t i = value.size();
                while (i > 0 && (static_cast<unsigned char>(value[i - 1]) & 0xC0) == 0x80) --i;
                if (i > 0) {
                    auto lead = static_cast<unsigned char>(value[i - 1]);
                    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                    if (value.size() - (i - 1) < need) value.resize(i - 1);
                }
            }
        }
        if (in.keyTyped(GLFW_KEY_BACKSPACE) && !value.empty()) {
            popUtf8(value);
            changed = true;
        }
        bool padB = false;
        for (int i = 0; i < Input::kMaxGamepads; ++i) padB |= in.padPressed(i, GLFW_GAMEPAD_BUTTON_B);
        if (in.keyPressed(GLFW_KEY_ENTER) || in.keyPressed(GLFW_KEY_KP_ENTER) || in.keyPressed(GLFW_KEY_ESCAPE) || padB) {
            ui.stopEditing();
            editing = false;
        }
    }

    Clay_ElementDeclaration box{};
    auto padX = uint16_t(ui.px(10)), padY = uint16_t(ui.px(6));
    box.layout.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)};
    box.layout.padding = {padX, padX, padY, padY};
    box.layout.childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER};
    box.backgroundColor = color(th.field);
    box.cornerRadius = radius(ui.px(th.radius));
    box.border = editing ? border(th.accent, ui.px(th.focusWidth))
                         : focusBorder(ui, it.focused, it.hovered ? th.widgetActive : th.widgetHover, std::max(1.0f, ui.px(1)));
    // Keep the end (where typing happens) in view once the text overflows.
    box.clip.horizontal = true;
    Clay_ElementData last = Clay_GetElementData(id);
    if (last.found) {
        Clay_TextElementConfig tc = ui.textConfig();
        float textW = ui.renderer().measureText(tc.fontId, tc.fontSize, value + "|").x;
        float inner = last.boundingBox.width - 2.0f * padX;
        box.clip.childOffset.x = std::min(0.0f, inner - textW);
    }
    open(id, box);
    TextStyle ts;
    ts.wrap = CLAY_TEXT_WRAP_NONE;
    if (value.empty() && !editing) {
        ts.dim = true;
        text(hint, ts);
    } else {
        text(value, ts);
    }
    if (editing) {
        TextStyle caret = ts;
        caret.color = std::fmod(ui.time(), 1.0f) < 0.6f ? th.accent : glm::vec4(0.0f);
        text("|", caret);
    }
    close();
    return changed;
}

void tooltip(Clay_ElementId anchor, std::string_view s) {
    Context& ui = *Context::current();
    const Theme& th = ui.theme;
    Clay_ElementDeclaration d{};
    auto padX = uint16_t(ui.px(12)), padY = uint16_t(ui.px(7));
    d.layout.sizing = {CLAY_SIZING_FIT(0, ui.px(420)), CLAY_SIZING_FIT(0)};
    d.layout.padding = {padX, padX, padY, padY};
    d.backgroundColor = color(th.field * glm::vec4(1, 1, 1, 0.97f));
    d.cornerRadius = radius(ui.px(th.radius));
    d.floating.offset = {0.0f, ui.px(6)};
    d.floating.parentId = anchor.id;
    d.floating.zIndex = 1000;
    d.floating.attachPoints = {CLAY_ATTACH_POINT_LEFT_TOP, CLAY_ATTACH_POINT_LEFT_BOTTOM};
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    d.floating.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
    d.border = border(th.accent * glm::vec4(1, 1, 1, 0.5f), std::max(1.0f, ui.px(1)));
    openAuto(d);
    TextStyle ts;
    ts.small = true;
    text(s, ts);
    close();
}

void tooltipAt(glm::vec2 pos, std::string_view s) {
    beginTooltipAt(pos);
    TextStyle ts;
    ts.small = true;
    text(s, ts);
    endTooltip();
}

void endTooltip() { close(); }

void beginTooltipAt(glm::vec2 pos) {
    Context& ui = *Context::current();
    const Theme& th = ui.theme;
    Clay_ElementDeclaration d{};
    auto padX = uint16_t(ui.px(12)), padY = uint16_t(ui.px(7));
    d.layout.sizing = {CLAY_SIZING_FIT(0, ui.px(420)), CLAY_SIZING_FIT(0)};
    d.layout.padding = {padX, padX, padY, padY};
    d.backgroundColor = color(th.field * glm::vec4(1, 1, 1, 0.97f));
    d.cornerRadius = radius(ui.px(th.radius));
    // Below-right of the point, flipped left/up near the screen's far edges
    // (judged roughly — the note's size isn't known until layout).
    glm::vec2 screen = ui.renderer().screenSize();
    bool left = pos.x > screen.x - ui.px(440), up = pos.y > screen.y - ui.px(120);
    d.floating.offset = {pos.x + (left ? -ui.px(12) : ui.px(16)), pos.y + (up ? -ui.px(8) : ui.px(20))};
    d.floating.zIndex = 1000;
    d.floating.attachPoints = {left ? (up ? CLAY_ATTACH_POINT_RIGHT_BOTTOM : CLAY_ATTACH_POINT_RIGHT_TOP)
                                    : (up ? CLAY_ATTACH_POINT_LEFT_BOTTOM : CLAY_ATTACH_POINT_LEFT_TOP),
                               CLAY_ATTACH_POINT_LEFT_TOP};
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    d.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    d.border = border(th.accent * glm::vec4(1, 1, 1, 0.5f), std::max(1.0f, ui.px(1)));
    d.layout.childGap = uint16_t(ui.px(3));
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    openAuto(d);
}

Context::Interaction hitbox(Clay_ElementId id, const Rect& r, bool focusable, int16_t zIndex) {
    Context& ui = *Context::current();
    auto it = ui.interact(id, focusable);
    Clay_ElementDeclaration d{};
    d.layout.sizing = {CLAY_SIZING_FIXED(std::max(0.0f, r.size.x)), CLAY_SIZING_FIXED(std::max(0.0f, r.size.y))};
    d.floating.offset = {r.pos.x, r.pos.y};
    d.floating.zIndex = zIndex;
    d.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_CAPTURE;
    open(id, d);
    close();
    return it;
}

void beginPanel(Clay_ElementId id, const PanelOptions& o) {
    Context& ui = *Context::current();
    glm::vec2 screen = ui.renderer().screenSize();
    if (o.dim) {  // a full-screen shade, its own layer just under the panel
        Clay_ElementDeclaration shade{};
        shade.layout.sizing = {CLAY_SIZING_FIXED(screen.x), CLAY_SIZING_FIXED(screen.y)};
        shade.backgroundColor = color(o.dimColor);
        shade.floating.zIndex = int16_t(o.zIndex - 1);
        shade.floating.attachTo = CLAY_ATTACH_TO_ROOT;
        shade.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_CAPTURE;
        open(childId(id, CLAY_STRING("shade")), shade);
        close();
    }
    float maxW = screen.x - ui.px(24), maxH = screen.y - ui.px(24);
    Clay_ElementDeclaration d{};
    Clay_SizingAxis w = o.width > 0.0f ? CLAY_SIZING_FIXED(std::min(ui.px(o.width), maxW)) : CLAY_SIZING_FIT(0, maxW);
    Clay_SizingAxis h = o.height > 0.0f ? CLAY_SIZING_FIXED(std::min(ui.px(o.height), maxH))
                                        : CLAY_SIZING_FIT(0, o.maxHeight > 0.0f ? std::min(ui.px(o.maxHeight), maxH) : maxH);
    d.layout.sizing = {w, h};
    auto px = uint16_t(ui.px(o.padX)), py = uint16_t(ui.px(o.padY));
    d.layout.padding = {px, px, py, py};
    d.layout.childGap = uint16_t(ui.px(o.gap));
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    d.custom = o.background;
    if (!o.background.customData) {
        d.backgroundColor = color(o.color.r >= 0.0f ? o.color : ui.theme.panel);
        d.cornerRadius = radius(ui.px(ui.theme.radius * 2.0f));
    }
    if (o.scroll) d.clip = ui.scrollY();
    // Anchored by the same point of the panel as of the screen (0.5 = centered).
    auto point = [](glm::vec2 a) {
        int col = a.x < 0.34f ? 0 : a.x < 0.67f ? 1 : 2, row = a.y < 0.34f ? 0 : a.y < 0.67f ? 1 : 2;
        static const Clay_FloatingAttachPointType kPoints[3][3] = {
            {CLAY_ATTACH_POINT_LEFT_TOP, CLAY_ATTACH_POINT_CENTER_TOP, CLAY_ATTACH_POINT_RIGHT_TOP},
            {CLAY_ATTACH_POINT_LEFT_CENTER, CLAY_ATTACH_POINT_CENTER_CENTER, CLAY_ATTACH_POINT_RIGHT_CENTER},
            {CLAY_ATTACH_POINT_LEFT_BOTTOM, CLAY_ATTACH_POINT_CENTER_BOTTOM, CLAY_ATTACH_POINT_RIGHT_BOTTOM}};
        return kPoints[row][col];
    };
    Clay_FloatingAttachPointType p = point(o.anchor);
    // Attach point = the panel's point to the root's same point; the offset
    // then moves it from that screen anchor to the requested fraction.
    glm::vec2 snapped{p == CLAY_ATTACH_POINT_LEFT_TOP || p == CLAY_ATTACH_POINT_LEFT_CENTER || p == CLAY_ATTACH_POINT_LEFT_BOTTOM ? 0.0f
                      : p == CLAY_ATTACH_POINT_CENTER_TOP || p == CLAY_ATTACH_POINT_CENTER_CENTER || p == CLAY_ATTACH_POINT_CENTER_BOTTOM
                          ? 0.5f
                          : 1.0f,
                      p == CLAY_ATTACH_POINT_LEFT_TOP || p == CLAY_ATTACH_POINT_CENTER_TOP || p == CLAY_ATTACH_POINT_RIGHT_TOP ? 0.0f
                      : p == CLAY_ATTACH_POINT_LEFT_CENTER || p == CLAY_ATTACH_POINT_CENTER_CENTER || p == CLAY_ATTACH_POINT_RIGHT_CENTER
                          ? 0.5f
                          : 1.0f};
    glm::vec2 off = (o.anchor - snapped) * screen + glm::vec2(ui.px(o.offset.x), ui.px(o.offset.y));
    d.floating.offset = {off.x, off.y};
    d.floating.zIndex = o.zIndex;
    d.floating.attachPoints = {p, p};
    d.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    d.floating.pointerCaptureMode = o.capture ? CLAY_POINTER_CAPTURE_MODE_CAPTURE : CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    open(id, d);
}

void endPanel() { close(); }

}  // namespace eng::ui
