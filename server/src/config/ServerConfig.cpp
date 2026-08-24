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

        /** @brief True for the four LSP severity names a diagnostic can be remapped to.
         *  @note Validated here rather than at use: an unrecognised name silently dropped at
         *        startup is far easier to diagnose than one that reaches the analyzer and picks a
         *        severity by accident. */
        bool IsDiagnosticSeverityName(std::string_view val)
        {
            return val == "error" || val == "warning" || val == "information" || val == "hint";
        }

        /** @brief Applies one `--engine-property=<name>=<bool>` pair.
         *  @return False when the name is not one this server reads, so the caller can drop it.
         *  @note Names are the asEEngineProp identifiers without the asEP_ prefix, in lowerCamel -
         *        allowUnsafeReferences for asEP_ALLOW_UNSAFE_REFERENCES. Matching the engine's own
         *        vocabulary means a host author can map its SetEngineProperty calls across without
         *        translating anything, and it leaves room for the other thirty-odd properties to
         *        arrive here the day a rule needs one. */
        bool ApplyEngineProperty(EngineProperties &engine, std::string_view name, bool value)
        {
            if (name == "allowUnsafeReferences")
            {
                engine.allowUnsafeReferences = value;
                return true;
            }
            if (name == "privatePropAsProtected")
            {
                engine.privatePropAsProtected = value;
                return true;
            }
            if (name == "disallowGlobalVars")
            {
                engine.disallowGlobalVars = value;
                return true;
            }
            return false;
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
                  << "  --disable-predefined-loader             Disable predefined loader\n"
                  << "  --enable-document-symbols[=true|false]  Enable/disable document symbols outline (default: true)\n"
                  << "  --disable-document-symbols              Disable document symbols outline\n"
                  << "  --enable-workspace-symbols[=true|false] Enable/disable workspace symbol search (default: true)\n"
                  << "  --disable-workspace-symbols             Disable workspace symbol search\n"
                  << "  --enable-references[=true|false]        Enable/disable find references (default: true)\n"
                  << "  --disable-references                    Disable find references\n"
                  << "  --enable-rename[=true|false]            Enable/disable symbol rename (default: true)\n"
                  << "  --disable-rename                        Disable symbol rename\n"
                  << "  --enable-document-highlight[=true|false] Enable/disable document highlight (default: true)\n"
                  << "  --disable-document-highlight            Disable document highlight\n"
                  << "  --enable-folding-range[=true|false]     Enable/disable folding ranges (default: true)\n"
                  << "  --disable-folding-range                 Disable folding ranges\n"
                  << "  --enable-inlay-hints[=true|false]       Enable/disable inlay hints (default: true)\n"
                  << "  --disable-inlay-hints                   Disable inlay hints\n"
                  << "  --enable-code-action[=true|false]       Enable/disable code actions (default: true)\n"
                  << "  --disable-code-action                   Disable code actions\n"
                  << "  --enable-formatting[=true|false]        Enable/disable formatting (default: true)\n"
                  << "  --disable-formatting                    Disable formatting\n"
                  << "  --enable-document-link[=true|false]     Enable/disable #include links (default: true)\n"
                  << "  --disable-document-link                 Disable #include links\n"
                  << "  --enable-type-conversion-checks[=true|false] Enable/disable type conversion diagnostics (default: true)\n"
                  << "  --disable-type-conversion-checks        Disable type conversion diagnostics\n\n"
                  << "Options:\n"
                  << "  --locale=<string>                       Set diagnostic language/locale (default: en)\n"
                  << "  --file-ext=<string>                     Set script file extension (default: .as)\n"
                  << "  --predefined-ext=<string>               Set predefined symbols file extension (default: .as.predefined)\n"
                  << "  --predefined-file=<path>                Load a predefined stub by path, even outside the workspace (repeatable)\n"
                  << "  --search-dir=<string>                   Add directory to search path for #include resolution\n"
                  << "  --diagnostic-severity=<code>=<severity> Override one diagnostic's severity: error|warning|information|hint (repeatable)\n"
                  << "  --engine-property=<name>=<bool>         Describe the host engine's SetEngineProperty settings (repeatable).\n"
                  << "                                          Known names, with the engine's own defaults:\n"
                  << "                                            allowUnsafeReferences  (false) permit & on primitives and standalone references\n"
                  << "                                            privatePropAsProtected (false) let a derived class reach a private member\n"
                  << "                                            disallowGlobalVars     (false) reject global variable declarations\n"
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
            else if (key == "--enable-document-symbols" || key == "--enable-documentsymbols")
            {
                config.features.enableDocumentSymbols = getBoolValue(true);
            }
            else if (key == "--disable-document-symbols" || key == "--disable-documentsymbols")
            {
                config.features.enableDocumentSymbols = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-workspace-symbols" || key == "--enable-workspacesymbols")
            {
                config.features.enableWorkspaceSymbols = getBoolValue(true);
            }
            else if (key == "--disable-workspace-symbols" || key == "--disable-workspacesymbols")
            {
                config.features.enableWorkspaceSymbols = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-references")
            {
                config.features.enableReferences = getBoolValue(true);
            }
            else if (key == "--disable-references")
            {
                config.features.enableReferences = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-rename")
            {
                config.features.enableRename = getBoolValue(true);
            }
            else if (key == "--disable-rename")
            {
                config.features.enableRename = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-document-highlight" || key == "--enable-documenthighlight")
            {
                config.features.enableDocumentHighlight = getBoolValue(true);
            }
            else if (key == "--disable-document-highlight" || key == "--disable-documenthighlight")
            {
                config.features.enableDocumentHighlight = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-folding-range" || key == "--enable-foldingrange")
            {
                config.features.enableFoldingRange = getBoolValue(true);
            }
            else if (key == "--disable-folding-range" || key == "--disable-foldingrange")
            {
                config.features.enableFoldingRange = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-inlay-hints" || key == "--enable-inlayhints")
            {
                config.features.enableInlayHints = getBoolValue(true);
            }
            else if (key == "--disable-inlay-hints" || key == "--disable-inlayhints")
            {
                config.features.enableInlayHints = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-code-action" || key == "--enable-codeaction")
            {
                config.features.enableCodeAction = getBoolValue(true);
            }
            else if (key == "--disable-code-action" || key == "--disable-codeaction")
            {
                config.features.enableCodeAction = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-document-link" || key == "--enable-documentlink")
            {
                config.features.enableDocumentLink = getBoolValue(true);
            }
            else if (key == "--disable-document-link" || key == "--disable-documentlink")
            {
                config.features.enableDocumentLink = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-type-conversion-checks" || key == "--enable-typeconversionchecks")
            {
                config.features.enableTypeConversionChecks = getBoolValue(true);
            }
            else if (key == "--disable-type-conversion-checks" || key == "--disable-typeconversionchecks")
            {
                config.features.enableTypeConversionChecks = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-formatting")
            {
                config.features.enableFormatting = getBoolValue(true);
            }
            else if (key == "--disable-formatting")
            {
                config.features.enableFormatting = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
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
            else if (key == "--search-dir" || key == "--search-directory" || key == "--search-path")
            {
                std::string_view val;
                if (getStringValue(val))
                {
                    config.searchDirectories.push_back(std::string(val));
                }
            }
            else if (key == "--predefined-file" || key == "--predefined-path")
            {
                std::string_view val;
                if (getStringValue(val) && !val.empty())
                {
                    config.predefinedFiles.push_back(std::string(val));
                }
            }
            else if (key == "--engine-property" || key == "--engine-prop")
            {
                // Value is "<name>=<bool>", and like --diagnostic-severity the outer split above
                // consumed only the first '=', so the pair arrives intact.
                std::string_view val;
                if (getStringValue(val))
                {
                    const size_t sep = val.find('=');
                    const std::string_view name = val.substr(0, sep == std::string_view::npos ? val.size() : sep);
                    // A bare name means "on", the way --enable-x does.
                    const std::string_view raw = sep == std::string_view::npos
                                                 ? std::string_view("true") : val.substr(sep + 1);
                    if (!name.empty() && IsBoolLiteral(raw))
                    {
                        ApplyEngineProperty(config.engine, name, ParseBoolValue(raw, true));
                    }
                }
            }
            else if (key == "--diagnostic-severity" || key == "--severity")
            {
                // Value is "<code>=<severity>". The outer split above consumed only the first '=',
                // so the pair arrives intact here.
                std::string_view val;
                if (getStringValue(val))
                {
                    const size_t sep = val.find('=');
                    if (sep != std::string_view::npos && sep > 0 && sep + 1 < val.size())
                    {
                        std::string code(val.substr(0, sep));
                        std::string severity = ToLower(val.substr(sep + 1));
                        if (IsDiagnosticSeverityName(severity))
                        {
                            config.diagnosticSeverities[std::move(code)] = std::move(severity);
                        }
                    }
                }
            }
        }

        return config;
    }
}
