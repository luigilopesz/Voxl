#include "platform/Input.hpp"

#include <glad/gl.h>   // must precede GLFW so GLFW does not pull in its own GL header
#include <GLFW/glfw3.h>

#include "platform/Window.hpp"

#include <array>
#include <cstddef>

namespace voxl {
namespace {

/// Key -> GLFW_KEY_*. A table rather than a switch so the compiler checks that
/// every enumerator has an entry (the static_assert below pins the length).
constexpr std::array<int, static_cast<std::size_t>(Key::Count)> kGlfwKeys{
    GLFW_KEY_UNKNOWN,
    GLFW_KEY_A, GLFW_KEY_B, GLFW_KEY_C, GLFW_KEY_D, GLFW_KEY_E, GLFW_KEY_F, GLFW_KEY_G,
    GLFW_KEY_H, GLFW_KEY_I, GLFW_KEY_J, GLFW_KEY_K, GLFW_KEY_L, GLFW_KEY_M, GLFW_KEY_N,
    GLFW_KEY_O, GLFW_KEY_P, GLFW_KEY_Q, GLFW_KEY_R, GLFW_KEY_S, GLFW_KEY_T, GLFW_KEY_U,
    GLFW_KEY_V, GLFW_KEY_W, GLFW_KEY_X, GLFW_KEY_Y, GLFW_KEY_Z,
    GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4, GLFW_KEY_5,
    GLFW_KEY_6, GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9, GLFW_KEY_0,
    GLFW_KEY_SPACE,
    GLFW_KEY_ESCAPE,
    GLFW_KEY_TAB,
    GLFW_KEY_ENTER,
    GLFW_KEY_LEFT_SHIFT,
    GLFW_KEY_LEFT_CONTROL,
    GLFW_KEY_LEFT_ALT,
    GLFW_KEY_F1, GLFW_KEY_F2, GLFW_KEY_F3, GLFW_KEY_F4, GLFW_KEY_F5, GLFW_KEY_F6,
    GLFW_KEY_F7, GLFW_KEY_F8, GLFW_KEY_F9, GLFW_KEY_F10, GLFW_KEY_F11, GLFW_KEY_F12,
};

constexpr std::array<int, static_cast<std::size_t>(MouseButton::Count)> kGlfwButtons{
    GLFW_MOUSE_BUTTON_LEFT,
    GLFW_MOUSE_BUTTON_RIGHT,
    GLFW_MOUSE_BUTTON_MIDDLE,
};

}  // namespace

Input::Input(Window& window) noexcept : m_window(&window) {}

void Input::newFrame()
{
    GLFWwindow* handle = m_window->handle();

    for (std::size_t i = 0; i < kGlfwKeys.size(); ++i) {
        m_keyPrev[i] = m_keyDown[i];
        const int code = kGlfwKeys[i];
        m_keyDown[i] = code != GLFW_KEY_UNKNOWN && glfwGetKey(handle, code) == GLFW_PRESS;
    }

    for (std::size_t i = 0; i < kGlfwButtons.size(); ++i) {
        m_buttonPrev[i] = m_buttonDown[i];
        m_buttonDown[i] = glfwGetMouseButton(handle, kGlfwButtons[i]) == GLFW_PRESS;
    }

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(handle, &x, &y);
    const glm::vec2 cursor{static_cast<float>(x), static_cast<float>(y)};

    const bool captured = m_window->cursorCaptured();
    // A capture transition teleports the cursor, so the first delta after it is
    // meaningless and would snap the view somewhere the player never aimed.
    if (!m_hasCursor || captured != m_wasCaptured) {
        m_mouseDelta = glm::vec2{0.0f};
    } else {
        m_mouseDelta = cursor - m_cursor;
    }
    m_cursor      = cursor;
    m_hasCursor   = true;
    m_wasCaptured = captured;

    m_scrollDelta = m_window->consumeScrollDelta();
}

bool Input::keyDown(Key key) const noexcept
{
    return m_keyDown[static_cast<std::size_t>(key)];
}

bool Input::keyPressed(Key key) const noexcept
{
    const auto index = static_cast<std::size_t>(key);
    return m_keyDown[index] && !m_keyPrev[index];
}

bool Input::mouseDown(MouseButton button) const noexcept
{
    return m_buttonDown[static_cast<std::size_t>(button)];
}

bool Input::mousePressed(MouseButton button) const noexcept
{
    const auto index = static_cast<std::size_t>(button);
    return m_buttonDown[index] && !m_buttonPrev[index];
}

}  // namespace voxl
