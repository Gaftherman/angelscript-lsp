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
        bool enableImplementation = true;
        bool enableSelectionRange = true;
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
     * @brief Host engine options that change what a script is allowed to say.
     *
     * AngelScript is not one language but a family of them: the host picks its dialect with
     * asIScriptEngine::SetEngineProperty before it compiles anything, and several of those choices
     * decide whether a given line is legal. None of it is observable from script text, which is
     * why the rules that depend on one used to be unimplementable and sat in i18n.cpp as codes
     * nobody emitted. Telling the server which engine it is reading for is the missing input.
     *
     * Only the properties a rule actually consults are listed. AngelScript has forty of them and
     * most decide runtime behaviour - stack sizes, garbage collection, bytecode - which no reader
     * of source can see the effect of. A setting nobody reads is the same mistake as a message
     * nobody emits, so each field here names the rule it gates.
     *
     * Defaults match the engine's own defaults, so a host that sets nothing gets a server that
     * judges its scripts the way its engine will.
     */
    struct EngineProperties
    {
        /**
         * @brief asEP_ALLOW_UNSAFE_REFERENCES (engine default: false).
         *
         * With it off, `&` on a parameter means `&inout` and is only legal for an object type that
         * supports handles - `void f(int &x)` and `void f(int &inout x)` are both refused, with the
         * same message. With it on, primitives may be passed by reference and a script may declare
         * a standalone reference variable. Gates as-err-inout-on-primitive.
         */
        bool allowUnsafeReferences = false;

        /**
         * @brief asEP_PRIVATE_PROP_AS_PROTECTED (engine default: false).
         *
         * When set, a private member follows the protected rule instead: a derived class may reach
         * it. Gates the private branch of AccessChecker.
         */
        bool privatePropAsProtected = false;

        /**
         * @brief asEP_DISALLOW_GLOBAL_VARS (engine default: false).
         *
         * Hosts that give each script its own isolated state turn this on, and then a global
         * variable declaration is a compile error. Gates as-err-global-vars-disallowed.
         */
        bool disallowGlobalVars = false;
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
        EngineProperties engine;
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

