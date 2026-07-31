#include "voxel_app.hpp"
#include <filesystem>

#include <application/cli.hpp>
#include <utilities/debug.hpp>

void search_for_path_to_fix_working_directory(std::span<std::filesystem::path const> test_paths) {
    auto current_path = std::filesystem::current_path();
    while (true) {
        for (auto const &test_path : test_paths) {
            if (std::filesystem::exists(current_path / test_path)) {
                std::filesystem::current_path(current_path);
                return;
            }
        }
        if (!current_path.has_parent_path()) {
            break;
        }
        current_path = current_path.parent_path();
    }
}

auto main(int argc, char **argv) -> int {
    // Parsed before anything else so that a bad argument is reported before ~30 s of shader
    // compilation, and so the window can be created at the requested size on the first try
    // rather than being resized after the swapchain and every render target already exist.
    AppCli::parse(argc, argv);
    if (AppCli::get().help_requested) {
        return 0;
    }
    if (AppCli::get().parse_failed) {
        return 2;
    }

    search_for_path_to_fix_working_directory(std::array{
        std::filesystem::path{".out"},
        std::filesystem::path{"assets"},
    });

    auto global_console = debug_utils::Console{};
    auto global_debug_display = debug_utils::DebugDisplay{};

    auto settings = AppSettings{};

    FreeImage_Initialise();

    auto app = VoxelApp{};
    app.run();

    FreeImage_DeInitialise();
}
