#pragma once

// GLSL program compilation, uniform lookup and hot reload.
//
// A silently failing shader is the single most expensive class of bug in a
// renderer: the screen is black, nothing asserts, and the cause is a one-line
// driver message nobody printed. So every failure path here logs the *complete*
// driver log plus the line-numbered source that produced it, and a failed reload
// keeps the previously working program bound rather than leaving a hole.
//
// Thread safety: NONE. GL objects, main thread only.

#include <glad/gl.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace voxl {

/// One linked vertex+fragment program.
///
/// Uniform locations are enumerated once at link time (glGetActiveUniform) rather
/// than looked up lazily, so the per-frame path is a hash lookup with no driver
/// round trip, and a typo in a uniform name is reported once instead of silently
/// writing to location -1 forever.
class ShaderProgram {
public:
    ShaderProgram() noexcept = default;
    ~ShaderProgram();

    ShaderProgram(const ShaderProgram&)            = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;

    /// Compiles and links from in-memory source. Returns false and leaves the
    /// object unchanged (still valid if it was valid) on any error.
    bool loadFromSource(std::string name, std::string_view vertexSource,
                        std::string_view fragmentSource);

    /// Compiles and links from disk. `#include "relative/path.glsl"` is resolved
    /// relative to the including file's directory; included files are tracked so
    /// `reloadIfChanged()` notices edits to shared code as well.
    bool loadFromFiles(std::string name, std::filesystem::path vertexPath,
                       std::filesystem::path fragmentPath);

    /// Re-reads and relinks unconditionally. Returns true when a new program was
    /// installed. A failure logs and keeps the old program.
    bool reload();

    /// True when any source file (including transitive includes) has a newer
    /// write time than the one recorded at load.
    [[nodiscard]] bool sourcesChanged() const;

    /// Convenience for the hot-reload key: reloads only if something changed.
    bool reloadIfChanged();

    void               use() const;
    [[nodiscard]] bool valid() const noexcept { return m_program != 0; }
    [[nodiscard]] GLuint id() const noexcept { return m_program; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    /// -1 when the name is not an active uniform (never declared, or optimised
    /// away because it is unused). Warned about once per name.
    [[nodiscard]] GLint uniformLocation(std::string_view uniform) const;

    /// Setters take the program by id (glProgramUniform*), so they do not require
    /// the program to be current and cannot disturb the bound program.
    void setInt(GLint location, GLint value) const;
    void setUInt(GLint location, GLuint value) const;
    void setFloat(GLint location, float value) const;
    void setVec2(GLint location, const glm::vec2& value) const;
    void setVec3(GLint location, const glm::vec3& value) const;
    void setVec4(GLint location, const glm::vec4& value) const;
    void setMat4(GLint location, const glm::mat4& value) const;

    void setInt(std::string_view uniform, GLint value) const;
    void setFloat(std::string_view uniform, float value) const;
    void setVec2(std::string_view uniform, const glm::vec2& value) const;
    void setVec3(std::string_view uniform, const glm::vec3& value) const;
    void setVec4(std::string_view uniform, const glm::vec4& value) const;
    void setMat4(std::string_view uniform, const glm::mat4& value) const;

private:
    struct SourceFile {
        std::filesystem::path           path;
        std::filesystem::file_time_type writeTime{};
    };

    /// Heterogeneous lookup so `uniformLocation("uFoo")` does not allocate a
    /// std::string on every call.
    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
    };

    bool link(GLuint vertexShader, GLuint fragmentShader);
    void cacheUniforms();
    void destroy() noexcept;

    GLuint      m_program = 0;
    std::string m_name;

    std::filesystem::path   m_vertexPath;
    std::filesystem::path   m_fragmentPath;
    std::vector<SourceFile> m_dependencies;

    mutable std::unordered_map<std::string, GLint, StringHash, std::equal_to<>> m_uniforms;
    mutable std::vector<std::string>                                           m_warnedMissing;
};

/// Owns the engine's programs and reloads them together.
///
/// Programs are held by unique_ptr so that a pointer handed out to the renderer
/// stays valid across further `load()` calls (rehashing the map would otherwise
/// move the program objects).
class ShaderLibrary {
public:
    explicit ShaderLibrary(std::filesystem::path shaderDirectory);

    /// Loads `<name>.vert` + `<name>.frag` from the shader directory. Returns
    /// nullptr on failure. Loading the same name twice returns the existing
    /// program without recompiling.
    ShaderProgram* load(std::string_view name);

    /// Explicit file names, for programs whose stages do not share a stem.
    ShaderProgram* load(std::string_view name, std::string_view vertexFile,
                        std::string_view fragmentFile);

    [[nodiscard]] ShaderProgram*       find(std::string_view name) noexcept;
    [[nodiscard]] const ShaderProgram* find(std::string_view name) const noexcept;

    /// Reloads every program whose source changed. Returns how many relinked.
    std::size_t reloadChanged();

    /// Reloads everything unconditionally. Returns how many relinked.
    std::size_t reloadAll();

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return m_directory; }
    [[nodiscard]] std::size_t                  size() const noexcept { return m_programs.size(); }

private:
    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept
        {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::filesystem::path m_directory;
    std::unordered_map<std::string, std::unique_ptr<ShaderProgram>, StringHash, std::equal_to<>>
        m_programs;
};

}  // namespace voxl
