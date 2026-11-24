#pragma once

// eng::ui — immediate-mode game UI: Clay (https://github.com/nicbarker/clay)
// lays it out, Renderer2D draws it, and a small widget set adds interaction,
// keyboard/gamepad focus navigation and text entry on top.
//
// Per frame, between Renderer2D::beginFrame() and endFrame():
//
//   ui.begin(input, dt);
//   CLAY(CLAY_ID("menu"), { .layout = { ... }, .backgroundColor = ui::color(ui.theme.panel) }) {
//       ui::text("Settings", {.title = true});
//       if (ui::button(CLAY_ID("back"), "Back")) ...;
//   }
//   ui.end();  // draws on top of everything drawn so far this frame
//
// Like ImGui, interaction uses the previous frame's layout: a widget knows
// it's hovered or clicked when it's declared, before this frame's layout.
//
// Clay notes for C++:
// - Designated initializers must follow declaration order. For
//   Clay_ElementDeclaration that's: layout, backgroundColor, overlayColor,
//   cornerRadius, aspectRatio, image, floating, custom, clip, border,
//   transition, userData. Clay_LayoutConfig: sizing, padding, childGap,
//   childAlignment, layoutDirection.
// - Strings must stay alive until end(): CLAY_STRING("literal"), or copy
//   with ui.str()/ui.fmt(). Element ids: CLAY_ID("literal"), CLAY_IDI("row", i),
//   or CLAY_SID(ui.str(name)).
// - Colors are Clay's 0–255 convention; ui::color() converts a glm::vec4.

#include "eng/render/Renderer2D.h"

#include <clay.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eng {
class Input;
class Window;
}  // namespace eng

namespace eng::ui {

inline Clay_Color color(const glm::vec4& c) { return {c.r * 255.0f, c.g * 255.0f, c.b * 255.0f, c.a * 255.0f}; }
inline Clay_CornerRadius radius(float r) { return {r, r, r, r}; }

// Sizes are authored for a 1080p-tall screen; Context::setScale multiplies
// them (and the fonts) at draw time.
struct Theme {
    FontId font = kNoFont;       // body text
    FontId titleFont = kNoFont;  // titles; falls back to `font`
    float fontSize = 22.0f;
    float smallFontSize = 18.0f;
    float titleFontSize = 56.0f;

    glm::vec4 text{0.92f, 0.89f, 0.83f, 1.0f};
    glm::vec4 textDim{0.62f, 0.58f, 0.53f, 1.0f};
    glm::vec4 panel{0.07f, 0.06f, 0.06f, 0.92f};
    glm::vec4 widget{0.19f, 0.16f, 0.14f, 1.0f};
    glm::vec4 widgetHover{0.29f, 0.24f, 0.20f, 1.0f};
    glm::vec4 widgetActive{0.38f, 0.30f, 0.24f, 1.0f};
    glm::vec4 field{0.04f, 0.035f, 0.03f, 1.0f};  // text field / checkbox / slider track
    glm::vec4 accent{0.86f, 0.63f, 0.31f, 1.0f};  // check marks, slider fill, focus ring

