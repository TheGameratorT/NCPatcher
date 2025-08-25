#include "app/application.hpp"

/**
 * Entry point for the NCPatcher application.
 * This simply creates and runs the Application instance.
 */
int main(int argc, char* argv[])
{
    ncp::Application app;
    
    int initResult = app.initialize(argc, argv);
    if (initResult != 0) {
        return initResult;
    }
    
    return app.run();
}
