#include "eng/dev/Overlay.h"

#include "eng/render/DrawList.h"
#include "eng/ui/Ui.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#define PSAPI_VERSION 2  // GetProcessMemoryInfo from kernel32 (K32...), no psapi.lib
#include <psapi.h>
#else
#include <fstream>
#include <sstream>
#include <unistd.h>
#endif

namespace eng::dev {

std::string fmt(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (n < int(sizeof buf)) return std::string(buf, size_t(std::max(n, 0)));
    std::string big(size_t(n) + 1, '\0');
    va_start(args, format);
    std::vsnprintf(big.data(), big.size(), format, args);
    va_end(args);
    big.resize(size_t(n));
    return big;
}

namespace {

using Clock = std::chrono::steady_clock;

float msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<float, std::milli>(b - a).count(); }

// Section colours, then the renderer's wait / submit and "other".
const glm::vec4 kSectionColors[Overlay::kMaxSections] = {
    {0.36f, 0.66f, 0.95f, 1.0f}, {0.95f, 0.66f, 0.30f, 1.0f}, {0.55f, 0.85f, 0.45f, 1.0f},
    {0.85f, 0.45f, 0.80f, 1.0f}, {0.95f, 0.85f, 0.40f, 1.0f}, {0.45f, 0.85f, 0.85f, 1.0f},
};
const glm::vec4 kWaitColor{0.33f, 0.36f, 0.42f, 1.0f};
const glm::vec4 kSubmitColor{0.62f, 0.52f, 0.85f, 1.0f};
const glm::vec4 kOtherColor{0.50f, 0.47f, 0.44f, 1.0f};
const glm::vec4 kHitchColor{0.95f, 0.30f, 0.25f, 1.0f};
const glm::vec4 kWarnColor{0.95f, 0.45f, 0.35f, 1.0f};

std::string_view sv(Clay_String s) { return {s.chars, size_t(s.length)}; }

Color32 col32(const glm::vec4& c) {
    auto b = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return ENG_COL32(b(c.r), b(c.g), b(c.b), b(c.a));
}

// The process's total CPU time in seconds, resident memory in MB, threads.
bool readProcess(double& cpuSeconds, double& rssMb, int& threads) {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    auto toSeconds = [](const FILETIME& t) {
        return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7;  // 100 ns units
    };
    cpuSeconds = toSeconds(kernel) + toSeconds(user);
    PROCESS_MEMORY_COUNTERS mem{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof mem)) rssMb = double(mem.WorkingSetSize) / (1024.0 * 1024.0);
    threads = 0;
    return true;
#else
    std::ifstream stat("/proc/self/stat");
    std::string line;
    if (!std::getline(stat, line)) return false;
    // Fields after the command name (which may hold spaces) in parentheses.
    size_t close = line.rfind(')');
    if (close == std::string::npos) return false;
    std::istringstream fields(line.substr(close + 2));
    std::string f;
    double utime = 0, stime = 0;
    long nthreads = 0;
    for (int i = 3; fields >> f; ++i) {  // field 3 is the state
        if (i == 14) utime = std::atof(f.c_str());
        if (i == 15) stime = std::atof(f.c_str());
        if (i == 20) {
            nthreads = std::atol(f.c_str());
            break;
        }
    }
    double ticks = double(sysconf(_SC_CLK_TCK));
    cpuSeconds = ticks > 0 ? (utime + stime) / ticks : 0.0;
    threads = int(nthreads);
    std::ifstream statm("/proc/self/statm");
    long size = 0, resident = 0;
    if (statm >> size >> resident) rssMb = double(resident) * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
    return true;
#endif
}

}  // namespace

// ── Sections ─────────────────────────────────────────────────────────────────

Overlay::Section::Section(Overlay* o, int index) : m_overlay(o), m_index(index), m_start(Clock::now()) {}

Overlay::Section::~Section() {
    if (m_overlay && m_index >= 0) m_overlay->m_current.sectionMs[m_index] += msBetween(m_start, Clock::now());
}

int Overlay::sectionIndex(const char* name) {
    for (int i = 0; i < m_sectionCount; ++i)
        if (m_sectionNames[i] == name || std::strcmp(m_sectionNames[i], name) == 0) return i;
    if (m_sectionCount >= kMaxSections) return -1;
    m_sectionNames[m_sectionCount] = name;
    return m_sectionCount++;
}

Overlay::Section Overlay::section(const char* name) { return Section(this, sectionIndex(name)); }

void Overlay::beginSection(const char* name) {
    endSection();
    m_openSection = sectionIndex(name);
    m_openStart = Clock::now();
}

void Overlay::endSection() {
    if (m_openSection >= 0) m_current.sectionMs[m_openSection] += msBetween(m_openStart, Clock::now());
    m_openSection = -1;
}

