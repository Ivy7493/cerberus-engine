//! eng — shared engine library (2D Vulkan renderer, input, audio, Steam/UDP
//! networking), built as a static library with GLFW compiled in.
//!
//!   zig build                            # → zig-out/lib/libeng.a + headers
//!   zig build --watch -fincremental      # rebuild on save
//!   zig build -Dsteam=false              # drop Steamworks
//!   zig build -Dtarget=x86_64-linux-gnu.2.31   # portable Linux (any distro with glibc ≥ 2.31)
//!   zig build -Dtarget=x86_64-windows-gnu      # Windows, cross-compiled
//!
//! Headers for Vulkan, GLM, X11, xkbcommon and Wayland come from pkg-config
//! (the devenv provides them) for every target, so the native, old-glibc and
//! Windows builds all take the same path. Nothing links against X11/Wayland:
//! GLFW dlopen()s them at runtime.
//!
//! Zig games consume it as a dependency:
//!
//!   const eng_build = @import("eng");
//!   const eng = b.dependency("eng", .{ .target = target, .optimize = optimize, .steam = steam });
//!   eng_build.link(b, exe, eng, steam);
//!   if (steam) eng_build.installSteamRuntime(b, eng, exe, b.path("steam_appid.txt"));
const std = @import("std");

const eng_sources = [_][]const u8{
    "src/core/CrashLog.cpp",
    "src/platform/Window.cpp",
    "src/platform/Input.cpp",
    "src/render/VulkanContext.cpp",
    "src/render/Swapchain.cpp",
    "src/render/VulkanUtils.cpp",
    "src/render/Renderer2D.cpp",
    "src/render/StbImage.cpp",
    "src/net/UdpSocket.cpp",
    "src/net/UdpTransport.cpp",
    "src/net/SteamPlatform.cpp",
    "src/net/SteamTransport.cpp",
    "src/net/LagTransport.cpp",
};

const imgui_sources = [_][]const u8{
    "imgui.cpp",
    "imgui_draw.cpp",
    "imgui_tables.cpp",
    "imgui_widgets.cpp",
    "backends/imgui_impl_glfw.cpp",
    "backends/imgui_impl_vulkan.cpp",
};

const glfw_common = [_][]const u8{
    "context.c",     "init.c",         "input.c",       "monitor.c",
    "platform.c",    "vulkan.c",       "window.c",      "egl_context.c",
    "osmesa_context.c", "null_init.c", "null_monitor.c", "null_window.c",
    "null_joystick.c",
};
const glfw_linux = [_][]const u8{
    "posix_time.c",  "posix_thread.c", "posix_module.c", "posix_poll.c",
    "linux_joystick.c", "xkb_unicode.c",
    "x11_init.c",    "x11_monitor.c",  "x11_window.c",   "glx_context.c",
    "wl_init.c",     "wl_monitor.c",   "wl_window.c",
};
const glfw_windows = [_][]const u8{
    "win32_time.c",  "win32_thread.c", "win32_module.c", "win32_init.c",
    "win32_joystick.c", "win32_monitor.c", "win32_window.c", "wgl_context.c",
};
const wayland_protocols = [_][]const u8{
    "wayland",                           "viewporter",
    "xdg-shell",                         "idle-inhibit-unstable-v1",
    "pointer-constraints-unstable-v1",   "relative-pointer-unstable-v1",
    "fractional-scale-v1",               "xdg-activation-v1",
    "xdg-decoration-unstable-v1",
};

/// Header-only pkg-config packages: their include dirs are all eng needs.
const pc_headers_common = [_][]const u8{ "vulkan", "glm" };
const pc_headers_linux = [_][]const u8{
    "x11", "xcursor", "xrandr", "xinerama", "xi", "xext", "xrender", "xfixes", "xkbcommon", "wayland-client",
};

const shaders = [_][]const u8{ "sprite.vert", "sprite.frag" };

