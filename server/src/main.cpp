/**
 * @file main.cpp
 * @brief Clean entrypoint instantiating the pipeline tracking metrics for the AngelScript LSP.
 */
#include "LspServer.h"

// CRITICAL FIX: Explicit Win32 binary stream headers to override carriage return text-mode translations
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/**
 * @brief Main execution entry point for the AngelScript Language Server Protocol (LSP) application.
 * * Instantiates the server context, applies required platform-specific configurations for
 * standard input/output streams, and launches the primary event-blocking execution loop.
 * * @return Integer exit status code representing the program's termination state (0 upon successful execution).
 * @warning On Windows systems, standard streams must be explicitly forced into binary mode to prevent
 * the C runtime from corrupting specific byte lengths during inter-process communication.
 */
int main()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    AngelScriptLSPServer server;
    server.Run();
    return 0;
}
