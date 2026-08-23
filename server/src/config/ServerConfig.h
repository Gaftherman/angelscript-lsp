#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        bool enableDocumentSymbols = true;
        bool enableWorkspaceSymbols = true;
        bool enableReferences = true;
        bool enableRename = true;
        bool enableDocumentHighlight = true;
        bool enableFoldingRange = true;
        bool enableInlayHints = true;
        bool enableCodeAction = true;
        bool enableFormatting = true;
        bool enableDocumentLink = true;
        bool enableTypeConversionChecks = true;
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
        std::vector<std::string> searchDirectories;

        /**
         * @brief Predefined stub files to load by path, on top of the workspace scan.
         *
         * The scan only ever finds stubs that live inside a workspace folder, but a host
         * application's declarations usually ship with the application rather than with the
         * scripts. Absolute entries are used as-is; relative ones are resolved against each
         * workspace folder. Distinct from Info::predefinedFileExtension, which is the suffix that
         * decides whether a file found by the scan is a stub at all.
         */
        std::vector<std::string> predefinedFiles;

        /**
         * @brief Per-rule diagnostic severity overrides, keyed by diagnostic code.
         *
         * Values are the LSP severity names ("error", "warning", "information", "hint"). Kept as
         * strings because DiagnosticSeverity is a Layer 2 type and this is Layer 1; the server
         * converts them once at startup.
         */
        std::unordered_map<std::string, std::string> diagnosticSeverities;
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

