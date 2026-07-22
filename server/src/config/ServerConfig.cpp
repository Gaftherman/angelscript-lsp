/**
 * @file ServerConfig.cpp
 * @brief Implementation of ServerConfig CLI arguments parser.
 * @ingroup Config
 */

#include "config/ServerConfig.h"
#include <cstring>

namespace angel_lsp
{

    /**
     * @brief Parses CLI arguments into a ServerConfig object.
     * 
     * Supported flags:
     * - --disable-hover
     * - --disable-definition
     * - --disable-completion
     * - --disable-semantic-tokens
     * - --disable-signature-help
     * - --disable-predefined
     */
    ServerConfig ServerConfig::FromArgs(int argc, char **argv)
    {
        ServerConfig config;

        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--disable-hover") == 0)
            {
                config.features.enableHover = false;
            }
            else if (std::strcmp(argv[i], "--disable-definition") == 0)
            {
                config.features.enableDefinition = false;
            }
            else if (std::strcmp(argv[i], "--disable-completion") == 0)
            {
                config.features.enableCompletion = false;
            }
            else if (std::strcmp(argv[i], "--disable-semantic-tokens") == 0)
            {
                config.features.enableSemanticTokens = false;
            }
            else if (std::strcmp(argv[i], "--disable-signature-help") == 0)
            {
                config.features.enableSignatureHelp = false;
            }
            else if (std::strcmp(argv[i], "--disable-predefined") == 0)
            {
                config.features.enablePredefinedLoader = false;
            }
        }

        return config;
    }

} // namespace angel_lsp
