#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace angel_lsp::config
{
    /**
     * @brief Feature flags for toggling individual LSP capabilities.
     */
    struct FeatureFlags
    {
        bool enableHover = true;
        bool enableDefinition = true;
        bool enableCompletion = true;
        bool enableSemanticTokens = true;
        bool enableSignatureHelp = true;
        bool enablePredefinedLoader = true;
    };

    /**
     * @brief General server information and CLI options.
     */
    struct Info
    {
        std::string name = "AngelScript Language Server";
        std::string version = "1.0.0";
        std::string fileExtension = ".as";
        std::string predefinedFileExtension = ".as.predefined";
        std::string locale = "en";
        bool showHelp = false;
        bool showVersion = false;
    };

    /**
     * @brief Type configuration for AngelScript analysis.
     */
    struct TypeConfig
    {
        std::string stringTypeName = "string";
        std::string arrayTypeName = "array";
        std::unordered_set<std::string> registeredSymbols;
    };

    /**
     * @brief Full configuration bundle for the LSP server.
     */
    struct ServerConfig
    {
        FeatureFlags features;
        Info info;
        TypeConfig types;
    };

    /**
     * @brief Parses CLI arguments into a ServerConfig instance.
     * @param argc Number of command-line arguments.
     * @param argv Array of command-line argument strings.
     * @return Initialized ServerConfig with parsed options.
     */
    ServerConfig FromArgs(int argc, char **argv);

    /**
     * @brief Prints the CLI help message to standard output.
     */
    void PrintHelp();

    /**
     * @brief Prints the version information to standard output.
     */
    void PrintVersion();
}

