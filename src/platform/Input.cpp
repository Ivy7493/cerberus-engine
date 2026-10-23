#include "eng/platform/Input.h"

#include <GLFW/glfw3.h>

#include <cmath>

namespace eng {

void Input::update(GLFWwindow* window) {
    m_prevKeys = m_keys;
    m_prevMouse = m_mouse;
    bool anyKey = false;
    // GLFW_KEY_SPACE (32) is the lowest valid key code; glfwGetKey on
    // codes below that raises a GLFW_INVALID_ENUM error.
    for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST && k < kMaxKeys; ++k) {
        m_keys[k] = glfwGetKey(window, k) == GLFW_PRESS;
        anyKey |= m_keys[k] && !m_prevKeys[k];
    }
    for (int b = 0; b < kMaxMouseButtons; ++b) {
        m_mouse[b] = glfwGetMouseButton(window, b) == GLFW_PRESS;
        anyKey |= m_mouse[b] && !m_prevMouse[b];
    }

    // Window-space cursor → framebuffer pixels, so HiDPI matches the
    // coordinates Renderer2D works in.
    double cx = 0, cy = 0;
    glfwGetCursorPos(window, &cx, &cy);
    int ww = 1, wh = 1, fw = 1, fh = 1;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    m_mouseX = cx * (ww > 0 ? double(fw) / ww : 1.0);
    m_mouseY = cy * (wh > 0 ? double(fh) / wh : 1.0);

    bool anyPad = false;
    for (int i = 0; i < kMaxGamepads; ++i) {
        Gamepad& pad = m_pads[i];
        pad.prevButtons = pad.buttons;
        GLFWgamepadstate state;
        pad.connected = glfwJoystickIsGamepad(GLFW_JOYSTICK_1 + i) &&
                        glfwGetGamepadState(GLFW_JOYSTICK_1 + i, &state);
        if (!pad.connected) {
            pad.buttons.fill(false);
            pad.axes.fill(0.0f);
            continue;
        }
        for (int b = 0; b < kMaxGamepadButtons; ++b) {
            pad.buttons[b] = state.buttons[b] == GLFW_PRESS;
            anyPad |= pad.buttons[b] && !pad.prevButtons[b];
        }
        for (int a = 0; a < kMaxGamepadAxes; ++a) {
            float v = state.axes[a];
            // Triggers rest at -1; leave them raw, deadzone only the sticks.
            if (a < GLFW_GAMEPAD_AXIS_LEFT_TRIGGER && std::fabs(v) < kDeadzone) v = 0.0f;
            pad.axes[a] = v;
            if (a < GLFW_GAMEPAD_AXIS_LEFT_TRIGGER && v != 0.0f) anyPad = true;
        }
    }
    if (anyPad) m_lastWasPad = true;
    else if (anyKey) m_lastWasPad = false;
}

bool Input::keyDown(int key) const { return key >= 0 && key < kMaxKeys && m_keys[key]; }
bool Input::keyPressed(int key) const { return keyDown(key) && !m_prevKeys[key]; }
bool Input::keyReleased(int key) const { return key >= 0 && key < kMaxKeys && !m_keys[key] && m_prevKeys[key]; }

bool Input::mouseDown(int button) const { return button >= 0 && button < kMaxMouseButtons && m_mouse[button]; }
bool Input::mousePressed(int button) const { return mouseDown(button) && !m_prevMouse[button]; }

bool Input::padDown(int index, int button) const {
    return index >= 0 && index < kMaxGamepads && button >= 0 && button < kMaxGamepadButtons &&
           m_pads[index].buttons[button];
}

bool Input::padPressed(int index, int button) const {
    return padDown(index, button) && !m_pads[index].prevButtons[button];
}

float Input::padAxis(int index, int axis) const {
    if (index < 0 || index >= kMaxGamepads || axis < 0 || axis >= kMaxGamepadAxes) return 0.0f;
    return m_pads[index].axes[axis];
}

int Input::firstConnectedGamepad() const {
    for (int i = 0; i < kMaxGamepads; ++i)
        if (m_pads[i].connected) return i;
    return -1;
}

}  // namespace eng
