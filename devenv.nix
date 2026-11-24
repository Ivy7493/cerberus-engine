{ pkgs, lib, ... }:

{
  packages = [
    pkgs.git
    pkgs.zig
    pkgs.shaderc # glslc: shaders are compiled to embedded SPIR-V at build time
    pkgs.pkg-config
    pkgs.vulkan-headers
    pkgs.vulkan-loader
    pkgs.vulkan-validation-layers
    pkgs.glm
    # GLFW is built from source (build.zig); these are only its headers.
    # X11 and Wayland are dlopen()ed at runtime, so nothing links against them.
    pkgs.xorg.libX11
    pkgs.xorg.libXcursor
    pkgs.xorg.libXrandr
    pkgs.xorg.libXinerama
    pkgs.xorg.libXi
    pkgs.xorg.libXext
    pkgs.xorg.libXrender
    pkgs.xorg.libXfixes
    pkgs.xorg.xorgproto
    pkgs.libxkbcommon
    pkgs.wayland
    pkgs.wayland-scanner
  ];

  env.VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";

  enterShell = ''
    # GLFW dlopens libvulkan / X11 / Wayland at runtime.
    export LD_LIBRARY_PATH="${lib.makeLibraryPath [
      pkgs.vulkan-loader
      pkgs.libxkbcommon
      pkgs.wayland
      pkgs.xorg.libX11
      pkgs.xorg.libXcursor
      pkgs.xorg.libXi
      pkgs.xorg.libXrandr
    ]}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  '';

  scripts = {
    build.exec = ''zig build "$@"'';
    # Rebuild on every save; only changed translation units recompile.
    watch.exec = ''zig build --watch -fincremental "$@"'';
  };

  enterTest = ''
    zig build
  '';
}
