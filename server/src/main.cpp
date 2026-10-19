/**
 * @file main.cpp
 * @brief Entry point for the AngelScript Language Server executable (angel_lsp).
 * @ingroup Server
 */

#include "analysis/EngineConfigConverter.h"
#include "lsp/Server.h"
#include <fstream>
#include <iostream>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace
{
int RunConvertConfig(const char* inputPath, const char* outputPath)
{
    std::ifstream inFile(inputPath, std::ios::binary);
    if (!inFile.is_open())
    {
        std::cerr << "angel_lsp: error: could not open input config file '" << inputPath << "'\n";
        return 1;
    }
    std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
    auto result = angel_lsp::analysis::ConvertEngineConfigToPredefined(content);

    if (outputPath != nullptr && outputPath[0] != '\0')
    {
        std::ofstream outFile(outputPath, std::ios::binary);
        if (!outFile.is_open())
        {
            std::cerr << "angel_lsp: error: could not open output file '" << outputPath << "'\n";
            return 1;
        }
        outFile << result.predefinedContent;
        std::cout << "Successfully converted '" << inputPath << "' -> '" << outputPath << "'\n";
    }
    else
    {
        std::cout << result.predefinedContent;
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--convert-config")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Usage: angel_lsp --convert-config <as_config.txt> [output.as.predefined]\n";
                return 1;
            }
            const char* inPath = argv[i + 1];
            const char* outPath = (i + 2 < argc && argv[i + 2][0] != '-') ? argv[i + 2] : nullptr;
            return RunConvertConfig(inPath, outPath);
        }
    }

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
    catch (const std::exception& e)
    {
        std::cerr << "angel_lsp: fatal error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
