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

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    angel_lsp::config::ServerConfig config = angel_lsp::config::FromArgs(argc, argv);
    if (config.info.showHelp || config.info.showVersion)
    {
        return 0;
    }

    // Run() already recovers from malformed messages and closes cleanly on a dead transport. This
    // guard is for what it cannot anticipate: anything escaping here would otherwise reach
    // std::terminate, and terminate skips ~Server - which is what stops and joins the analysis and
    // workspace threads while the state they read is still alive.
    try
    {
        angel_lsp::Server server(config);
        server.Run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "angel_lsp: fatal error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}