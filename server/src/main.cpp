#include <iostream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "lsp/Server.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main() {
    try {
        // Setup spdlog to write to angelscript-lsp.log to avoid corrupting JSON-RPC stdout
        auto logger = spdlog::basic_logger_mt("lsp_logger", "angelscript-lsp.log", true);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_on(spdlog::level::debug);
        
        spdlog::info("AngelScript LSP Server starting...");

#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif

        angel_lsp::Server server;
        server.Run();

        spdlog::info("AngelScript LSP Server stopped.");
    } catch (const std::exception& e) {
        // Fallback to std::cerr if spdlog fails
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
