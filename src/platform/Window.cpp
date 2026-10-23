#include "eng/platform/Window.h"

#include <GLFW/glfw3.h>

#include <stdexcept>

namespace eng {

Window::Window(const std::string& title, int width, int height) {
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }
    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
}

Window::~Window() {
    if (m_window) glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(m_window); }
void Window::requestClose() { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }
void Window::pollEvents() { glfwPollEvents(); }

int Window::framebufferWidth() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return w;
}

int Window::framebufferHeight() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return h;
}

bool Window::consumeResized() {
    bool r = m_resized;
    m_resized = false;
    return r;
}

void Window::toggleFullscreen() {
    if (!m_fullscreen) {
        glfwGetWindowPos(m_window, &m_savedX, &m_savedY);
        glfwGetWindowSize(m_window, &m_savedW, &m_savedH);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(m_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(m_window, nullptr, m_savedX, m_savedY, m_savedW, m_savedH, 0);
    }
    m_fullscreen = !m_fullscreen;
    m_resized = true;
}

void Window::setTitle(const std::string& title) { glfwSetWindowTitle(m_window, title.c_str()); }

void Window::framebufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) self->m_resized = true;
}

}  // namespace eng
