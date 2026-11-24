// eng::ui demo: every widget, a scroll list, an image and a custom draw, laid
// out with CLAY() from C++ the way a game would. Also the engine's visual
// check for the UI layer:
//
//   zig build ui-demo                                  # interactive (Esc quits)
//   zig build ui-demo -- --screenshot ui.png           # render, save, quit
//   zig build ui-demo -- --fonts ../delve/Assets/Fonts # where the TTFs live
//   zig build ui-demo -- --scale 1                     # fixed UI scale (default: height / 1080)
#include "eng/platform/Input.h"
#include "eng/platform/Window.h"
#include "eng/render/Renderer2D.h"
#include "eng/ui/Ui.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ui = eng::ui;

int main(int argc, char** argv) {
    std::string screenshot;
    std::string fonts = "../delve/Assets/Fonts";
    float fixedScale = 0.0f;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--screenshot")) screenshot = argv[++i];
        else if (!std::strcmp(argv[i], "--fonts")) fonts = argv[++i];
        else if (!std::strcmp(argv[i], "--scale")) fixedScale = std::strtof(argv[++i], nullptr);
    }

    eng::Window window("eng::ui demo", 1280, 720);
    eng::Renderer2D::Options options;
    options.appName = "ui-demo";
    eng::Renderer2D renderer(window, options);
    eng::Input input;
    ui::Context ctx(renderer, window);
    ctx.theme.font = renderer.loadFont(fonts + "/Alegreya.ttf");
    ctx.theme.titleFont = renderer.loadFont(fonts + "/IMFellEnglishSC-Regular.ttf");

    bool music = true, subtitles = false;
    float volume = 0.7f;
    int players = 2;
    std::string name = "Wanderer", address;
    int plays = 0;

    auto last = std::chrono::steady_clock::now();
    for (int frame = 0; !window.shouldClose(); ++frame) {
        window.pollEvents();
        input.update(window.handle());
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (!renderer.beginFrame()) continue;

        // Something in the world behind the UI.
        renderer.setCamera({0.0f, 0.0f}, 40.0f);
        for (int x = -16; x <= 16; ++x)
            for (int y = -9; y <= 9; ++y)
                renderer.drawCircle({float(x), float(y)}, 0.12f + 0.08f * std::sin(float(frame) * 0.05f + float(x + y)),
                                    {0.35f, 0.25f, 0.45f, 1.0f});

        ctx.setScale(fixedScale > 0.0f ? fixedScale : renderer.screenSize().y / 1080.0f);
        if (!screenshot.empty() && frame == 3) ctx.startEditing(CLAY_ID("name"));  // show the caret in the capture
        ctx.begin(input, dt);
        const ui::Theme& th = ctx.theme;
        auto gap = uint16_t(ctx.px(th.gap * 1.5f));

        CLAY(CLAY_ID("root"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                          .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}}) {
            auto pad = uint16_t(ctx.px(28));
            auto edge = uint16_t(ctx.px(2));
            CLAY(CLAY_ID("panel"), {.layout = {.sizing = {CLAY_SIZING_FIXED(ctx.px(820)), CLAY_SIZING_FIT(0)},
                                               .padding = {pad, pad, pad, pad},
                                               .childGap = gap,
                                               .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                    .backgroundColor = ui::color(th.panel),
                                    .cornerRadius = ui::radius(ctx.px(14)),
                                    .border = {.color = ui::color(th.accent * glm::vec4(1, 1, 1, 0.6f)),
                                               .width = {edge, edge, edge, edge, 0}}}) {
                ui::text("Settings", {.title = true});
                ui::text("Clay lays this panel out, Renderer2D draws it, and eng::ui adds the widgets: mouse, "
                         "keyboard (arrows, Tab, Enter) and gamepad (d-pad or stick, A, B) all work. Long text "
                         "like this wraps on its own.",
                         {.dim = true});

                CLAY(CLAY_ID("buttons"), {.layout = {.childGap = gap, .childAlignment = {CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER}}}) {
                    if (ui::button(CLAY_ID("play"), "Play")) ++plays;
                    ui::button(CLAY_ID("options"), "Options");
                    ui::button(CLAY_ID("quit"), "Quit (disabled)", {.disabled = true});
                    ui::text("Played " + std::to_string(plays) + " times", {.small = true, .dim = true});
                }

                CLAY(CLAY_ID("checks"), {.layout = {.childGap = gap}}) {
                    ui::checkbox(CLAY_ID("music"), "Music", music);
                    ui::checkbox(CLAY_ID("subs"), "Subtitles", subtitles);
                }
                ui::sliderFloat(CLAY_ID("volume"), "Volume", volume, 0.0f, 1.0f, "%.2f");
                ui::sliderInt(CLAY_ID("players"), "Players", players, 1, 4);
                ui::textField(CLAY_ID("name"), name, "Your name", 24);
                ui::textField(CLAY_ID("address"), address, "Host address, e.g. 192.168.1.20", 64);

                auto inset = uint16_t(ctx.px(8));
                CLAY(CLAY_ID("lower"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .childGap = gap}}) {
                    CLAY(CLAY_ID("slots"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(ctx.px(170))},
                                                       .padding = {inset, inset, inset, inset},
                                                       .childGap = uint16_t(ctx.px(4)),
                                                       .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                            .backgroundColor = ui::color(th.field),
                                            .cornerRadius = ui::radius(ctx.px(th.radius)),
                                            .clip = ctx.scrollY()}) {
                        for (int i = 0; i < 12; ++i)
                            ui::button(CLAY_IDI("slot", i), "Save slot " + std::to_string(i + 1), {.grow = true, .small = true});
                    }
                    CLAY(CLAY_ID("art"), {.layout = {.childGap = gap,
                                                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER},
                                                     .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                        CLAY(CLAY_ID("image"), {.layout = {.sizing = {CLAY_SIZING_FIXED(ctx.px(64)), CLAY_SIZING_FIXED(ctx.px(64))}},
                                                .image = ctx.image(eng::Renderer2D::kCircle, th.accent)}) {}
                        CLAY(CLAY_ID("custom"), {.layout = {.sizing = {CLAY_SIZING_FIXED(ctx.px(120)), CLAY_SIZING_FIXED(ctx.px(64))}},
                                                 .custom = ctx.custom([t = float(frame)](eng::Renderer2D& r, const eng::Rect& b) {
                                                     for (int k = 0; k < 12; ++k) {
                                                         float x0 = b.pos.x + b.size.x * float(k) / 12.0f;
                                                         float x1 = b.pos.x + b.size.x * float(k + 1) / 12.0f;
                                                         auto y = [&](float x) {
                                                             return b.pos.y + b.size.y * (0.5f + 0.4f * std::sin(x * 0.08f + t * 0.1f));
                                                         };
                                                         r.drawLine({x0, y(x0)}, {x1, y(x1)}, 3.0f, {0.86f, 0.63f, 0.31f, 1.0f},
                                                                    eng::Space::Screen);
                                                     }
                                                 })}) {}
                    }
                }
            }
        }
        if (ctx.backPressed()) window.requestClose();
        ctx.end();

        if (!screenshot.empty() && frame == 20) renderer.requestScreenshot(screenshot);
        renderer.endFrame();
        if (!screenshot.empty() && frame == 21) break;
    }
    return 0;
}
