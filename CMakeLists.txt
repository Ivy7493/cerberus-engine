# eng — shared engine library (2D Vulkan renderer, input, audio, Steam/UDP
# networking). Games consume it with:
#
#   add_subdirectory(${ENGINE_DIR} ${CMAKE_BINARY_DIR}/engine)
#   target_link_libraries(mygame PRIVATE eng)
#   eng_setup_runtime(mygame)   # rpath/steam_appid/static-runtime plumbing
#
# Windows builds expect the same WINDOWS_* cache vars arena's build.sh
# passes; scripts/windows-deps.sh fetches/creates them.
cmake_minimum_required(VERSION 3.20)
project(eng CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENG_STEAM "Link Steamworks (lobbies + P2P transport)" ON)

set(ENG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(ENG_DIR ${ENG_DIR} PARENT_SCOPE)
set(ENG_STEAM_SDK ${ENG_DIR}/third_party/steamworks)

# ── Platform deps ────────────────────────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    foreach(dep WINDOWS_GLFW_INCLUDE WINDOWS_GLFW_LIB WINDOWS_VULKAN_INCLUDE WINDOWS_VULKAN_LIB)
        if(NOT ${dep})
            message(FATAL_ERROR "${dep} must be set. Use the game's build.sh for automatic setup.")
        endif()
    endforeach()
    if(NOT TARGET Vulkan::Vulkan)
        add_library(Vulkan::Vulkan IMPORTED UNKNOWN GLOBAL)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${WINDOWS_VULKAN_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${WINDOWS_VULKAN_INCLUDE}")
    endif()
    add_library(eng_glfw IMPORTED STATIC GLOBAL)
    set_target_properties(eng_glfw PROPERTIES
        IMPORTED_LOCATION "${WINDOWS_GLFW_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${WINDOWS_GLFW_INCLUDE}")
    set(ENG_GLFW_LIBS eng_glfw ws2_32 gdi32 winmm shell32)
    set(ENG_GLFW_INCLUDES "${WINDOWS_GLFW_INCLUDE}")
else()
    find_package(Vulkan REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW REQUIRED glfw3)
    set(ENG_GLFW_LIBS ${GLFW_LIBRARIES})
    set(ENG_GLFW_INCLUDES ${GLFW_INCLUDE_DIRS})
endif()

# ── Dear ImGui (built into eng; games include <imgui.h> through it) ──────────
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ── Shaders → SPIR-V embedded as C arrays (no runtime shader files) ──────────
find_program(GLSLC glslc REQUIRED)
set(ENG_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${ENG_GEN_DIR})
set(ENG_SHADER_INCS)
foreach(shader sprite.vert sprite.frag)
    set(out ${ENG_GEN_DIR}/${shader}.inc)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${GLSLC} -mfmt=c ${ENG_DIR}/shaders/${shader} -o ${out}
        DEPENDS ${ENG_DIR}/shaders/${shader}
        COMMENT "Compiling ${shader}")
    list(APPEND ENG_SHADER_INCS ${out})
endforeach()

add_library(eng STATIC
    src/core/CrashLog.cpp
    src/platform/Window.cpp
    src/platform/Input.cpp
    src/render/VulkanContext.cpp
    src/render/Swapchain.cpp
    src/render/VulkanUtils.cpp
    src/render/Renderer2D.cpp
    src/render/StbImage.cpp
    src/audio/Audio.cpp
    src/net/UdpSocket.cpp
    src/net/UdpTransport.cpp
    src/net/SteamPlatform.cpp
    src/net/SteamTransport.cpp
    src/net/LagTransport.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${ENG_SHADER_INCS}
)

target_include_directories(eng
    PUBLIC  include ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${ENG_GLFW_INCLUDES}
    PRIVATE third_party ${ENG_GEN_DIR})
target_link_libraries(eng PUBLIC Vulkan::Vulkan ${ENG_GLFW_LIBS})
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(eng PUBLIC dl pthread m)
endif()

if(ENG_STEAM)
    target_include_directories(eng PUBLIC ${ENG_STEAM_SDK}/public)
    target_compile_definitions(eng PUBLIC ENG_HAVE_STEAM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_libraries(eng PUBLIC ${ENG_STEAM_SDK}/redistributable_bin/win64/steam_api64.lib)
    else()
        target_link_libraries(eng PUBLIC ${ENG_STEAM_SDK}/redistributable_bin/linux64/libsteam_api.so)
    endif()
endif()

# miniaudio's implementation TU trips plenty of warnings; keep eng quiet.
set_source_files_properties(src/audio/Audio.cpp PROPERTIES COMPILE_OPTIONS "-w")

# Per-executable runtime plumbing. `appid_file` is the game's own
# steam_appid.txt (SteamAPI_Init looks for it next to the binary).
function(eng_setup_runtime target appid_file)
    # Runs in the caller's scope, where ENG_STEAM_SDK isn't defined.
    set(ENG_STEAM_SDK ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/third_party/steamworks)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
    else()
        # libsteam_api.so / bundled libglfw sit next to the binary.
        target_link_options(${target} PRIVATE "-Wl,-rpath,$ORIGIN")
    endif()
    set(bin_dir $<TARGET_FILE_DIR:${target}>)
    if(ENG_STEAM)
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(steam_lib ${ENG_STEAM_SDK}/redistributable_bin/win64/steam_api64.dll)
        else()
            set(steam_lib ${ENG_STEAM_SDK}/redistributable_bin/linux64/libsteam_api.so)
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${steam_lib} ${bin_dir}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${appid_file} ${bin_dir}/steam_appid.txt)
    endif()
endfunction()
