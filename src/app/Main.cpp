// Voxl entry point.
//
// At this milestone Main only proves that the platform layer, the GL loader and
// the UI backend all come up and shut down cleanly. The application object that
// owns the real frame loop lands in the next milestone.

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/Log.hpp"
#include "platform/Window.hpp"

#include <exception>

namespace {

/// RAII wrapper so ImGui is always shut down in reverse order, including on the
/// exception paths out of the frame loop.
class ImGuiContextScope {
public:
    explicit ImGuiContextScope(voxl::Window& window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window.handle(), true);
        ImGui_ImplOpenGL3_Init("#version 450 core");
    }

    ~ImGuiContextScope()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    ImGuiContextScope(const ImGuiContextScope&)            = delete;
    ImGuiContextScope& operator=(const ImGuiContextScope&) = delete;
};

int run()
{
    voxl::setLogFile("voxl.log");
    VOXL_LOG_INFO("Voxl starting up");

    voxl::WindowConfig config;
    config.title = "Voxl";
    voxl::Window window{config};

    ImGuiContextScope imguiScope{window};

    // Sanity-check that GLM is linked and behaving: an identity-projected origin
    // must land at the origin. Cheap, and it catches a mis-linked GLM instantly.
    const glm::mat4 projection = glm::perspective(glm::radians(70.0f), window.aspectRatio(), 0.1f, 512.0f);
    VOXL_LOG_DEBUG("Projection[0][0] = {:.4f}", projection[0][0]);

    while (!window.shouldClose()) {
        window.pollEvents();

        if (window.isIconified()) {
            continue;  // No framebuffer to draw into; do not burn a core spinning.
        }

        glViewport(0, 0, window.width(), window.height());
        glClearColor(0.53f, 0.71f, 0.92f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Voxl");
        ImGui::TextUnformatted(window.glVersionString().c_str());
        ImGui::TextUnformatted(window.glRendererString().c_str());
        ImGui::Text("%.1f FPS", static_cast<double>(ImGui::GetIO().Framerate));
        ImGui::End();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        window.swapBuffers();
    }

    VOXL_LOG_INFO("Voxl shutting down");
    return 0;
}

}  // namespace

int main()
{
    int exitCode = 1;
    try {
        exitCode = run();
    } catch (const std::exception& error) {
        VOXL_LOG_FATAL("Unhandled exception: {}", error.what());
    } catch (...) {
        VOXL_LOG_FATAL("Unhandled non-standard exception");
    }
    voxl::shutdownLogging();
    return exitCode;
}