    float radius = 6.0f;
    float padding = 10.0f;
    float gap = 8.0f;
    float focusWidth = 2.0f;
};

struct TextStyle {
    bool title = false;          // theme's title font and size
    bool small = false;          // theme's small size
    bool dim = false;            // theme's dim color
    float size = 0.0f;           // authored px; overrides title/small
    glm::vec4 color{-1.0f};      // overrides dim/text when r >= 0
    FontId font = kNoFont;       // overrides the theme's
    Clay_TextAlignment align = CLAY_TEXT_ALIGN_LEFT;
    Clay_TextElementConfigWrapMode wrap = CLAY_TEXT_WRAP_WORDS;
};

struct ButtonOptions {
    float width = 0.0f;  // authored px; 0 = fit the label
    bool grow = false;   // fill the parent's free width instead
    bool disabled = false;
    bool small = false;  // small font, less padding
    float padY = 0.0f;   // authored vertical padding; 0 = the default
    glm::vec4 color{-1.0f};      // fill (hover/press lighten it); r < 0 = the theme's
    glm::vec4 textColor{-1.0f};  // r < 0 = the theme's
};

// Floating layers (modal panels, tooltips) sit above the main layout and
// hitboxes; higher z draws on top and takes the pointer first.
struct PanelOptions {
    float width = 0.0f;         // authored px (clamped to the screen); 0 = fit
    float height = 0.0f;        // authored px, fixed (clamped to the screen); 0 = fit
    float maxHeight = 0.0f;     // authored px, when fitting; 0 = up to the screen
    glm::vec2 anchor{0.5f};     // where on the screen (0–1); the panel's same point sits there
    glm::vec2 offset{0.0f};     // authored px from the anchor
    float padX = 34.0f, padY = 30.0f, gap = 9.0f;
    int16_t zIndex = 100;
    bool capture = true;        // block the pointer from layers below
    bool dim = false;           // a full-screen shade under it (also captures)
    glm::vec4 dimColor{0.0f, 0.0f, 0.02f, 0.6f};
    Clay_CustomElementConfig background{};  // drawn behind the contents (e.g. a themed frame)
    bool scroll = false;        // contents scroll when taller than maxHeight
    glm::vec4 color{-1.0f};     // background (no `background`); r < 0 = the theme's panel colour
};

class Context {
public:
    // One per process (Clay keeps one global context). `window` is used for
    // clipboard paste in text fields.
    Context(Renderer2D& renderer, Window& window, int32_t maxElements = 8192);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // `navigation` false: keys and pad don't move focus or activate this
    // frame (say, while the game waits for a key to rebind). The mouse
    // still works.
    void begin(const Input& input, float dt, bool navigation = true);
    void end();

    Theme theme;
    void setScale(float scale);  // e.g. screen height / 1080
    float scale() const { return m_scale; }
    float px(float authored) const;           // authored px × scale, rounded
    uint16_t fontPx(float authored) const;    // same, for font sizes
    Clay_TextElementConfig textConfig(const TextStyle& style = {}) const;

