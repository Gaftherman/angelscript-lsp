/**
 * @file main.cpp
 * @brief Clean entrypoint instantiating the pipeline tracking metrics.
 */

#include "LspServer.h"

// Explicit Win32 system headers to guarantee standard stream binding rules on execution startup
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    AngelScriptLSPServer server;
    server.Run();
    return 0;
}