#include "render/Shader.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#include <glm/gtc/type_ptr.hpp>

namespace voxl {
namespace {

namespace fs = std::filesystem;

/// Deepest `#include` chain we will follow. A cycle is the realistic failure and
/// a hard cap turns it into a clear error instead of a stack overflow.
constexpr int kMaxIncludeDepth = 8;

[[nodiscard]] const char* stageName(GLenum stage) noexcept
{
    switch (stage) {
        case GL_VERTEX_SHADER:   return "vertex";
        case GL_FRAGMENT_SHADER: return "fragment";
        default:                 return "unknown";
    }
}

[[nodiscard]] bool readTextFile(const fs::path& path, std::string& out)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        VOXL_LOG_ERROR("shader source not found: {}", path.string());
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

/// Extracts the path from `#include "..."`. Returns false when the line is not an
/// include directive at all.
[[nodiscard]] bool parseInclude(std::string_view line, std::string_view& target)
{
    std::size_t cursor = line.find_first_not_of(" \t");
    if (cursor == std::string_view::npos || line[cursor] != '#') {
        return false;
    }
    ++cursor;
    cursor = line.find_first_not_of(" \t", cursor);
    if (cursor == std::string_view::npos) {
        return false;
    }
    constexpr std::string_view kKeyword = "include";
    if (line.compare(cursor, kKeyword.size(), kKeyword) != 0) {
        return false;
    }
    cursor += kKeyword.size();

    const std::size_t open = line.find('"', cursor);
    if (open == std::string_view::npos) {
        return false;
    }
    const std::size_t close = line.find('"', open + 1);
    if (close == std::string_view::npos) {
        return false;
    }
    target = line.substr(open + 1, close - open - 1);
    return true;
}

/// Flattens `#include` directives into a single source string.
///
/// No `#line` directives are emitted on purpose. The driver then reports errors
/// in terms of the flattened text, which is exactly what `logNumberedSource()`
/// prints on failure - so a reported line number can always be matched to a
/// printed line. Emitting `#line` would restore per-file numbering but make the
/// dump and the driver disagree, which is worse than no mapping at all.
[[nodiscard]] bool flatten(const fs::path& path, std::string& out,
                           std::vector<fs::path>& dependencies, int depth)
{
    if (depth > kMaxIncludeDepth) {
        VOXL_LOG_ERROR("shader include depth exceeded at {} - cyclic #include?", path.string());
        return false;
    }

    std::string source;
    if (!readTextFile(path, source)) {
        return false;
    }
    dependencies.push_back(path);

    const fs::path directory = path.parent_path();

    std::size_t lineStart = 0;
    while (lineStart <= source.size()) {
        std::size_t lineEnd = source.find('\n', lineStart);
        const bool  lastLine = lineEnd == std::string::npos;
        if (lastLine) {
            lineEnd = source.size();
        }
        const std::string_view line{source.data() + lineStart, lineEnd - lineStart};

        std::string_view includeTarget;
        if (parseInclude(line, includeTarget)) {
            const fs::path resolved = directory / fs::path(includeTarget);
            if (!flatten(resolved, out, dependencies, depth + 1)) {
                VOXL_LOG_ERROR("  included from {}", path.string());
                return false;
            }
        } else {
            out.append(line);
            out.push_back('\n');
        }

        if (lastLine) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    return true;
}

/// Prints the source with line numbers so a driver message like
/// "0(57) : error C1503" can be read without counting lines by hand.
void logNumberedSource(std::string_view name, GLenum stage, std::string_view source)
{
    // Assembled into one message rather than one log call per line: the source
    // is the context for the error immediately above it, and interleaving a
    // hundred separately timestamped lines makes it far harder to read.
    std::string listing;
    listing.reserve(source.size() + source.size() / 8);
    listing += std::format("--- {} {} shader source ---\n", name, stageName(stage));

    std::size_t lineNumber = 1;
    std::size_t cursor     = 0;
    while (cursor <= source.size()) {
        std::size_t end = source.find('\n', cursor);
        if (end == std::string_view::npos) {
            end = source.size();
        }
        listing += std::format("{:4}| {}\n", lineNumber, source.substr(cursor, end - cursor));
        if (end == source.size()) {
            break;
        }
        cursor = end + 1;
        ++lineNumber;
    }
    listing += std::format("--- end {} {} shader source ---", name, stageName(stage));
    VOXL_LOG_ERROR("{}", listing);
}

[[nodiscard]] std::string shaderInfoLog(GLuint shader)
{
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    // The driver includes the terminating NUL in the reported length.
    while (!log.empty() && (log.back() == '\0' || log.back() == '\n' || log.back() == '\r')) {
        log.pop_back();
    }
    return log;
}

[[nodiscard]] std::string programInfoLog(GLuint program)
{
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(length), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    while (!log.empty() && (log.back() == '\0' || log.back() == '\n' || log.back() == '\r')) {
        log.pop_back();
    }
    return log;
}

/// Compiles one stage. Returns 0 on failure, having logged the driver log and the
/// numbered source.
[[nodiscard]] GLuint compileStage(std::string_view name, GLenum stage, std::string_view source)
{
    const GLuint shader = glCreateShader(stage);
    if (shader == 0) {
        VOXL_LOG_ERROR("glCreateShader failed for {} {} - no GL context?", name, stageName(stage));
        return 0;
    }

    const auto*     text   = source.data();
    const GLint     length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    const std::string log = shaderInfoLog(shader);
    if (compiled == GL_FALSE) {
        VOXL_LOG_ERROR("{} {} shader FAILED to compile:\n{}", name, stageName(stage),
                       log.empty() ? "(driver produced no log)" : log);
        logNumberedSource(name, stage, source);
        glDeleteShader(shader);
        return 0;
    }
    if (!log.empty()) {
        // Warnings are worth surfacing: implicit conversions and unused varyings
        // are how a shader ends up subtly wrong on one vendor only.
        VOXL_LOG_WARN("{} {} shader compiled with warnings:\n{}", name, stageName(stage), log);
    }
    return shader;
}

[[nodiscard]] fs::file_time_type writeTimeOrEpoch(const fs::path& path) noexcept
{
    std::error_code error;
    const auto      time = fs::last_write_time(path, error);
    return error ? fs::file_time_type{} : time;
}

}  // namespace

// ------------------------------------------------------------ ShaderProgram --

ShaderProgram::~ShaderProgram()
{
    destroy();
}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : m_program(other.m_program),
      m_name(std::move(other.m_name)),
      m_vertexPath(std::move(other.m_vertexPath)),
      m_fragmentPath(std::move(other.m_fragmentPath)),
      m_dependencies(std::move(other.m_dependencies)),
      m_uniforms(std::move(other.m_uniforms)),
      m_warnedMissing(std::move(other.m_warnedMissing))
{
    other.m_program = 0;
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept
{
    if (this != &other) {
        destroy();
        m_program       = other.m_program;
        m_name          = std::move(other.m_name);
        m_vertexPath    = std::move(other.m_vertexPath);
        m_fragmentPath  = std::move(other.m_fragmentPath);
        m_dependencies  = std::move(other.m_dependencies);
        m_uniforms      = std::move(other.m_uniforms);
        m_warnedMissing = std::move(other.m_warnedMissing);
        other.m_program = 0;
    }
    return *this;
}

void ShaderProgram::destroy() noexcept
{
    if (m_program != 0) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
    m_uniforms.clear();
    m_warnedMissing.clear();
}

bool ShaderProgram::loadFromSource(std::string name, std::string_view vertexSource,
                                   std::string_view fragmentSource)
{
    m_name = std::move(name);

    const GLuint vertexShader = compileStage(m_name, GL_VERTEX_SHADER, vertexSource);
    if (vertexShader == 0) {
        return false;
    }
    const GLuint fragmentShader = compileStage(m_name, GL_FRAGMENT_SHADER, fragmentSource);
    if (fragmentShader == 0) {
        glDeleteShader(vertexShader);
        return false;
    }

    const bool linked = link(vertexShader, fragmentShader);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return linked;
}

bool ShaderProgram::loadFromFiles(std::string name, fs::path vertexPath, fs::path fragmentPath)
{
    m_name         = std::move(name);
    m_vertexPath   = std::move(vertexPath);
    m_fragmentPath = std::move(fragmentPath);
    return reload();
}

bool ShaderProgram::reload()
{
    if (m_vertexPath.empty() || m_fragmentPath.empty()) {
        VOXL_LOG_ERROR("shader '{}' was not loaded from files and cannot be reloaded", m_name);
        return false;
    }

    std::vector<fs::path> dependencies;
    std::string           vertexSource;
    std::string           fragmentSource;
    if (!flatten(m_vertexPath, vertexSource, dependencies, 0) ||
        !flatten(m_fragmentPath, fragmentSource, dependencies, 0)) {
        // Distinguish the two cases: on a first load there is nothing to fall
        // back to and the caller gets a null program, whereas a failed hot
        // reload is survivable and the running frame keeps working shaders.
        VOXL_LOG_ERROR("shader '{}' source could not be read; {}", m_name,
                       m_program != 0 ? "keeping the previously linked program"
                                      : "the program was never linked");
        return false;
    }

    // Record the timestamps even when compilation fails: otherwise a broken save
    // is retried every frame and floods the log with the same error.
    m_dependencies.clear();
    m_dependencies.reserve(dependencies.size());
    for (const fs::path& path : dependencies) {
        m_dependencies.push_back(SourceFile{path, writeTimeOrEpoch(path)});
    }

    const GLuint vertexShader = compileStage(m_name, GL_VERTEX_SHADER, vertexSource);
    if (vertexShader == 0) {
        return false;
    }
    const GLuint fragmentShader = compileStage(m_name, GL_FRAGMENT_SHADER, fragmentSource);
    if (fragmentShader == 0) {
        glDeleteShader(vertexShader);
        return false;
    }

    const bool linked = link(vertexShader, fragmentShader);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return linked;
}

bool ShaderProgram::link(GLuint vertexShader, GLuint fragmentShader)
{
    const GLuint program = glCreateProgram();
    if (program == 0) {
        VOXL_LOG_ERROR("glCreateProgram failed for shader '{}'", m_name);
        return false;
    }
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    // Detach immediately: the program keeps its own reference, and leaving the
    // shaders attached keeps their source alive in driver memory for nothing.
    glDetachShader(program, vertexShader);
    glDetachShader(program, fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    const std::string log = programInfoLog(program);

    if (linked == GL_FALSE) {
        VOXL_LOG_ERROR("shader '{}' FAILED to link:\n{}", m_name,
                       log.empty() ? "(driver produced no log)" : log);
        glDeleteProgram(program);
        return false;
    }
    if (!log.empty()) {
        VOXL_LOG_WARN("shader '{}' linked with warnings:\n{}", m_name, log);
    }

    // Only swap in the new program once it is known good, so a bad hot reload
    // leaves the running frame with working shaders.
    const bool replacing = m_program != 0;
    if (replacing) {
        glDeleteProgram(m_program);
    }
    m_program = program;
    m_uniforms.clear();
    m_warnedMissing.clear();
    cacheUniforms();

    VOXL_LOG_INFO("shader '{}' {}: {} active uniform(s)", m_name,
                  replacing ? "reloaded" : "linked", m_uniforms.size());
    return true;
}

void ShaderProgram::cacheUniforms()
{
    GLint count = 0;
    glGetProgramiv(m_program, GL_ACTIVE_UNIFORMS, &count);
    GLint maxNameLength = 0;
    glGetProgramiv(m_program, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxNameLength);
    if (count <= 0 || maxNameLength <= 0) {
        return;
    }

    std::string buffer(static_cast<std::size_t>(maxNameLength), '\0');
    for (GLint index = 0; index < count; ++index) {
        GLsizei written = 0;
        GLint   size    = 0;
        GLenum  type    = 0;
        glGetActiveUniform(m_program, static_cast<GLuint>(index), maxNameLength, &written, &size,
                           &type, buffer.data());
        if (written <= 0) {
            continue;
        }
        std::string uniform(buffer.data(), static_cast<std::size_t>(written));

        // Uniforms that live inside a uniform block have no location; skipping
        // them keeps the cache honest about what setInt/setVec3 can reach.
        const GLint location = glGetUniformLocation(m_program, uniform.c_str());
        if (location < 0) {
            continue;
        }
        m_uniforms.emplace(uniform, location);

        // Array uniforms are reported as "name[0]"; also cache the bare name so
        // callers can write uniformLocation("uKernel").
        const std::size_t bracket = uniform.find('[');
        if (bracket != std::string::npos) {
            m_uniforms.emplace(uniform.substr(0, bracket), location);
        }
    }
}

bool ShaderProgram::sourcesChanged() const
{
    for (const SourceFile& file : m_dependencies) {
        if (writeTimeOrEpoch(file.path) != file.writeTime) {
            return true;
        }
    }
    return false;
}

bool ShaderProgram::reloadIfChanged()
{
    return sourcesChanged() ? reload() : false;
}

void ShaderProgram::use() const
{
    glUseProgram(m_program);
}

GLint ShaderProgram::uniformLocation(std::string_view uniform) const
{
    const auto found = m_uniforms.find(uniform);
    if (found != m_uniforms.end()) {
        return found->second;
    }
    // Warn once. An unused uniform is legal (the compiler strips it), but a
    // misspelled one is a bug that otherwise shows up as "the value never
    // changes anything".
    if (std::find(m_warnedMissing.begin(), m_warnedMissing.end(), uniform) ==
        m_warnedMissing.end()) {
        m_warnedMissing.emplace_back(uniform);
        VOXL_LOG_WARN("shader '{}' has no active uniform '{}'", m_name, uniform);
    }
    return -1;
}

void ShaderProgram::setInt(GLint location, GLint value) const
{
    if (location >= 0) {
        glProgramUniform1i(m_program, location, value);
    }
}

void ShaderProgram::setUInt(GLint location, GLuint value) const
{
    if (location >= 0) {
        glProgramUniform1ui(m_program, location, value);
    }
}

void ShaderProgram::setFloat(GLint location, float value) const
{
    if (location >= 0) {
        glProgramUniform1f(m_program, location, value);
    }
}

void ShaderProgram::setVec2(GLint location, const glm::vec2& value) const
{
    if (location >= 0) {
        glProgramUniform2fv(m_program, location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setVec3(GLint location, const glm::vec3& value) const
{
    if (location >= 0) {
        glProgramUniform3fv(m_program, location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setVec4(GLint location, const glm::vec4& value) const
{
    if (location >= 0) {
        glProgramUniform4fv(m_program, location, 1, glm::value_ptr(value));
    }
}

void ShaderProgram::setMat4(GLint location, const glm::mat4& value) const
{
    if (location >= 0) {
        glProgramUniformMatrix4fv(m_program, location, 1, GL_FALSE, glm::value_ptr(value));
    }
}

void ShaderProgram::setInt(std::string_view uniform, GLint value) const
{
    setInt(uniformLocation(uniform), value);
}

void ShaderProgram::setFloat(std::string_view uniform, float value) const
{
    setFloat(uniformLocation(uniform), value);
}

void ShaderProgram::setVec2(std::string_view uniform, const glm::vec2& value) const
{
    setVec2(uniformLocation(uniform), value);
}

void ShaderProgram::setVec3(std::string_view uniform, const glm::vec3& value) const
{
    setVec3(uniformLocation(uniform), value);
}

void ShaderProgram::setVec4(std::string_view uniform, const glm::vec4& value) const
{
    setVec4(uniformLocation(uniform), value);
}

void ShaderProgram::setMat4(std::string_view uniform, const glm::mat4& value) const
{
    setMat4(uniformLocation(uniform), value);
}

// ------------------------------------------------------------ ShaderLibrary --

ShaderLibrary::ShaderLibrary(fs::path shaderDirectory) : m_directory(std::move(shaderDirectory))
{
    std::error_code error;
    if (!fs::exists(m_directory, error)) {
        VOXL_LOG_WARN("shader directory does not exist: {}", m_directory.string());
    }
}

ShaderProgram* ShaderLibrary::load(std::string_view name)
{
    const std::string stem{name};
    return load(name, stem + ".vert", stem + ".frag");
}

ShaderProgram* ShaderLibrary::load(std::string_view name, std::string_view vertexFile,
                                   std::string_view fragmentFile)
{
    if (ShaderProgram* existing = find(name); existing != nullptr) {
        return existing;
    }

    auto program = std::make_unique<ShaderProgram>();
    if (!program->loadFromFiles(std::string{name}, m_directory / fs::path(vertexFile),
                                m_directory / fs::path(fragmentFile))) {
        return nullptr;
    }

    ShaderProgram* raw = program.get();
    m_programs.emplace(std::string{name}, std::move(program));
    return raw;
}

ShaderProgram* ShaderLibrary::find(std::string_view name) noexcept
{
    const auto found = m_programs.find(name);
    return found == m_programs.end() ? nullptr : found->second.get();
}

const ShaderProgram* ShaderLibrary::find(std::string_view name) const noexcept
{
    const auto found = m_programs.find(name);
    return found == m_programs.end() ? nullptr : found->second.get();
}

std::size_t ShaderLibrary::reloadChanged()
{
    std::size_t reloaded = 0;
    for (auto& [name, program] : m_programs) {
        if (program->reloadIfChanged()) {
            ++reloaded;
        }
    }
    return reloaded;
}

std::size_t ShaderLibrary::reloadAll()
{
    std::size_t reloaded = 0;
    for (auto& [name, program] : m_programs) {
        if (program->reload()) {
            ++reloaded;
        }
    }
    return reloaded;
}

}  // namespace voxl
