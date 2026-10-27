# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`eng` — the shared C++20 engine library for these games (currently used by `../delve`; `../arena` is meant to migrate onto it — its Vulkan core, crash log and Steam code were extracted from arena and kept API-compatible apart from the `eng` namespace). Static library, consumed via `add_subdirectory`:

```cmake
set(ENGINE_DIR "${CMAKE_SOURCE_DIR}/../engine" CACHE PATH "")
add_subdirectory(${ENGINE_DIR} ${CMAKE_BINARY_DIR}/engine)
target_link_libraries(mygame PRIVATE eng)
eng_setup_runtime(mygame ${CMAKE_SOURCE_DIR}/steam_appid.txt)  # rpath, steam lib + appid copy, static MinGW runtime
```

No standalone build or tests — it's built and exercised through a game (`../delve`'s build, `delve_simtest`, and its `--screenshot` visual checks).

## Modules (all namespace `eng`, headers under `include/eng/`)

- `core/` — `CrashLog` (tees stdout/stderr to `<app>_log.txt`, catches signals/SEH/terminate; call `crashlog::init` first thing in main), `Log.h` (`ENG_LOG`).
- `platform/` — `Window` (GLFW, no-API, resize flag, fullscreen toggle), `Input` (per-frame key/mouse/gamepad snapshot with pressed/released edges; mouse in framebuffer pixels).
- `render/` — `VulkanContext`, `Swapchain` (now takes `srgb`/`depth` options; arena's defaults preserved), `VulkanUtils`, and `Renderer2D`:
  - Immediate-mode sprites (`draw(Sprite)`, `drawRect/Circle/Line`), drawn in submission order, batched by texture/blend/space. Builtin textures `kWhite`, `kCircle`.
  - `Space::World` (camera: center + pixels-per-unit, y-down) vs `Space::Screen` (pixels).
  - Dear ImGui (v1.91.0, FetchContent) is initialized and rendered on top every frame; games call `ImGui::*` freely between `beginFrame()`/`endFrame()`. Sprite textures get their descriptor sets from `ImGui_ImplVulkan_AddTexture`, so the sprite pipeline's set layout must stay identical to ImGui's (one combined image sampler, binding 0, fragment) — and any texture can be shown with `ImGui::Image(renderer.imguiTexture(id), ...)`.
  - Shaders (`shaders/`) are compiled at build time with `glslc -mfmt=c` and embedded — no runtime shader files.
  - UNORM swapchain (not sRGB) so pixel-art colors and ImGui pass through untouched.
  - `beginFrame()` returns false while minimized instead of blocking — a host's simulation must keep ticking.
  - `requestScreenshot(path)` reads back the next presented frame to PNG (stb_image_write).
  - Blend modes: Alpha, Additive, Modulate2x (dst × src × 2 — 0.5 grey is neutral). `drawTriangle` for fans/polygons (a quad with a degenerate second triangle, so it batches with sprites).
  - **Lighting**: a half-resolution, linearly-filtered lightmap (one per frame in flight, `R8G8B8A8_UNORM`, its own render pass recorded before the main one). `beginLights(ambient)` … `endLights()` routes every draw into the lightmap's batch list (same vertex buffer); `compositeLights()` puts a Modulate2x screen quad of it into the scene stream at that point, so what's drawn before is lit and what's after isn't. Frames that never composite skip the pass. The lightmap images are recreated with the swapchain; their texture slots keep their ids. Shadow geometry is the game's job (Delve: `client/Lighting.cpp`).
- `audio/` — `Audio` (miniaudio): named one-shots with N voices, one streamed music track; silent no-op if the device or file is missing.
- `net/`
  - `Transport` interface: message-oriented, `send(peer, bytes, Reliable|Unreliable)`, `poll()` → Data/Disconnected events, `pingMs`. No accept step — the game protocol decides who's in.
  - `UdpTransport`: plain UDP + minimal reliability (seq/ack/resend, in-order delivery), keepalive pings, 6s timeout. For LAN/localhost testing.
  - `SteamTransport`: ISteamNetworkingMessages (P2P with relay fallback); `acceptPeer` callback gates unsolicited sessions.
  - `SteamPlatform`: SteamAPI init, persona/SteamID, lobbies (create/join/leave/invite overlay/data), `+connect_lobby` parsing, event queue (incl. `JoinRequested` — the game decides whether to drop its session). Everything no-ops when Steam is absent or `ENG_STEAM=OFF`.
  - `ByteWriter` / `ByteReader`: little-endian, varints, int16 fixed-point. The reader is bounds-checked and latches `ok()` false — use it for all untrusted bytes.

## Gotchas

- **MinGW + Steam:** Valve's `steam_api64.dll` is MSVC-built; OOP accessors that *return* `CSteamID` by value crash intermittently under MinGW. Use the flat API (`SteamAPI_ISteam*_*`, returns uint64) for anything returning a SteamID — see `SteamPlatform.cpp`.
- Windows cross-compile needs the `WINDOWS_*` cache vars; `scripts/windows-deps.sh` (`eng_windows_deps`) fetches GLFW/Vulkan import lib/headers into `deps/windows` once.
- `eng_setup_runtime` runs in the caller's scope — it recomputes the SDK path from `CMAKE_CURRENT_FUNCTION_LIST_DIR` rather than relying on engine-scope variables.
- Steam app id is Valve's test id 480 in each game's `steam_appid.txt` until real ones exist.
