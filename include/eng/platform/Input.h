#pragma once

#include <array>
#include <cstdint>

struct GLFWwindow;

namespace eng {

// Generic per-frame input snapshot: keyboard, mouse and up to 4 gamepads.
// Games map these to their own actions — nothing here knows about "fire"
// or "move". Call update() once per frame after Window::pollEvents().
class Input {
public:
    static constexpr int kMaxKeys = 512;          // >= GLFW_KEY_LAST
    static constexpr int kMaxMouseButtons = 8;
    static constexpr int kMaxGamepads = 4;
    static constexpr int kMaxGamepadButtons = 15;  // GLFW_GAMEPAD_BUTTON_LAST + 1
    static constexpr int kMaxGamepadAxes = 6;

    struct Gamepad {
        bool connected = false;
        std::array<bool, kMaxGamepadButtons> buttons{};
        std::array<bool, kMaxGamepadButtons> prevButtons{};
        std::array<float, kMaxGamepadAxes> axes{};
    };

    void update(GLFWwindow* window);

    // Keys use GLFW_KEY_* codes; mouse buttons GLFW_MOUSE_BUTTON_*.
    bool keyDown(int key) const;
    bool keyPressed(int key) const;   // went down this frame
    bool keyReleased(int key) const;  // went up this frame

    bool mouseDown(int button) const;
    bool mousePressed(int button) const;
    double mouseX() const { return m_mouseX; }  // framebuffer pixels
    double mouseY() const { return m_mouseY; }

    // Gamepad buttons use GLFW_GAMEPAD_BUTTON_*, axes GLFW_GAMEPAD_AXIS_*.
    // Axes have a radial-ish deadzone applied (per-axis, `kDeadzone`).
    const Gamepad& gamepad(int index) const { return m_pads[index]; }
    bool padDown(int index, int button) const;
    bool padPressed(int index, int button) const;
    float padAxis(int index, int axis) const;
    int firstConnectedGamepad() const;  // -1 if none

    // True once any key/mouse/pad button was used this frame — lets a game
    // swap on-screen prompts between keyboard and controller glyphs.
    bool lastInputWasGamepad() const { return m_lastWasPad; }

    static constexpr float kDeadzone = 0.2f;

private:
    std::array<bool, kMaxKeys> m_keys{};
    std::array<bool, kMaxKeys> m_prevKeys{};
    std::array<bool, kMaxMouseButtons> m_mouse{};
    std::array<bool, kMaxMouseButtons> m_prevMouse{};
    std::array<Gamepad, kMaxGamepads> m_pads{};
    double m_mouseX = 0.0, m_mouseY = 0.0;
    bool m_lastWasPad = false;
};

}  // namespace eng
