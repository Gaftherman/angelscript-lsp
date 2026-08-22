#include "config/ServerConfig.h"
#include <iostream>
#include <string>
#include <string_view>
#include <optional>
#include <algorithm>
#include <cctype>

namespace angel_lsp::config
{
    namespace
    {
        std::string ToLower(std::string_view str)
        {
            std::string result;
            result.reserve(str.size());
            for (char c : str)
            {
                result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return result;
        }

        bool ParseBoolValue(std::string_view val, bool defaultIfEmpty = true)
        {
            if (val.empty())
            {
                return defaultIfEmpty;
            }
            std::string lower = ToLower(val);
            if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
            {
                return true;
            }
            if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
            {
                return false;
            }
            return defaultIfEmpty;
        }

        bool IsBoolLiteral(std::string_view val)
        {
            std::string lower = ToLower(val);
            return lower == "true" || lower == "1" || lower == "yes" || lower == "on" ||
                   lower == "false" || lower == "0" || lower == "no" || lower == "off";
        }
    }

    void PrintHelp()
    {
        std::cout << "AngelScript Language Server (AngelLSP) v1.0.0\n"
                  << "Usage: angel_lsp [options]\n\n"
                  << "Feature Flags:\n"
                  << "  --enable-hover[=true|false]             Enable/disable hover tooltips (default: true)\n"
                  << "  --disable-hover                         Disable hover tooltips\n"
                  << "  --enable-definition[=true|false]        Enable/disable Go to Definition / Type Definition (default: true)\n"
                  << "  --disable-definition                    Disable Go to Definition\n"
                  << "  --enable-completion[=true|false]        Enable/disable auto-completion (default: true)\n"
                  << "  --disable-completion                    Disable auto-completion\n"
                  << "  --enable-semantic-tokens[=true|false]   Enable/disable semantic syntax highlighting (default: true)\n"
                  << "  --disable-semantic-tokens               Disable semantic tokens\n"
                  << "  --enable-signature-help[=true|false]    Enable/disable signature help (default: true)\n"
                  << "  --disable-signature-help                Disable signature help\n"
                  << "  --enable-predefined-loader[=true|false] Enable/disable predefined file loader (default: true)\n"
                  << "  --disable-predefined-loader             Disable predefined loader\n\n"
                  << "Options:\n"
                  << "  --locale=<string>                       Set diagnostic language/locale (default: en)\n"
                  << "  --file-ext=<string>                     Set script file extension (default: .as)\n"
                  << "  --predefined-ext=<string>               Set predefined symbols file extension (default: .as.predefined)\n"
                  << "  -h, --help                              Show this help message and exit\n"
                  << "  -v, --version                           Show version information and exit\n";
    }

    void PrintVersion()
    {
        std::cout << "AngelScript Language Server (AngelLSP) version 1.0.0\n";
    }

    ServerConfig FromArgs(int argc, char **argv)
    {
        ServerConfig config;

        if (argc <= 1 || argv == nullptr)
        {
            return config;
        }

        for (int i = 1; i < argc; ++i)
        {
            if (argv[i] == nullptr)
            {
                continue;
            }

            std::string_view arg(argv[i]);
            if (arg.empty())
            {
                continue;
            }

            std::string_view key = arg;
            std::optional<std::string_view> inlineVal;

            size_t eqPos = arg.find('=');
            if (eqPos != std::string_view::npos)
            {
                key = arg.substr(0, eqPos);
                inlineVal = arg.substr(eqPos + 1);
            }

            auto getStringValue = [&](std::string_view &out) -> bool
            {
                if (inlineVal.has_value())
                {
                    out = *inlineVal;
                    return true;
                }
                if (i + 1 < argc && argv[i + 1] != nullptr)
                {
                    out = argv[++i];
                    return true;
                }
                return false;
            };

            auto getBoolValue = [&](bool defaultVal = true) -> bool
            {
                if (inlineVal.has_value())
                {
                    return ParseBoolValue(*inlineVal, defaultVal);
                }
                if (i + 1 < argc && argv[i + 1] != nullptr)
                {
                    std::string_view nextArg(argv[i + 1]);
                    if (IsBoolLiteral(nextArg))
                    {
                        ++i;
                        return ParseBoolValue(nextArg, defaultVal);
                    }
                }
                return defaultVal;
            };

            if (key == "--help" || key == "-h")
            {
                config.info.showHelp = true;
                PrintHelp();
            }
            else if (key == "--version" || key == "-v")
            {
                config.info.showVersion = true;
                PrintVersion();
            }
            else if (key == "--enable-hover")
            {
                config.features.enableHover = getBoolValue(true);
            }
            else if (key == "--disable-hover")
            {
                config.features.enableHover = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-definition")
            {
                config.features.enableDefinition = getBoolValue(true);
            }
            else if (key == "--disable-definition")
            {
                config.features.enableDefinition = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-completion")
            {
                config.features.enableCompletion = getBoolValue(true);
            }
            else if (key == "--disable-completion")
            {
                config.features.enableCompletion = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-semantic-tokens" || key == "--enable-semantictokens")
            {
                config.features.enableSemanticTokens = getBoolValue(true);
            }
            else if (key == "--disable-semantic-tokens" || key == "--disable-semantictokens")
            {
                config.features.enableSemanticTokens = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-signature-help" || key == "--enable-signaturehelp")
            {
                config.features.enableSignatureHelp = getBoolValue(true);
            }
            else if (key == "--disable-signature-help" || key == "--disable-signaturehelp")
            {
                config.features.enableSignatureHelp = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-predefined-loader" || key == "--enable-predefinedloader")
            {
                config.features.enablePredefinedLoader = getBoolValue(true);
            }
            else if (key == "--disable-predefined-loader" || key == "--disable-predefinedloader")
            {
                config.features.enablePredefinedLoader = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--locale")
            {
                std::string_view val;
                if (getStringValue(val))
                {
                    config.info.locale = std::string(val);
                }
            }
            else if (key == "--file-ext" || key == "--file-extension")
            {
                std::string_view val;
                if (getStringValue(val))
                {
                    config.info.fileExtension = std::string(val);
                }
            }
            else if (key == "--predefined-ext" || key == "--predefined-extension")
            {
                std::string_view val;
                if (getStringValue(val))
                {
                    config.info.predefinedFileExtension = std::string(val);
                }
            }
        }

        return config;
    }
}