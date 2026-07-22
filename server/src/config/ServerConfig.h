/**
 * @file ServerConfig.h
 * @brief Server configuration parameters and feature flags parsing.
 * @ingroup Config
 */

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
     * @note Thread-safe immutable configuration container once initialized.
     */
    struct ServerConfig
    {
        FeatureFlags features;

        /**
         * @brief Parses server configuration settings from command line arguments.
         *
         * @param[in] argc Number of command line arguments.
         * @param[in] argv Array of command line argument string pointers.
         * @return ServerConfig Initialized ServerConfig instance.
         */
        static ServerConfig FromArgs(int argc, char **argv);
    };

} // namespace angel_lsp