    // Strings handed to Clay must outlive end(); these copy into a
    // per-frame arena.
    Clay_String str(std::string_view s);
    Clay_String fmt(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;

    // Element configs: CLAY(id, { .layout = ..., .image = ui.image(tex) }).
    // Tint the image here, not with backgroundColor: Clay draws an
    // element's background over its image.
    Clay_ImageElementConfig image(TextureId texture, const glm::vec4& tint = glm::vec4(1.0f));
    // Pointer over this element (last frame's layout), without registering it.
    bool isHovered(Clay_ElementId id) const { return Clay_PointerOver(id); }
    // Draw anything inside a laid-out box (screen pixels), in layout order.
    Clay_CustomElementConfig custom(std::function<void(Renderer2D&, const Rect&)> draw);
    // Vertical scroll container: CLAY(id, { ..., .clip = ui.scrollY() }).
    // Wheel scrolls it, and keyboard/gamepad focus scrolls to stay visible.
    Clay_ClipElementConfig scrollY();

    // ── Input routing ──
    bool wantsKeyboard() const { return m_editingId != 0; }  // a text field has the keyboard
    bool wantsMouse() const { return m_hoveredAny || m_activeId != 0; }
    bool navVisible() const { return m_navVisible; }         // focus ring shown (keys/pad in use)
    // Escape / gamepad B this frame, unless a text field used it to stop editing.
    bool backPressed() const { return m_back; }

    // ── Focus ──
    void setFocus(Clay_ElementId id);
    void clearFocus() { m_focusId = 0; }
    bool isFocused(Clay_ElementId id) const { return m_focusId == id.id && id.id != 0; }

    Renderer2D& renderer() { return m_renderer; }
    const Input& input() const { return *m_input; }
    static Context* current();

    // ── For widgets (Widgets.cpp) and custom ones built the same way ──
    struct Interaction {
        bool hovered = false;    // pointer over it (last frame's layout)
        bool held = false;       // mouse pressed on it and still down
        bool clicked = false;    // mouse released over it after pressing on it
        bool focused = false;    // keyboard/gamepad focus is on it
        bool activated = false;  // focused and Enter / Space / pad A this frame
    };
    // Registers a focusable (in declaration order, for navigation) and
    // reports pointer/nav state. Call once per frame per widget id.
    Interaction interact(Clay_ElementId id, bool focusable = true);
    // Left/right nav input this frame (sliders, checkboxes): -1, 0 or +1.
    int navHorizontal() const { return m_navX; }
    glm::vec2 mouse() const { return m_mouse; }
    bool isActive(Clay_ElementId id) const { return m_activeId == id.id && id.id != 0; }
    bool isEditing(Clay_ElementId id) const { return m_editingId == id.id && id.id != 0; }
    // Editing began this frame: the key or click that started it isn't input.
    bool editingJustStarted() const { return m_editStarted; }
    void startEditing(Clay_ElementId id);
    void stopEditing() { m_editingId = 0; }
    Window& window() { return m_window; }
    float time() const { return m_time; }

private:
    void processNavigation(float dt);
    void scrollFocusIntoView();
    void render(Clay_RenderCommandArray commands);

    Renderer2D& m_renderer;
    Window& m_window;
    const Input* m_input = nullptr;
    std::unique_ptr<char[]> m_clayMemory;
    float m_scale = 1.0f;
    float m_time = 0.0f;
    float m_dt = 0.0f;

    // Per-frame string arena: chunks never reallocate, so earlier strings
    // stay put; all reused from the start each frame.
    struct Chunk {
        std::unique_ptr<char[]> data;
        size_t size = 0;
    };
    std::vector<Chunk> m_chunks;
    size_t m_chunkUsed = 0, m_chunkIndex = 0;
    std::vector<std::function<void(Renderer2D&, const Rect&)>> m_customDraws;
    struct Image {
        TextureId texture;
        glm::vec4 tint;
    };
    std::vector<Image> m_images;

    // Pointer.
    glm::vec2 m_mouse{0.0f}, m_lastMouse{-1.0f};
    bool m_mouseDown = false, m_mousePressed = false, m_mouseReleased = false;
    uint32_t m_activeId = 0;  // element the mouse went down on
    bool m_hoveredAny = false;

    // Keyboard / gamepad navigation.
    std::vector<uint32_t> m_focusOrder, m_prevFocusOrder;
    std::vector<uint32_t> m_scrollContainers, m_prevScrollContainers;
    uint32_t m_focusId = 0;
    uint32_t m_editingId = 0;
    bool m_editStarted = false;
    bool m_navVisible = false;
    bool m_navActivate = false;
    bool m_back = false;
    bool m_scrollToFocus = false;
    int m_navX = 0;
    // Left stick as a d-pad with auto-repeat, per axis (x, y).
    int m_stickDir[2]{};
    float m_stickRepeat[2]{};
};

// ── Widgets ── (on Context::current(); return true when the value changed
// or the button was pressed)
void text(std::string_view s, const TextStyle& style = {});
bool button(Clay_ElementId id, std::string_view label, const ButtonOptions& options = {});
bool checkbox(Clay_ElementId id, std::string_view label, bool& value);
bool sliderFloat(Clay_ElementId id, std::string_view label, float& value, float min, float max,
                 const char* format = "%.2f");
bool sliderInt(Clay_ElementId id, std::string_view label, int& value, int min, int max, const char* format = "%d");
// Single line. Click or activate to edit; Enter, Escape, pad B or clicking
// elsewhere stops. Ctrl+V pastes. `maxBytes` caps the UTF-8 length.
bool textField(Clay_ElementId id, std::string& value, std::string_view hint = {}, size_t maxBytes = 64);
// A small floating note under `anchor` (drawn above everything). Call when
// you want it shown, e.g. while the anchor is hovered or nav-focused.
void tooltip(Clay_ElementId anchor, std::string_view text);
// The same at a screen position (e.g. next to the mouse), kept on screen.
void tooltipAt(glm::vec2 screenPos, std::string_view text);
// A tooltip with several lines/colors: declare its contents (ui::text...)
// between these.
void beginTooltipAt(glm::vec2 screenPos);
void endTooltip();

// An invisible, focusable box at an exact screen rectangle — for controls
// the game draws itself (like ImGui's InvisibleButton). Floating at
// `zIndex`, so it isn't moved by layout.
Context::Interaction hitbox(Clay_ElementId id, const Rect& rect, bool focusable = true, int16_t zIndex = 1);

// A floating panel: opens a layer; declare its contents, then endPanel().
//   ui::beginPanel(CLAY_ID("name"), {.width = 440, .dim = true}); ...; ui::endPanel();
void beginPanel(Clay_ElementId id, const PanelOptions& options = {});
void endPanel();

}  // namespace eng::ui
