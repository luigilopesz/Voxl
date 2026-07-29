#pragma once

// GLFW window + OpenGL 4.5 core context ownership.
//
// This is the only place in the engine that is allowed to include GLFW. Every
// other subsystem consumes input and window events through the structures
// declared here, which keeps the windowing library swappable and keeps GLFW's
// global-callback model from leaking into gameplay code.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct GLFWwindow;

namespace voxl {

struct WindowConfig {
    int         width      = 1600;
    int         height     = 900;
    std::string title      = "Voxl";
    bool        vsync      = true;
    bool        fullscreen = false;
    /// Requests a debug context and installs the GL debug-message callback.
    /// Costs a little performance, so it follows the build configuration.
    bool        debugContext = VOXL_DEBUG != 0;
    /// 0 disables MSAA. The default framebuffer is only multisampled when the
    /// renderer is not doing its own resolve.
    int         msaaSamples = 0;
};

/// Owns the OS window and the GL context. Non-copyable, non-movable: the GLFW
/// user pointer stores `this`, so the address must be stable.
class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&)                 = delete;
    Window& operator=(Window&&)      = delete;

    /// Pumps the OS event queue. Call once per frame before simulation.
    void pollEvents();
    void swapBuffers();

    [[nodiscard]] bool shouldClose() const noexcept;
    void requestClose() noexcept;

    [[nodiscard]] int width() const noexcept { return m_framebufferWidth; }
    [[nodiscard]] int height() const noexcept { return m_framebufferHeight; }
    [[nodiscard]] float aspectRatio() const noexcept;

    /// True while the window is minimised or has a zero-area framebuffer, in
    /// which case rendering must be skipped rather than dividing by zero.
    [[nodiscard]] bool isIconified() const noexcept { return m_iconified; }

    void setVSync(bool enabled);
    [[nodiscard]] bool vsync() const noexcept { return m_vsync; }

    /// Captures the cursor for mouse-look. Idempotent.
    void setCursorCaptured(bool captured);
    [[nodiscard]] bool cursorCaptured() const noexcept { return m_cursorCaptured; }

    [[nodiscard]] GLFWwindow* handle() const noexcept { return m_window; }

    /// Raw GL version actually granted by the driver, for the debug overlay.
    [[nodiscard]] const std::string& glVersionString() const noexcept { return m_glVersion; }
    [[nodiscard]] const std::string& glRendererString() const noexcept { return m_glRenderer; }

private:
    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void iconifyCallback(GLFWwindow* window, int iconified);

    GLFWwindow* m_window = nullptr;
    int  m_framebufferWidth  = 0;
    int  m_framebufferHeight = 0;
    bool m_iconified      = false;
    bool m_vsync          = true;
    bool m_cursorCaptured = false;
    std::string m_glVersion;
    std::string m_glRenderer;
};

}  // namespace voxl
