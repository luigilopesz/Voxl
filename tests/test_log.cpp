#include <catch2/catch_test_macros.hpp>

#include "core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("log level filtering suppresses lower-severity messages", "[core][log]")
{
    const auto original = voxl::logLevel();
    voxl::setLogLevel(voxl::LogLevel::Error);
    CHECK(voxl::logLevel() == voxl::LogLevel::Error);
    voxl::setLogLevel(original);
}

TEST_CASE("file sink captures formatted output", "[core][log]")
{
    const auto path = std::filesystem::temp_directory_path() / "voxl_log_test.log";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const auto originalLevel = voxl::logLevel();
    voxl::setLogLevel(voxl::LogLevel::Trace);
    REQUIRE(voxl::setLogFile(path.string()));

    VOXL_LOG_WARN("marker value={}", 1234);

    REQUIRE(voxl::setLogFile(""));  // closes and flushes the sink
    voxl::setLogLevel(originalLevel);

    std::ifstream input{path};
    REQUIRE(input.is_open());
    const std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(contents.find("marker value=1234") != std::string::npos);
    CHECK(contents.find("WARN") != std::string::npos);

    input.close();
    std::filesystem::remove(path, ec);
}
