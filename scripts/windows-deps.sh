#!/usr/bin/env bash
# Shared by every game's build.sh: fetches/creates the MinGW cross-compile
# deps (GLFW prebuilt, Vulkan import lib, header-only GLM/Vulkan headers)
# into engine/deps/windows, once. Source it, then call eng_windows_deps;
# it exports WINDEV pointing at the result.
eng_windows_deps() {
    local engine_dir
    engine_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    export WINDEV="$engine_dir/deps/windows"
    local glfw_version="3.4"

    if ! command -v x86_64-w64-mingw32-g++ &>/dev/null; then
        echo "ERROR: mingw-w64-gcc not found. Install it first: sudo pacman -S mingw-w64-gcc" >&2
        return 1
    fi
    mkdir -p "$WINDEV"

    if [[ ! -f "$WINDEV/glfw/lib-mingw-w64/libglfw3.a" ]]; then
        echo "==> Downloading GLFW $glfw_version Windows prebuilt..."
        local zip="$WINDEV/glfw-win64.zip"
        curl -fsSL "https://github.com/glfw/glfw/releases/download/$glfw_version/glfw-$glfw_version.bin.WIN64.zip" -o "$zip"
        unzip -q "$zip" -d "$WINDEV"
        mv "$WINDEV/glfw-$glfw_version.bin.WIN64" "$WINDEV/glfw"
        rm "$zip"
    fi

    if [[ ! -f "$WINDEV/vulkan/libvulkan-1.a" ]]; then
        echo "==> Creating Vulkan Windows import library..."
        mkdir -p "$WINDEV/vulkan"
        curl -fsSL "https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/main/loader/vulkan-1.def" \
            -o "$WINDEV/vulkan/vulkan-1.def"
        x86_64-w64-mingw32-dlltool --dllname vulkan-1.dll --def "$WINDEV/vulkan/vulkan-1.def" \
            --output-lib "$WINDEV/vulkan/libvulkan-1.a"
    fi

    # Clean include dir: /usr/include mixes Linux system headers with
    # MinGW's, causing uintptr_t/socket conflicts.
    local hdr="$WINDEV/include"
    mkdir -p "$hdr"
    [[ -d "$hdr/glm" ]] || cp -r /usr/include/glm "$hdr/"
    if [[ ! -d "$hdr/vulkan" ]]; then
        cp -r /usr/include/vulkan "$hdr/"
        cp -r /usr/include/vk_video "$hdr/"
    fi
}
