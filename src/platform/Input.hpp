#pragma once

// Polled keyboard/mouse state for the frame loop.
//
// Lives in platform/ because it is the only other place allowed to talk to GLFW
// (see the include rule in Window.hpp). Gameplay code names keys through the
// `Key` enum below, so nothing outside this directory ever sees a GLFW_KEY_*
// constant and the windowing library stays swappable.
//
// The class is a *sampler*, not an event queue: `newFrame()` snapshots the
// current device state and diffs it against the previous snapshot. That makes
// "pressed this frame" edges well defined without a callback chain fighting
// ImGui's, at the cost of losing a press that begins and ends inside one frame.
// At 60 Hz that is a 16 ms double-tap, which no human produces on a keyboard.
//
// Scroll is the exception - a wheel notch is an event, not a state - so it is
// accumulated by Window's GLFW callback and read back here.
//
// Thread safety: none. Main thread only.

#include <cstddef>

#include <glm/vec2.hpp>

namespace voxl {

class Window;

/// Keys the game binds. Deliberately not a full keyboard map: an unused
/// enumerator is a mapping in Input.cpp that nothing tests.
enum class Key : int {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Space,
    Escape,
    Tab,
    Enter,
    LeftShift,
    LeftControl,
    LeftAlt,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Count,
};

enum class MouseButton : int { Left = 0, Right, Middle, Count };

class Input {
public:
    /// `window` must outlive the Input. Nothing is captured beyond the handle.
    explicit Input(Window& window) noexcept;

    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;
    Input(Input&&)                 = delete;
    Input& operator=(Input&&)      = delete;

    /// Samples every device. Call once per frame, after Window::pollEvents().
    void newFrame();

    [[nodiscard]] bool keyDown(Key key) const noexcept;
    /// True only on the frame the key went down.
    [[nodiscard]] bool keyPressed(Key key) const noexcept;

    [[nodiscard]] bool mouseDown(MouseButton button) const noexcept;
    [[nodiscard]] bool mousePressed(MouseButton button) const noexcept;

    /// Cursor movement since the previous frame, in pixels, GLFW convention
    /// (+y downward). Zero on the frame the cursor is (re)captured, because the
    /// jump from the last free-cursor position to the capture point is not aim
    /// input and would spin the camera.
    [[nodiscard]] const glm::vec2& mouseDelta() const noexcept { return m_mouseDelta; }

    /// Wheel notches this frame; positive is away from the user.
    [[nodiscard]] float scrollDelta() const noexcept { return m_scrollDelta; }

private:
    Window* m_window = nullptr;

    bool m_keyDown[static_cast<std::size_t>(Key::Count)]{};
    bool m_keyPrev[static_cast<std::size_t>(Key::Count)]{};
    bool m_buttonDown[static_cast<std::size_t>(MouseButton::Count)]{};
    bool m_buttonPrev[static_cast<std::size_t>(MouseButton::Count)]{};

    glm::vec2 m_cursor{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    float     m_scrollDelta = 0.0f;
    bool      m_hasCursor   = false;

    /// Capture state observed on the previous frame; a change discards the
    /// cursor delta for one frame.
    bool m_wasCaptured = false;
};

}  // namespace voxl