const steam_sdk = "third_party/steamworks";

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const steam = b.option(bool, "steam", "Link Steamworks (lobbies + P2P transport)") orelse true;
    const os = target.result.os.tag;
    if (os != .linux and os != .windows) @panic("eng builds for Linux and Windows only");
    const is_windows = os == .windows;

    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    addPublicMacros(mod, is_windows, steam);

    for (pc_headers_common) |pkg| addPkgConfigIncludes(b, mod, pkg);

    const cxx_flags = [_][]const u8{
        "-std=c++20",
        "-Wno-nontrivial-memcall", // imgui's memset(this, ...) constructors
    };

    // ── GLFW 3.4, built from source ──────────────────────────────────────────
    // Its own library: GLFW's sources refuse the header-option macros
    // (GLFW_INCLUDE_NONE) that eng and games are compiled with.
    const glfw = b.dependency("glfw", .{});
    const glfw_mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    glfw_mod.addIncludePath(glfw.path("include"));
    glfw_mod.addCSourceFiles(.{ .root = glfw.path("src"), .files = &glfw_common, .flags = glfwFlags(is_windows) });
    if (is_windows) {
        glfw_mod.addCSourceFiles(.{ .root = glfw.path("src"), .files = &glfw_windows, .flags = glfwFlags(true) });
        for ([_][]const u8{ "gdi32", "user32", "shell32" }) |lib| glfw_mod.linkSystemLibrary(lib, .{});
    } else {
        for (pc_headers_linux) |pkg| addPkgConfigIncludes(b, glfw_mod, pkg);
        glfw_mod.addCSourceFiles(.{ .root = glfw.path("src"), .files = &glfw_linux, .flags = glfwFlags(false) });
        const protocols = b.addWriteFiles();
        for (wayland_protocols) |name| {
            const xml = glfw.path(b.fmt("deps/wayland/{s}.xml", .{name}));
            for ([_][2][]const u8{ .{ "client-header", "-client-protocol.h" }, .{ "private-code", "-client-protocol-code.h" } }) |kind| {
                const scan = b.addSystemCommand(&.{ "wayland-scanner", kind[0] });
                scan.addFileArg(xml);
                const out_name = b.fmt("{s}{s}", .{ name, kind[1] });
                _ = protocols.addCopyFile(scan.addOutputFileArg(out_name), out_name);
            }
        }
        glfw_mod.addIncludePath(protocols.getDirectory());
        for ([_][]const u8{ "dl", "pthread", "m" }) |lib| glfw_mod.linkSystemLibrary(lib, .{});
    }
    const glfw_lib = b.addLibrary(.{ .name = "glfw3", .linkage = .static, .root_module = glfw_mod });
    mod.linkLibrary(glfw_lib);
    mod.addIncludePath(glfw.path("include"));
    if (is_windows) {
        for ([_][]const u8{ "ws2_32", "winmm" }) |lib| mod.linkSystemLibrary(lib, .{});
    } else {
        for ([_][]const u8{ "dl", "pthread", "m" }) |lib| mod.linkSystemLibrary(lib, .{});
    }

    // ── Dear ImGui (built into eng; games include <imgui.h> through it) ──────
    const imgui = b.dependency("imgui", .{});
    mod.addCSourceFiles(.{ .root = imgui.path(""), .files = &imgui_sources, .flags = &cxx_flags });
    mod.addIncludePath(imgui.path(""));
    mod.addIncludePath(imgui.path("backends"));

    // ── Shaders → SPIR-V embedded as C arrays (no runtime shader files) ──────
    const gen = b.addWriteFiles();
    for (shaders) |shader| {
        const glslc = b.addSystemCommand(&.{ "glslc", "-mfmt=c" });
        glslc.addFileArg(b.path(b.fmt("shaders/{s}", .{shader})));
        glslc.addArg("-o");
        const inc = glslc.addOutputFileArg(b.fmt("{s}.inc", .{shader}));
        _ = gen.addCopyFile(inc, b.fmt("{s}.inc", .{shader}));
    }
    mod.addIncludePath(gen.getDirectory());

    // ── eng itself ───────────────────────────────────────────────────────────
    mod.addCSourceFiles(.{ .files = &eng_sources, .flags = &cxx_flags });
    // miniaudio's implementation TU trips plenty of warnings; keep eng quiet.
    mod.addCSourceFile(.{ .file = b.path("src/audio/Audio.cpp"), .flags = &(cxx_flags ++ .{"-w"}) });
    mod.addIncludePath(b.path("include"));
    mod.addIncludePath(b.path("third_party"));
    if (steam) mod.addIncludePath(b.path(steam_sdk ++ "/public"));

    const lib = b.addLibrary(.{ .name = "eng", .linkage = .static, .root_module = mod });
    lib.installHeadersDirectory(b.path("include/eng"), "eng", .{});
    lib.installHeadersDirectory(imgui.path(""), "", .{ .include_extensions = &.{".h"} });
    lib.installHeadersDirectory(glfw.path("include/GLFW"), "GLFW", .{});
    if (steam) lib.installHeadersDirectory(b.path(steam_sdk ++ "/public/steam"), "steam", .{});
    b.installArtifact(lib);
}

fn glfwFlags(is_windows: bool) []const []const u8 {
    return if (is_windows)
        &.{ "-std=c99", "-D_GLFW_WIN32", "-DUNICODE", "-D_UNICODE", "-w" }
    else
        &.{ "-std=c99", "-D_GLFW_X11", "-D_GLFW_WAYLAND", "-D_DEFAULT_SOURCE", "-DHAVE_MEMFD_CREATE", "-w" };
}

