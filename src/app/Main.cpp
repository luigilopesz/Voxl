// Voxl entry point.
//
// Main does nothing but establish logging, construct the Application and
// translate an escaping exception into a logged failure and a non-zero exit
// code. Every subsystem lives in Application so that construction and - more
// importantly - destruction order is stated in one place.

#include "app/Application.hpp"
#include "core/Log.hpp"

#include <exception>

int main()
{
    voxl::setLogFile("voxl.log");
    VOXL_LOG_INFO("Voxl starting up");

    int exitCode = 1;
    try {
        voxl::Application application;
        exitCode = application.run();
    } catch (const std::exception& error) {
        VOXL_LOG_FATAL("Unhandled exception: {}", error.what());
    } catch (...) {
        VOXL_LOG_FATAL("Unhandled non-standard exception");
    }

    VOXL_LOG_INFO("Voxl exiting with code {}", exitCode);
    voxl::shutdownLogging();
    return exitCode;
}
