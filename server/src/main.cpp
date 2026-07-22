/**
 * @file main.cpp
 * @brief Entry point for the AngelScript Language Server executable (angel_lsp).
 * @ingroup Server
 */

#include <iostream>
#include "lsp/Server.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/**
 * @brief Main entry point for angel_lsp server executable.
 *
 * @param[in] argc Command line argument count.
 * @param[in] argv Command line argument string array.
 * @return int Exit code (0 for clean shutdown, 1 on fatal error).
 */
int main(int argc, char **argv)
{
    try
    {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif

        angel_lsp::ServerConfig config = angel_lsp::ServerConfig::FromArgs(argc, argv);
        angel_lsp::Server server(config);
        server.Run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}