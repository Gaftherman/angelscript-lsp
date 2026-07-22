#pragma once

#include <string>
#include <vector>

namespace angel_lsp
{

    /**
     * @brief Feature flags for toggling LSP capabilities at runtime.
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
     * @brief Centralized configuration settings for the AngelScript Language Server.
     */
    struct ServerConfig
    {
        FeatureFlags features;

        /**
         * @brief Parses server configuration settings from command line arguments.
         * @param argc Number of command line arguments.
         * @param argv Array of command line argument string pointers.
         * @return Initialized ServerConfig instance.
         */
        static ServerConfig FromArgs(int argc, char **argv);
    };

} // namespace angel_lsp
