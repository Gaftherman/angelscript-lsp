#pragma once

#include <string_view>
#include <unordered_set>
#include <string>

namespace angel_lsp::config
{
    struct FeatureFlags
    {
        bool enableHover = true;
        bool enableDefinition = true;
        bool enableCompletion = true;
        bool enableSemanticTokens = true;
        bool enableSignatureHelp = true;
        bool enablePredefinedLoader = true;
    };

    struct Info
    {
        std::string_view name;
        std::string_view version;
        std::string_view fileExtension;
        std::string_view predefinedFileExtension;
    };

    struct TypeConfig
    {
        std::string_view stringTypeName = "string";
        std::string_view arrayTypeName = "array";
        std::unordered_set<std::string> registeredSymbols;
    };

    struct ServerConfig
    {
        FeatureFlags features;
        Info info;
        TypeConfig types;
    };

    ServerConfig FromArgs(int argc, char **argv);
}