// ── Frames ───────────────────────────────────────────────────────────────────

Overlay::Overlay(Renderer2D& renderer) : m_renderer(renderer), m_frames(kHistory) {}

float Overlay::otherMs(const Frame& f) const {
    float known = f.waitMs + f.submitMs;
    for (int i = 0; i < m_sectionCount; ++i) known += f.sectionMs[i];
    return std::max(0.0f, f.totalMs - known);
}

void Overlay::frame() {
    endSection();
    Clock::time_point now = Clock::now();
    double at = std::chrono::duration<double>(now - m_epoch).count();
    if (m_started) {
        Frame& f = m_current;
        f.at = at;
        f.totalMs = msBetween(m_frameStart, now);
        const Renderer2D::FrameStats& rs = m_renderer.lastFrameStats();
        if (rs.frame != m_lastRendered) {  // it drew a frame since the last one (not minimized)
            f.waitMs = rs.waitMs;
            f.submitMs = rs.submitMs;
            m_lastRendered = rs.frame;
        }
        // Median of the recent frames, for hitch detection.
        if (m_count >= 30) {
            std::vector<float> recent;
            size_t n = std::min<size_t>(m_count, 120);
            recent.reserve(n);
            for (size_t k = 1; k <= n; ++k) recent.push_back(m_frames[(m_next + kHistory - k) % kHistory].totalMs);
            std::nth_element(recent.begin(), recent.begin() + long(n / 2), recent.end());
            float median = recent[n / 2];
            if (f.totalMs > std::max(2.0f * median, median + 4.0f)) {
                f.hitch = true;
                Hitch h{at, f.totalMs, "other", otherMs(f)};
                auto consider = [&](const char* name, float ms) {
                    if (ms > h.culpritMs) h.culprit = name, h.culpritMs = ms;
                };
                for (int i = 0; i < m_sectionCount; ++i) consider(m_sectionNames[i], f.sectionMs[i]);
                consider("GPU / vsync wait", f.waitMs);
                consider("submit", f.submitMs);
                m_hitches.push_back(h);
                if (m_hitches.size() > 8) m_hitches.erase(m_hitches.begin());
            }
        }
        m_frames[m_next] = f;
        m_next = (m_next + 1) % kHistory;
        m_count = std::min<size_t>(m_count + 1, kHistory);
    }
    m_current = Frame{};
    m_frameStart = now;
    m_started = true;
    if (at - m_lastProcessSample >= 0.5) sampleProcess(at);
}

void Overlay::sampleProcess(double now) {
    double cpu = 0.0, rss = m_process.rssMb;
    int threads = 0;
    if (!readProcess(cpu, rss, threads)) return;
    if (m_lastProcessSample >= 0.0 && now > m_lastProcessSample)
        m_process.cpuPercent = (cpu - m_lastCpuSeconds) / (now - m_lastProcessSample) * 100.0;
    m_process.rssMb = rss;
    m_process.threads = threads;
    m_process.cores = int(std::thread::hardware_concurrency());
    m_lastCpuSeconds = cpu;
    m_lastProcessSample = now;
}

Overlay::Summary Overlay::summary(float seconds) const {
    Summary s;
    if (m_count == 0) return s;
    double newest = m_frames[(m_next + kHistory - 1) % kHistory].at;
    std::vector<float> totals;
    for (size_t k = 1; k <= m_count; ++k) {
        const Frame& f = m_frames[(m_next + kHistory - k) % kHistory];
        if (newest - f.at > seconds) break;
        totals.push_back(f.totalMs);
        s.avgMs += f.totalMs;
        s.worstMs = std::max(s.worstMs, f.totalMs);
        s.hitches += f.hitch;
        for (int i = 0; i < m_sectionCount; ++i) s.sectionAvgMs[i] += f.sectionMs[i];
        s.waitAvgMs += f.waitMs;
        s.submitAvgMs += f.submitMs;
        s.otherAvgMs += otherMs(f);
    }
    s.frames = int(totals.size());
    if (s.frames == 0) return s;
    float n = float(s.frames);
    s.avgMs /= n;
    for (float& v : s.sectionAvgMs) v /= n;
    s.waitAvgMs /= n;
    s.submitAvgMs /= n;
    s.otherAvgMs /= n;
    size_t p = std::min(totals.size() - 1, size_t(float(totals.size()) * 0.99f));
    std::nth_element(totals.begin(), totals.begin() + long(p), totals.end());
    s.lowMs = totals[p];
    s.fps = s.avgMs > 0.0f ? 1000.0f / s.avgMs : 0.0f;
    s.lowFps = s.lowMs > 0.0f ? 1000.0f / s.lowMs : 0.0f;
    return s;
}

