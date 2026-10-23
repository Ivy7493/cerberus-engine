#pragma once

#include <string>

struct GLFWwindow;

namespace eng {

// Owns the GLFW window (and GLFW itself — one Window per process). Created
// with GLFW_NO_API: Vulkan is set up separately by Renderer2D/VulkanContext.
class Window {
public:
    Window(const std::string& title, int width, int height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    GLFWwindow* handle() const { return m_window; }

    bool shouldClose() const;
    void requestClose();
    void pollEvents();

    // Framebuffer size in pixels (differs from window size on HiDPI).
    int framebufferWidth() const;
    int framebufferHeight() const;

    // Set by GLFW's resize callback, cleared by whoever recreates the
    // swapchain (Renderer2D::endFrame).
    bool consumeResized();

    void toggleFullscreen();
    bool isFullscreen() const { return m_fullscreen; }

    void setTitle(const std::string& title);

private:
    GLFWwindow* m_window = nullptr;
    bool m_resized = false;
    bool m_fullscreen = false;
    // Windowed-mode geometry saved on entering fullscreen, restored on exit.
    int m_savedX = 0, m_savedY = 0, m_savedW = 0, m_savedH = 0;

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
};

}  // namespace eng
