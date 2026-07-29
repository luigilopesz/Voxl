#include "platform/Window.hpp"

#include <glad/gl.h>   // must precede GLFW so GLFW does not pull in its own GL header
#include <GLFW/glfw3.h>

#include "core/Log.hpp"

#include <atomic>
#include <stdexcept>

namespace voxl {
namespace {

// GLFW is a process-wide singleton; reference-count init so tests can create
// and destroy windows repeatedly without terminating a library still in use.
std::atomic<int> g_glfwRefCount{0};

void glfwErrorCallback(int code, const char* description)
{
    VOXL_LOG_ERROR("GLFW error {}: {}", code, description ? description : "<null>");
}

void APIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
                              GLsizei /*length*/, const GLchar* message, const void* /*user*/)
{
    // Driver chatter that carries no signal for us.
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204) {
        return;
    }

    const char* sourceText = "other";
    switch (source) {
        case GL_DEBUG_SOURCE_API:             sourceText = "api"; break;
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM:   sourceText = "window"; break;
        case GL_DEBUG_SOURCE_SHADER_COMPILER: sourceText = "shader"; break;
        case GL_DEBUG_SOURCE_THIRD_PARTY:     sourceText = "thirdparty"; break;
        case GL_DEBUG_SOURCE_APPLICATION:     sourceText = "app"; break;
        default: break;
    }

    const char* typeText = "other";
    switch (type) {
        case GL_DEBUG_TYPE_ERROR:               typeText = "error"; break;
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: typeText = "deprecated"; break;
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  typeText = "undefined"; break;
        case GL_DEBUG_TYPE_PORTABILITY:         typeText = "portability"; break;
        case GL_DEBUG_TYPE_PERFORMANCE:         typeText = "performance"; break;
        case GL_DEBUG_TYPE_MARKER:              typeText = "marker"; break;
        default: break;
    }

    switch (severity) {
        case GL_DEBUG_SEVERITY_HIGH:
            VOXL_LOG_ERROR("GL[{}/{}] {}: {}", sourceText, typeText, id, message);
            break;
        case GL_DEBUG_SEVERITY_MEDIUM:
            VOXL_LOG_WARN("GL[{}/{}] {}: {}", sourceText, typeText, id, message);
            break;
        case GL_DEBUG_SEVERITY_LOW:
            VOXL_LOG_DEBUG("GL[{}/{}] {}: {}", sourceText, typeText, id, message);
            break;
        default:
            VOXL_LOG_TRACE("GL[{}/{}] {}: {}", sourceText, typeText, id, message);
            break;
    }
}

const char* orEmpty(const GLubyte* value)
{
    return value ? reinterpret_cast<const char*>(value) : "<unknown>";
}

}  // namespace

Window::Window(const WindowConfig& config)
{
    if (g_glfwRefCount.fetch_add(1) == 0) {
        glfwSetErrorCallback(&glfwErrorCallback);
        if (glfwInit() != GLFW_TRUE) {
            g_glfwRefCount.fetch_sub(1);
            throw std::runtime_error{"Failed to initialise GLFW"};
        }
    }

    // 4.5 core is the floor: direct state access and immutable texture storage
    // are load-bearing in the renderer, and every GPU we target exposes them.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, config.debugContext ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SAMPLES, config.msaaSamples);
    glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // shown once the first frame is ready

    GLFWmonitor* monitor = config.fullscreen ? glfwGetPrimaryMonitor() : nullptr;
    int windowWidth  = config.width;
    int windowHeight = config.height;
    if (monitor != nullptr) {
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitor)) {
            windowWidth  = mode->width;
            windowHeight = mode->height;
        }
    }

    m_window = glfwCreateWindow(windowWidth, windowHeight, config.title.c_str(), monitor, nullptr);
    if (m_window == nullptr) {
        if (g_glfwRefCount.fetch_sub(1) == 1) {
            glfwTerminate();
        }
        throw std::runtime_error{"Failed to create a GLFW window with an OpenGL 4.5 core context"};
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwMakeContextCurrent(m_window);

    const int gladVersion = gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress));
    if (gladVersion == 0) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        if (g_glfwRefCount.fetch_sub(1) == 1) {
            glfwTerminate();
        }
        throw std::runtime_error{"Failed to load OpenGL function pointers"};
    }

    m_glVersion  = orEmpty(glGetString(GL_VERSION));
    m_glRenderer = orEmpty(glGetString(GL_RENDERER));
    VOXL_LOG_INFO("OpenGL {} on {}", m_glVersion, m_glRenderer);
    VOXL_LOG_INFO("glad loaded GL {}.{}", GLAD_VERSION_MAJOR(gladVersion),
                  GLAD_VERSION_MINOR(gladVersion));

    if (config.debugContext) {
        GLint contextFlags = 0;
        glGetIntegerv(GL_CONTEXT_FLAGS, &contextFlags);
        if ((contextFlags & GL_CONTEXT_FLAG_DEBUG_BIT) != 0) {
            glEnable(GL_DEBUG_OUTPUT);
            // Synchronous output costs throughput but makes the callstack at the
            // point of the error meaningful, which is the entire point.
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(&glDebugCallback, nullptr);
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
            VOXL_LOG_INFO("OpenGL debug output enabled");
        } else {
            VOXL_LOG_WARN("A debug context was requested but the driver did not provide one");
        }
    }

    glfwGetFramebufferSize(m_window, &m_framebufferWidth, &m_framebufferHeight);
    glfwSetFramebufferSizeCallback(m_window, &framebufferSizeCallback);
    glfwSetWindowIconifyCallback(m_window, &iconifyCallback);

    setVSync(config.vsync);
    glfwShowWindow(m_window);
}

Window::~Window()
{
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    if (g_glfwRefCount.fetch_sub(1) == 1) {
        glfwTerminate();
    }
}

void Window::pollEvents()
{
    glfwPollEvents();
}

void Window::swapBuffers()
{
    glfwSwapBuffers(m_window);
}

bool Window::shouldClose() const noexcept
{
    return glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

void Window::requestClose() noexcept
{
    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

float Window::aspectRatio() const noexcept
{
    if (m_framebufferHeight <= 0) {
        return 1.0f;
    }
    return static_cast<float>(m_framebufferWidth) / static_cast<float>(m_framebufferHeight);
}

void Window::setVSync(bool enabled)
{
    m_vsync = enabled;
    glfwSwapInterval(enabled ? 1 : 0);
}

void Window::setCursorCaptured(bool captured)
{
    if (m_cursorCaptured == captured) {
        return;
    }
    m_cursorCaptured = captured;
    glfwSetInputMode(m_window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

    // Raw motion removes the OS pointer-acceleration curve, which otherwise
    // makes mouse-look feel inconsistent between machines.
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    }
}

void Window::framebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self == nullptr) {
        return;
    }
    self->m_framebufferWidth  = width;
    self->m_framebufferHeight = height;
}

void Window::iconifyCallback(GLFWwindow* window, int iconified)
{
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self == nullptr) {
        return;
    }
    self->m_iconified = iconified == GLFW_TRUE;
}

}  // namespace voxl