/// `pkg-config --cflags-only-I` at configure time, as system include paths
/// (so warnings in them stay quiet). Works for cross targets too, unlike
/// zig's native path detection.
pub fn addPkgConfigIncludes(b: *std.Build, m: *std.Build.Module, pkg: []const u8) void {
    var it = std.mem.tokenizeAny(u8, b.run(&.{ "pkg-config", "--cflags-only-I", pkg }), " \t\r\n");
    while (it.next()) |flag| {
        if (std.mem.startsWith(u8, flag, "-I")) m.addSystemIncludePath(.{ .cwd_relative = flag[2..] });
    }
}

/// Links eng into a game module: libeng.a, the Vulkan loader, libsteam_api
/// when `steam` is set (pass the same value as the dependency's `steam`
/// option), and eng's public macros.
///
/// Use this rather than a bare `linkLibrary`. Include paths and system
/// libraries propagate from the artifact, but C macros, library search paths
/// and prebuilt objects don't — and eng's headers change class layouts on
/// ENG_HAVE_STEAM (SteamPlatform/SteamTransport). A game compiled without it
/// against a Steam-enabled libeng gets a smaller SteamPlatform than the
/// engine writes to: silent stack corruption of whatever member follows it.
pub fn link(b: *std.Build, exe: *std.Build.Step.Compile, eng_dep: *std.Build.Dependency, steam: bool) void {
    const m = exe.root_module;
    const is_windows = m.resolved_target.?.result.os.tag == .windows;
    // libvulkan.so here is the build machine's, linked against a newer glibc
    // than an old-glibc target (x86_64-linux-gnu.2.31) provides. Only its
    // DT_NEEDED matters; players' own loader resolves against their glibc.
    if (!is_windows) exe.linker_allow_shlib_undefined = true;
    m.linkLibrary(eng_dep.artifact("eng"));
    addPublicMacros(m, is_windows, steam);
    for (pc_headers_common) |pkg| addPkgConfigIncludes(b, m, pkg);
    if (is_windows) {
        // vulkan-1.dll import library, generated from the loader's export list.
        const dlltool = b.addSystemCommand(&.{ b.graph.zig_exe, "dlltool", "-m", "i386:x86-64", "-d" });
        dlltool.addFileArg(eng_dep.path("third_party/vulkan/vulkan-1.def"));
        dlltool.addArg("-l");
        m.addObjectFile(dlltool.addOutputFileArg("libvulkan-1.a"));
    } else {
        // libvulkan.so from the loader's pkg-config dir; DT_NEEDED is its soname
        // (libvulkan.so.1), which every system with a Vulkan driver has.
        var it = std.mem.tokenizeAny(u8, b.run(&.{ "pkg-config", "--variable=libdir", "vulkan" }), " \t\r\n");
        m.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ it.next().?, "libvulkan.so" }) });
    }
    if (steam) m.addObjectFile(eng_dep.path(steamLib(is_windows, .link)));
}

/// Macros that must match between eng and everything compiled against its headers.
fn addPublicMacros(m: *std.Build.Module, is_windows: bool, steam: bool) void {
    if (steam) m.addCMacro("ENG_HAVE_STEAM", "1");
    m.addCMacro("GLFW_INCLUDE_NONE", "1"); // no OpenGL anywhere; skips <GL/gl.h>
    if (is_windows) {
        m.addCMacro("WIN32_LEAN_AND_MEAN", "");
        m.addCMacro("NOMINMAX", "");
        m.addCMacro("_WIN32_WINNT", "0x0601");
    }
}

const SteamLibKind = enum { link, runtime };

fn steamLib(is_windows: bool, kind: SteamLibKind) []const u8 {
    const dir = steam_sdk ++ "/redistributable_bin/";
    if (!is_windows) return dir ++ "linux64/libsteam_api.so";
    return switch (kind) {
        .link => dir ++ "win64/steam_api64.lib",
        .runtime => dir ++ "win64/steam_api64.dll",
    };
}

/// Per-executable runtime plumbing: rpath $ORIGIN on Linux (libsteam_api.so
/// sits next to the binary), and installs the Steam runtime library plus the
/// game's steam_appid.txt into bin/.
pub fn installSteamRuntime(
    b: *std.Build,
    eng_dep: *std.Build.Dependency,
    exe: *std.Build.Step.Compile,
    appid_file: std.Build.LazyPath,
) void {
    const is_windows = exe.rootModuleTarget().os.tag == .windows;
    if (!is_windows) exe.root_module.addRPathSpecial("$ORIGIN");
    const runtime = steamLib(is_windows, .runtime);
    b.getInstallStep().dependOn(&b.addInstallFileWithDir(eng_dep.path(runtime), .bin, std.fs.path.basename(runtime)).step);
    b.getInstallStep().dependOn(&b.addInstallFileWithDir(appid_file, .bin, "steam_appid.txt").step);
}