// ── Game stats and panels ────────────────────────────────────────────────────

Overlay::Stat& Overlay::statSlot(std::string_view key) {
    size_t slash = key.find('/');
    std::string_view group = slash == std::string_view::npos ? std::string_view("game") : key.substr(0, slash);
    std::string_view name = slash == std::string_view::npos ? key : key.substr(slash + 1);
    for (Stat& s : m_stats)
        if (s.group == group && s.name == name) return s;
    // Keep groups together, in the order each first appeared.
    auto at = m_stats.end();
    for (auto it = m_stats.begin(); it != m_stats.end(); ++it)
        if (it->group == group) at = it + 1;
    return *m_stats.insert(at, Stat{std::string(group), std::string(name), {}});
}

void Overlay::stat(std::string_view key, double value, const char* format) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, value);
    statSlot(key).value = buf;
}

void Overlay::stat(std::string_view key, std::string_view text) { statSlot(key).value = std::string(text); }

void Overlay::addPanel(std::string title, std::function<void()> draw) { m_panels.push_back({std::move(title), std::move(draw)}); }

// ── Drawing ──────────────────────────────────────────────────────────────────

void Overlay::drawGraph(Renderer2D& r, const Rect& box, FontId font, float fontPx) {
    DrawList dl(r);
    dl.rectFilled(box.pos, box.pos + box.size, ENG_COL32(0, 0, 0, 60), 3.0f);
    float budget = 1000.0f / std::max(1.0f, m_targetFps);
    // Scale: two budgets, or more when recent frames need it (up to 100 ms).
    float top = 2.0f * budget;
    for (size_t k = 0; k < m_count; ++k) top = std::max(top, std::min(100.0f, m_frames[k].totalMs * 1.1f));
    auto yOf = [&](float ms) { return box.pos.y + box.size.y * (1.0f - std::min(ms, top) / top); };

    float barW = box.size.x / float(kHistory);
    for (size_t k = 0; k < m_count; ++k) {
        const Frame& f = m_frames[(m_next + kHistory - m_count + k) % kHistory];
        float x0 = box.pos.x + box.size.x - float(m_count - k) * barW;
        float x1 = x0 + std::max(1.0f, barW - 0.5f);
        float base = 0.0f;
        auto stack = [&](float ms, const glm::vec4& c) {
            if (ms <= 0.0f) return;
            dl.rectFilled({x0, yOf(base + ms)}, {x1, yOf(base)}, col32(c));
            base += ms;
        };
        for (int i = 0; i < m_sectionCount; ++i) stack(f.sectionMs[i], kSectionColors[i]);
        stack(f.submitMs, kSubmitColor);
        stack(otherMs(f), kOtherColor);
        stack(f.waitMs, kWaitColor);
        if (f.hitch) dl.rectFilled({x0, box.pos.y}, {x1, box.pos.y + 4.0f}, col32(kHitchColor));
    }
    for (float line : {budget, 2.0f * budget}) {
        if (line > top) continue;
        float y = std::round(yOf(line));
        dl.line({box.pos.x, y}, {box.pos.x + box.size.x, y}, ENG_COL32(255, 255, 255, 70));
        char label[16];
        std::snprintf(label, sizeof label, "%.1f ms", line);
        r.drawText(font, fontPx, {box.pos.x + 4.0f, y + 2.0f}, {1.0f, 1.0f, 1.0f, 0.55f}, label);  // below its line, inside the graph
    }
}

