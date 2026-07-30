// Source-level invariants on the GLSL tree.
//
// WHY THESE ARE TEXT TESTS. The suite is headless by contract (see the note at
// the top of tests/CMakeLists.txt): nothing here may create a GL context, so a
// shader cannot be compiled, let alone executed, from this binary. What CAN be
// checked without a GPU is the structural property that the day/night work
// depends on - WHICH TERMS LIVE IN WHICH FUNCTION - and that is exactly the
// property that broke. So these read the shipped .glsl files off disk and assert
// on the code in them, with comments stripped first so that a comment describing
// the rule can never be mistaken for a violation of it.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// Injected by tests/CMakeLists.txt so the tests do not depend on the working
/// directory ctest happens to pick.
[[nodiscard]] fs::path shaderDirectory()
{
    return fs::path{VOXL_SHADER_DIR};
}

[[nodiscard]] std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/// GLSL has no string literals, so removing comments is exactly this simple and
/// there is no quoting case to get wrong.
///
/// Stripping is not cosmetic: the fix for the duplicated star field left behind a
/// comment that names `VOXL_TIME` and the word "hash" while explaining why
/// neither may appear in the code. Matching on raw source would flag that comment
/// and the test would then be asserting on prose.
[[nodiscard]] std::string stripComments(std::string_view source)
{
    std::string out;
    out.reserve(source.size());

    for (std::size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "//") == 0) {
            while (i < source.size() && source[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (source.compare(i, 2, "/*") == 0) {
            i += 2;
            while (i + 1 < source.size() && source.compare(i, 2, "*/") != 0) {
                ++i;
            }
            i = (i + 2 < source.size()) ? i + 2 : source.size();
            continue;
        }
        out.push_back(source[i]);
        ++i;
    }
    return out;
}

/// Body of the function whose signature starts with `signature`, brace matched.
/// Returns an empty string when the signature is absent, which the callers treat
/// as a failure rather than as a vacuous pass.
[[nodiscard]] std::string functionBody(std::string_view source, std::string_view signature)
{
    const std::size_t start = source.find(signature);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t open = source.find('{', start);
    if (open == std::string_view::npos) {
        return {};
    }

    int depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}') {
            --depth;
            if (depth == 0) {
                return std::string{source.substr(open + 1, i - open - 1)};
            }
        }
    }
    return {};
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

/// Every shader source in the tree, so a new file cannot quietly opt out of the
/// invariants below by not being listed.
[[nodiscard]] std::vector<fs::path> allShaderSources()
{
    std::vector<fs::path> paths;
    for (const fs::directory_entry& entry : fs::directory_iterator{shaderDirectory()}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string extension = entry.path().extension().string();
        if (extension == ".glsl" || extension == ".vert" || extension == ".frag") {
            paths.push_back(entry.path());
        }
    }
    REQUIRE(paths.size() >= 8u);
    return paths;
}

}  // namespace

// --------------------------------------------------------- the fog colour --

TEST_CASE("the shared sky colour holds no high-frequency term", "[shaders][daynight]")
{
    // THE INVARIANT. voxlSkyColour() is not just the sky: chunk.frag,
    // subvoxel.frag and water.frag all resolve their fog toward it, so whatever
    // it returns is also painted onto solid geometry and onto water once the fog
    // saturates. Every term in it must therefore be something fogged geometry
    // genuinely converges to - a smooth function of direction and of the frame
    // uniforms. A hash, a lattice or an animation has no meaning on a mountain
    // face.
    //
    // This is the regression guard for the duplicated star field: the legacy
    // block inside voxlSkyColour() drew stars a second time on top of sky.frag's
    // starField(), AND painted them onto every fogged surface in the world.
    const std::string source = stripComments(readFile(shaderDirectory() / "common.glsl"));
    const std::string body   = functionBody(source, "vec3 voxlSkyColour(");

    REQUIRE_FALSE(body.empty());

    // The hashes are the noise primitives; VOXL_TIME is the only thing that can
    // make the fog colour disagree with itself between two frames of a frozen
    // camera.
    CHECK_FALSE(contains(body, "voxlHash12"));
    CHECK_FALSE(contains(body, "voxlHash13"));
    CHECK_FALSE(contains(body, "voxlNoise"));
    CHECK_FALSE(contains(body, "VOXL_TIME"));
    CHECK_FALSE(contains(body, "uCameraPositionTime.w"));
}

TEST_CASE("the star field is drawn by exactly one shader", "[shaders][daynight]")
{
    // Stars are additive, so two implementations do not merely disagree - they
    // sum. sky.frag owns them because it is the only shader that draws the dome
    // and nothing else; see the header note in that file.
    std::vector<std::string> withStars;
    for (const fs::path& path : allShaderSources()) {
        const std::string source = stripComments(readFile(path));
        if (contains(source, "star") || contains(source, "Star")) {
            withStars.push_back(path.filename().string());
        }
    }

    CHECK(withStars.size() == 1u);
    if (withStars.size() == 1u) {
        CHECK(withStars.front() == "sky.frag");
    }
}

TEST_CASE("every fogged shader resolves toward the shared sky colour", "[shaders][daynight]")
{
    // The other half of the same invariant, and the reason the fix above was a
    // deletion rather than a second star field in common.glsl: the horizon has no
    // seam only while the fog target and the dome are literally the same
    // function. A shader that fogged toward a constant would band visibly at
    // dawn and dusk, where the gradient is steepest.
    for (const std::string_view name : {"chunk.frag", "subvoxel.frag", "water.frag"}) {
        const std::string source = stripComments(readFile(shaderDirectory() / name));
        INFO("shader: " << name);
        CHECK(contains(source, "voxlFogFactor("));
        CHECK(contains(source, "voxlSkyColour("));
    }

    // And the dome itself must use it unmodified as its base.
    const std::string sky = stripComments(readFile(shaderDirectory() / "sky.frag"));
    CHECK(contains(sky, "voxlSkyColour(dir)"));
}
