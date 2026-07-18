#include <iostream>
#include "lsp/Server.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main()
{
    try
    {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif

        angel_lsp::Server server;
        server.Run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