void Overlay::draw(ui::Context& ui) {
    if (!m_visible) return;
    const ui::Theme& th = ui.theme;
    const uint16_t gap = uint16_t(ui.px(4));

    // name ............ value, one row.
    auto row = [&](Clay_ElementId id, std::string_view name, std::string_view value, const glm::vec4& valueColor = glm::vec4(-1.0f)) {
        CLAY(id, {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = gap}}) {
            CLAY(CLAY_IDI("eng_dev_name", id.id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {
                ui::text(name, {.small = true, .dim = true});
            }
            ui::text(value, {.small = true, .color = valueColor});
        }
    };
    auto heading = [&](std::string_view title) { ui::text(title, {.color = th.accent}); };
    auto swatch = [&](const glm::vec4& c) {
        CLAY_AUTO_ID({.layout = {.sizing = {CLAY_SIZING_FIXED(ui.px(10)), CLAY_SIZING_FIXED(ui.px(10))}},
                      .backgroundColor = ui::color(c),
                      .cornerRadius = ui::radius(ui.px(2))}) {}
    };

    ui::beginPanel(CLAY_ID("eng_dev_overlay"),
                   {.width = 460, .maxHeight = 1040, .anchor = {0.0f, 0.0f}, .offset = {12.0f, 12.0f}, .padX = 14,
                    .padY = 12, .gap = 6, .zIndex = 900,
                    .capture = false,  // read-only: clicks go through to whatever's under it
                    .scroll = true,
                    .color = {th.panel.r, th.panel.g, th.panel.b, m_opacity}});

    // ── Frame ──
    Summary s = summary(2.0f);
    heading("Frame");
    ui::text(sv(ui.fmt("%.0f fps   %.2f ms avg   1%% low %.0f fps   worst %.1f ms", s.fps, s.avgMs, s.lowFps, s.worstMs)));
    FontId font = th.font;
    float fontPx = float(ui.fontPx(th.smallFontSize * 0.8f));
    CLAY(CLAY_ID("eng_dev_graph"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(ui.px(120))}},
                                    .custom = ui.custom([this, font, fontPx](Renderer2D& r, const Rect& b) { drawGraph(r, b, font, fontPx); })}) {}
    // Legend with the 2 s averages.
    auto legend = [&](Clay_ElementId id, const glm::vec4& c, std::string_view name, float ms) {
        CLAY(id, {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                             .childGap = uint16_t(ui.px(6)),
                             .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
            swatch(c);
            CLAY(CLAY_IDI("eng_dev_legend_name", id.id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {
                ui::text(name, {.small = true, .dim = true});
            }
            ui::text(sv(ui.fmt("%.2f ms", ms)), {.small = true});
        }
    };
    for (int i = 0; i < m_sectionCount; ++i) legend(CLAY_IDI("eng_dev_leg", i), kSectionColors[i], m_sectionNames[i], s.sectionAvgMs[i]);
    legend(CLAY_ID("eng_dev_leg_submit"), kSubmitColor, "submit (record, present)", s.submitAvgMs);
    legend(CLAY_ID("eng_dev_leg_other"), kOtherColor, "other", s.otherAvgMs);
    legend(CLAY_ID("eng_dev_leg_wait"), kWaitColor, "GPU / vsync wait", s.waitAvgMs);

    // ── Hitches ──
    heading(sv(ui.fmt("Hitches: %d in the last 2 s", s.hitches)));
    if (m_hitches.empty()) ui::text("none yet", {.small = true, .dim = true});
    for (size_t i = m_hitches.size(); i-- > 0 && m_hitches.size() - i <= 4;) {
        const Hitch& h = m_hitches[i];
        row(CLAY_IDI("eng_dev_hitch", int(i)), sv(ui.fmt("%.1f s", h.at)), sv(ui.fmt("%.1f ms   %s %.1f ms", h.totalMs, h.culprit, h.culpritMs)),
            kWarnColor);
    }

    // ── Renderer ──
    const Renderer2D::FrameStats& rs = m_renderer.lastFrameStats();
    heading("Renderer");
    row(CLAY_ID("eng_dev_present"), "present mode", m_renderer.presentModeName());
    row(CLAY_ID("eng_dev_quads"), "quads", sv(ui.fmt("%u / %u", rs.quads, rs.quadCapacity)));
    row(CLAY_ID("eng_dev_batches"), "draw calls", sv(ui.fmt("%u  (+%u lightmap)", rs.batches, rs.lightBatches)));
    if (rs.droppedQuads) row(CLAY_ID("eng_dev_dropped"), "dropped quads", sv(ui.fmt("%u: over capacity", rs.droppedQuads)), kWarnColor);

    // ── Process ──
    heading("Process");
    row(CLAY_ID("eng_dev_mem"), "memory", sv(ui.fmt("%.0f MB", m_process.rssMb)));
    if (m_process.cores > 0)
        row(CLAY_ID("eng_dev_cpu"), "CPU",
            sv(ui.fmt("%.0f%% of %d cores  (%.2f busy)", m_process.cpuPercent / m_process.cores, m_process.cores, m_process.cpuPercent / 100.0)));
    else
        row(CLAY_ID("eng_dev_cpu"), "CPU", sv(ui.fmt("%.2f cores busy", m_process.cpuPercent / 100.0)));
    if (m_process.threads) row(CLAY_ID("eng_dev_threads"), "threads", sv(ui.fmt("%d", m_process.threads)));

    // ── The game's ──
    const std::string* group = nullptr;
    for (size_t i = 0; i < m_stats.size(); ++i) {
        const Stat& st = m_stats[i];
        if (!group || *group != st.group) {
            heading(st.group);
            group = &st.group;
        }
        row(CLAY_IDI("eng_dev_stat", int(i)), st.name, st.value);
    }
    for (const Panel& p : m_panels) {
        heading(p.title);
        p.draw();
    }
    ui::endPanel();
}

}  // namespace eng::dev
