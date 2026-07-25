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
    angel_lsp::Server server(config);
    server.Run();

    return 0;
}