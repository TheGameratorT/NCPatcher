#include <optional>

#include "app/application.hpp"

/**
 * Entry point for the NCPatcher application.
 * This simply creates and runs the Application instance.
 */
int main(int argc, char* argv[])
{
    ncp::Application app;

    // Initialization stops the process on its own for --help, --version and a
    // command line that did not parse; only then does it hand back a code.
    if (const std::optional<int> stop = app.initialize(argc, argv))
        return *stop;

    return app.run();
}
