#pragma once

// eng::dev::Overlay — a developer overlay drawn with eng::ui: frame timing
// (fps, 1% lows, a per-frame graph split by what the frame spent its time
// on), jank detection, renderer and process stats, and whatever stats and
// panels the game adds. Collecting is cheap and always on; drawing only
// while it's visible.
//
//   eng::dev::Overlay overlay(renderer);
//   loop:
//     overlay.frame();                                  // first thing
//     { auto s = overlay.section("update"); ...update... }
//     if (!renderer.beginFrame()) continue;
//     { auto s = overlay.section("draw"); ...draw...; ui.begin(...); ...; overlay.draw(ui); ui.end(); }
//     renderer.endFrame();
//     overlay.stat("net/ping", pingMs, "%.0f ms");
//
// Don't put Renderer2D::beginFrame/endFrame inside a section: the renderer
// times those itself (waitMs: the GPU / vsync wait; submitMs), and the
// overlay shows them as their own parts of the frame. Whatever a frame spent
// outside sections and the renderer shows as "other".
//
// A hitch is a frame slower than both twice the recent median and the
// median + 4 ms; the overlay keeps the last few with the part that took the
// most time.

#include "eng/render/Renderer2D.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace eng::ui {
class Context;
}

namespace eng::dev {

// printf into a std::string (for stat() values and logs).
std::string fmt(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

class Overlay {
public:
    static constexpr int kHistory = 300;    // frames kept for the graph and summaries
    static constexpr int kMaxSections = 6;  // distinct section names (more are ignored)

    explicit Overlay(Renderer2D& renderer);

    // ── Per frame ──
    // First thing each frame: closes the previous frame's record.
    void frame();

    // Times a named part of the frame (CPU, wall clock) until it goes out of
    // scope. Sections with the same name add up within a frame. `name` is
    // kept by pointer: use a literal.
    class Section {
    public:
        ~Section();
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;

    private:
        friend class Overlay;
        Section(Overlay* o, int index);
        Overlay* m_overlay;
        int m_index;
        std::chrono::steady_clock::time_point m_start;
    };
    [[nodiscard]] Section section(const char* name);
    // The same without a scope, for loops with early exits: one open at a
    // time (beginning another, or frame(), ends it).
    void beginSection(const char* name);
    void endSection();

    // ── The game's own stats ──
    // "group/name" (the group is a heading; groups and names show in the
    // order first set). Shown until set again.
    void stat(std::string_view key, double value, const char* format = "%.1f");
    void stat(std::string_view key, std::string_view text);
    // A panel of the game's own (eng::ui widgets), drawn under the built-in
    // ones while the overlay is visible.
    void addPanel(std::string title, std::function<void()> draw);

    bool visible() const { return m_visible; }
    void setVisible(bool v) { m_visible = v; }
    void toggle() { m_visible = !m_visible; }
    void setTargetFps(float fps) { m_targetFps = fps; }  // the graph's budget line (default 60)
    void setOpacity(float a) { m_opacity = a; }           // the panel's background (default 0.55: the game shows through)

    // Between ui.begin() and ui.end(). Nothing while hidden.
    void draw(ui::Context& ui);

    // ── Numbers, for logs too ──
    struct Summary {
        int frames = 0;
        float avgMs = 0.0f, lowMs = 0.0f, worstMs = 0.0f;  // low: the 99th percentile frame time
        float fps = 0.0f, lowFps = 0.0f;                    // 1000 / avg, 1000 / low (the "1% low")
        int hitches = 0;
        std::array<float, kMaxSections> sectionAvgMs{};
        float waitAvgMs = 0.0f, submitAvgMs = 0.0f, otherAvgMs = 0.0f;
    };
    Summary summary(float seconds = 2.0f) const;  // over the most recent `seconds`
    int sectionCount() const { return m_sectionCount; }
    const char* sectionName(int i) const { return m_sectionNames[i]; }

    struct ProcessStats {
        double rssMb = 0.0;       // resident / working set
        double cpuPercent = 0.0;  // of one core (can pass 100 with threads)
        int threads = 0;          // 0 where unknown (Windows)
    };
    const ProcessStats& process() const { return m_process; }

    struct Hitch {
        double at = 0.0;  // seconds since the overlay started
        float totalMs = 0.0f;
        const char* culprit = "";  // the part of the frame that took longest
        float culpritMs = 0.0f;
    };
    const std::vector<Hitch>& hitches() const { return m_hitches; }  // oldest first, the last 8

private:
    struct Frame {
        double at = 0.0;
        float totalMs = 0.0f;
        std::array<float, kMaxSections> sectionMs{};
        float waitMs = 0.0f, submitMs = 0.0f;
        bool hitch = false;
    };
    struct Stat {
        std::string group, name, value;
    };
    struct Panel {
        std::string title;
        std::function<void()> draw;
    };

    float otherMs(const Frame& f) const;
    void sampleProcess(double now);
    void drawGraph(Renderer2D& r, const Rect& box, FontId font, float fontPx);
    Stat& statSlot(std::string_view key);

    Renderer2D& m_renderer;
    std::chrono::steady_clock::time_point m_epoch = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_frameStart{};
    bool m_started = false;
    uint64_t m_lastRendered = 0;  // the renderer's frame count last time
    Frame m_current;
    std::vector<Frame> m_frames;  // ring of kHistory
    size_t m_next = 0, m_count = 0;

    std::array<const char*, kMaxSections> m_sectionNames{};
    int m_sectionCount = 0;
    int sectionIndex(const char* name);
    int m_openSection = -1;
    std::chrono::steady_clock::time_point m_openStart{};

    std::vector<Hitch> m_hitches;
    std::vector<Stat> m_stats;
    std::vector<Panel> m_panels;
    ProcessStats m_process;
    double m_lastProcessSample = -1.0, m_lastCpuSeconds = 0.0;
    bool m_visible = false;
    float m_targetFps = 60.0f;
    float m_opacity = 0.55f;
};

}  // namespace eng::dev
