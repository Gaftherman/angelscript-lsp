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

        /** @brief Applies one `--engine-property=<name>=<value>` pair.
         *  @return False when the name is not one this server reads, or the value does not suit it,
         *          so the caller can drop the pair.
         *  @note Names are the asEEngineProp identifiers without the asEP_ prefix, in lowerCamel -
         *        allowUnsafeReferences for asEP_ALLOW_UNSAFE_REFERENCES. Matching the engine's own
         *        vocabulary means a host author can map its SetEngineProperty calls across without
         *        translating anything, and it leaves room for the other thirty-odd properties to
         *        arrive here the day a rule needs one.
         *  @note The value arrives as text rather than a bool because not every engine property is
         *        one. Most are, and read true/false; asEP_PROPERTY_ACCESSOR_MODE is an integer, and
         *        flattening it to a bool would have made this option lie about the engine's own
         *        vocabulary in exactly the place that vocabulary is the point. */
        /**
         * @brief Applies one `--preprocessor-feature=<name>=<value>` pair.
         *
         * Deliberately not folded into ApplyEngineProperty: an engine property is an asEP_* value
         * the SDK interprets, while these describe a source file the host may have patched. Sharing
         * the name space would suggest the SDK knows about `#else`, and it does not.
         *
         * @return False for an unknown name or an unparsable value, so a typo is not silently on.
         */
        bool ApplyPreprocessorFeature(ServerConfig &config, std::string_view name, std::string_view raw)
        {
            if (name == "pragmaMode")
            {
                if (raw == "accept") { config.pragmaMode = ServerConfig::PragmaMode::Accept; return true; }
                if (raw == "hint")   { config.pragmaMode = ServerConfig::PragmaMode::Hint;   return true; }
                if (raw == "error")  { config.pragmaMode = ServerConfig::PragmaMode::Error;  return true; }
                return false;
            }

            if (!IsBoolLiteral(raw))
            {
                return false;
            }
            const bool value = ParseBoolValue(raw, true);

            if (name == "elseSupport")     { config.preprocessor.elseSupport = value;     return true; }
            if (name == "elifSupport")     { config.preprocessor.elifSupport = value;     return true; }
            if (name == "ifdefSupport")    { config.preprocessor.ifdefSupport = value;    return true; }
            if (name == "defineInScripts") { config.preprocessor.defineInScripts = value; return true; }

            return false;
        }

        bool ApplyEngineProperty(EngineProperties &engine, std::string_view name, std::string_view raw)
        {
            if (name == "propertyAccessorMode")
            {
                // 0 disabled, 1 app-registered only, 2 app and script, 3 (the engine's default)
                // app and script but only where the `property` keyword is present. Only 2 and 3
                // differ for a script analyzer, so anything else is dropped rather than guessed at.
                if (raw == "2" || raw == "3")
                {
                    engine.propertyAccessorMode = raw == "2" ? 2 : 3;
                    return true;
                }
                return false;
            }
            if (name == "boolConversionMode")
            {
                if (raw == "0" || raw == "1")
                {
                    engine.boolConversionMode = raw == "1" ? 1 : 0;
                    return true;
                }
                return false;
            }

            if (name == "allowMultilineStrings")
            {
                if (raw == "0" || raw == "1")
                {
                    engine.allowMultilineStrings = raw == "1";
                    return true;
                }
                return false;
            }

            if (name == "useCharacterLiterals")
            {
                if (raw == "0" || raw == "1")
                {
                    engine.useCharacterLiterals = raw == "1" ? 1 : 0;
                    return true;
                }
                return false;
            }

            if (name == "disallowValueAssignForRef")
            {
                if (raw == "0" || raw == "1")
                {
                    engine.disallowValueAssignForRef = raw == "1";
                    return true;
                }
                return false;
            }

            if (name == "alterSyntaxNamedArgs")
            {
                if (raw == "0" || raw == "1" || raw == "2")
                {
                    engine.alterSyntaxNamedArgs = raw == "2" ? 2 : (raw == "1" ? 1 : 0);
                    return true;
                }
                return false;
            }

            if (name == "disableIntegerDivision")
            {
                if (raw == "0" || raw == "1")
                {
                    engine.disableIntegerDivision = raw == "1";
                    return true;
                }
                return false;
            }

            if (name == "disallowEmptyListElements")
            {
                if (raw == "0" || raw == "1")
                {
                    engine.disallowEmptyListElements = raw == "1";
                    return true;
                }
                return false;
            }

            if (name == "foreachSupport")
            {
                if (raw == "0" || raw == "1")
                {
                    engine.foreachSupport = raw == "1";
                    return true;
                }
                return false;
            }

            if (!IsBoolLiteral(raw))
            {
                return false;
            }
            const bool value = ParseBoolValue(raw, true);

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
                  << "  --disable-pull-diagnostics              Disable LSP 3.17 pull diagnostics\n"
                  << "  --enable-formatting[=true|false]        Enable/disable formatting (default: true)\n"
                  << "  --disable-formatting                    Disable formatting\n"
                  << "  --enable-document-link[=true|false]     Enable/disable #include links (default: true)\n"
                  << "  --disable-document-link                 Disable #include links\n"
                  << "  --enable-type-conversion-checks[=true|false] Enable/disable type conversion diagnostics (default: true)\n"
                  << "  --disable-type-conversion-checks        Disable type conversion diagnostics\n"
                  << "  --enable-implementation[=true|false]    Enable/disable Go to Implementation (default: true)\n"
                  << "  --disable-implementation                Disable Go to Implementation\n"
                  << "  --enable-selection-range[=true|false]   Enable/disable expand selection (default: true)\n"
                  << "  --disable-selection-range               Disable expand selection\n"
                  << "  --enable-call-hierarchy[=true|false]    Enable/disable call hierarchy (default: true)\n"
                  << "  --disable-call-hierarchy                Disable call hierarchy\n"
                  << "  --enable-type-hierarchy[=true|false]    Enable/disable type hierarchy (default: true)\n"
                  << "  --disable-type-hierarchy                Disable type hierarchy\n"
                  << "  --enable-linked-editing[=true|false]    Enable/disable linked editing of locals (default: true)\n"
                  << "  --disable-linked-editing                Disable linked editing\n\n"
                  << "Options:\n"
                  << "  --locale=<string>                       Set diagnostic language/locale (default: en)\n"
                  << "  -D, --define=<word>                     Treat <word> as defined for #if (repeatable).\n"
                  << "                                          Mirrors CScriptBuilder::DefineWord. With none set,\n"
                  << "                                          every #if block is excluded, as the builder does.\n"
                  << "  --log-level=<level>                     error|warning|info|debug (default: info).\n"
                  << "                                          debug logs every symbol on every analysis and\n"
                  << "                                          costs real throughput; it is not free.\n"
                  << "  --file-ext=<string>                     Set script file extension (default: .as)\n"
                  << "  --predefined-ext=<string>               Set predefined symbols file extension (default: .as.predefined)\n"
                  << "  --predefined-file=<path>                Load a predefined stub by path, even outside the workspace (repeatable)\n"
                  << "  --exclude=<glob>                        Directory glob the workspace scans do not descend\n"
                  << "                                          into (repeatable). ?, * within a segment and **\n"
                  << "                                          across segments. The first one given replaces the\n"
                  << "                                          defaults: .git, build and node_modules.\n"
                  << "  --search-dir=<string>                   Add directory to search path for #include resolution\n"
                  << "  --array-like-type=<name>                Name a template whose initializer list is a plain\n"
                  << "                                          repeat of its element type, as array<T>'s is\n"
                  << "                                          (repeatable). A list factory is registered in C++\n"
                  << "                                          and no predefined stub can express it, so a host\n"
                  << "                                          that registers its own has to say so here.\n"
                  << "  --preprocessor-feature=<name>=<value>   Preprocessor extensions the host added to its own copy\n"
                  << "                                          of CScriptBuilder. None exist in the stock add-on, so\n"
                  << "                                          all default off and the defaults match it exactly:\n"
                  << "                                            elseSupport, elifSupport, ifdefSupport,\n"
                  << "                                            defineInScripts   (booleans)\n"
                  << "                                            pragmaMode=accept|hint|error (default: accept)\n"
                  << "  --diagnostic-severity=<code>=<severity> Override one diagnostic's severity: error|warning|information|hint (repeatable)\n"
                  << "  --no-report-unknown-types               Stop reporting a parameter or return type that resolves\n"
                  << "                                          to nothing. On by default: an unreported one surfaces as\n"
                  << "                                          silence at every call site, since a call whose parameter\n"
                  << "                                          types are unknown cannot be judged either. Turn it off for\n"
                  << "                                          a host that registers types in C++ and declares none of\n"
                  << "                                          them - or better, name its --engine-profile.\n"
                  << "  --report-accessor-portability           Hint on accessors without the 'property' keyword\n"
                  << "  --report-bool-conversion                Hint on a class used where a bool is expected\n"
                  << "  --report-missing-funcdef                Hint when a type position names a function\n"
                  << "  --report-integer-division               Hint on integer division expressions\n"
                  << "  --report-named-argument-syntax          Hint on named argument syntax\n"
                  << "  --report-empty-list-elements            Hint on empty elements in initialization lists\n"
                  << "  --report-value-assign-for-ref           Hint on value assignment for reference types\n"
                  << "  --format-brace-style=<allman|kr>        Where a block's opening brace goes. Default allman.\n"
                  << "                                          A list or a lambda body keeps its brace on the line\n"
                  << "                                          either way - that is correctness, not style.\n"
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

        // The first --exclude replaces the built-in defaults; see the handler below.
        bool sawExclude = false;

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
            else if (key == "--enable-pull-diagnostics" || key == "--enable-pulldiagnostics")
            {
                config.features.enablePullDiagnostics = getBoolValue(true);
            }
            else if (key == "--disable-pull-diagnostics" || key == "--disable-pulldiagnostics")
            {
                config.features.enablePullDiagnostics = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
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
            else if (key == "--enable-implementation")
            {
                config.features.enableImplementation = getBoolValue(true);
            }
            else if (key == "--disable-implementation")
            {
                config.features.enableImplementation = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-selection-range" || key == "--enable-selectionrange")
            {
                config.features.enableSelectionRange = getBoolValue(true);
            }
            else if (key == "--disable-selection-range" || key == "--disable-selectionrange")
            {
                config.features.enableSelectionRange = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-call-hierarchy" || key == "--enable-callhierarchy")
            {
                config.features.enableCallHierarchy = getBoolValue(true);
            }
            else if (key == "--disable-call-hierarchy" || key == "--disable-callhierarchy")
            {
                config.features.enableCallHierarchy = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-type-hierarchy" || key == "--enable-typehierarchy")
            {
                config.features.enableTypeHierarchy = getBoolValue(true);
            }
            else if (key == "--disable-type-hierarchy" || key == "--disable-typehierarchy")
            {
                config.features.enableTypeHierarchy = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-linked-editing" || key == "--enable-linkedediting")
            {
                config.features.enableLinkedEditing = getBoolValue(true);
            }
            else if (key == "--disable-linked-editing" || key == "--disable-linkedediting")
            {
                config.features.enableLinkedEditing = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-code-lens" || key == "--enable-codelens")
            {
                config.features.enableCodeLens = getBoolValue(true);
            }
            else if (key == "--disable-code-lens" || key == "--disable-codelens")
            {
                config.features.enableCodeLens = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-formatting")
            {
                config.features.enableFormatting = getBoolValue(true);
            }
            else if (key == "--disable-formatting")
            {
                config.features.enableFormatting = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--enable-on-type-formatting" || key == "--enable-ontypeformatting")
            {
                config.features.enableOnTypeFormatting = getBoolValue(true);
            }
            else if (key == "--disable-on-type-formatting" || key == "--disable-ontypeformatting")
            {
                config.features.enableOnTypeFormatting = inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--locale")
            {
                std::string_view val;
                if (getStringValue(val))
                {
                    config.info.locale = std::string(val);
                }
            }
            else if (key == "--define" || key == "-D")
            {
                std::string_view val;
                if (getStringValue(val) && !val.empty())
                {
                    config.definedWords.push_back(std::string(val));
                }
            }
            else if (key == "--log-level")
            {
                std::string_view val;
                if (getStringValue(val) && !val.empty())
                {
                    config.info.logLevel = ToLower(val);
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
            else if (key == "--exclude")
            {
                std::string_view val;
                if (getStringValue(val))
                {
                    // The first --exclude replaces the defaults rather than adding to them: a user
                    // who names their own list means that list, and appending would leave them
                    // unable to scan a directory called `build` at all.
                    if (!sawExclude)
                    {
                        config.exclude.clear();
                        sawExclude = true;
                    }
                    config.exclude.push_back(std::string(val));
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
            else if (key == "--array-like-type" || key == "--array-like-template")
            {
                std::string_view val;
                if (getStringValue(val) && !val.empty())
                {
                    config.types.arrayLikeTemplates.insert(std::string(val));
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
            else if (key == "--format-brace-style" || key == "--brace-style")
            {
                std::string_view val;
                if (getStringValue(val) && !val.empty())
                {
                    config.format.braceStyle = std::string(val);
                }
            }
            else if (key == "--format-on-save")
            {
                config.format.formatOnSave = getBoolValue(true);
            }
            else if (key == "--engine-profile" || key == "--profile")
            {
                std::string_view val;
                if (getStringValue(val) && !val.empty())
                {
                    config.engineProfile = std::string(val);
                }
            }
            else if (key == "--report-unknown-types")
            {
                config.diagnostics.reportUnknownTypes = getBoolValue(true);
            }
            else if (key == "--no-report-unknown-types")
            {
                config.diagnostics.reportUnknownTypes =
                    inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--report-accessor-portability")
            {
                config.diagnostics.reportAccessorPortability = getBoolValue(true);
            }
            else if (key == "--no-report-accessor-portability")
            {
                config.diagnostics.reportAccessorPortability =
                    inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--report-bool-conversion")
            {
                config.diagnostics.reportBoolConversion = getBoolValue(true);
            }
            else if (key == "--no-report-bool-conversion")
            {
                config.diagnostics.reportBoolConversion =
                    inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--report-missing-funcdef")
            {
                config.diagnostics.reportMissingFuncdef = getBoolValue(true);
            }
            else if (key == "--no-report-missing-funcdef")
            {
                config.diagnostics.reportMissingFuncdef =
                    inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
            }
            else if (key == "--report-integer-division")
            {
                config.diagnostics.reportIntegerDivision = getBoolValue(true);
            }
            else if (key == "--no-report-integer-division")
            {
                config.diagnostics.reportIntegerDivision =
                    inlineVal.has_value() ? !ParseBoolValue(*inlineVal, true) : false;
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
                    if (!name.empty())
                    {
                        ApplyEngineProperty(config.engine, name, raw);
                    }
                }
            }
            else if (key == "--preprocessor-feature" || key == "--preproc")
            {
                // Same "<name>=<value>" shape as --engine-property, and for the same reason: the
                // outer split consumed only the first '=', so the pair arrives intact.
                std::string_view val;
                if (getStringValue(val))
                {
                    const size_t sep = val.find('=');
                    const std::string_view name = val.substr(0, sep == std::string_view::npos ? val.size() : sep);
                    const std::string_view raw = sep == std::string_view::npos
                                                 ? std::string_view("true") : val.substr(sep + 1);
                    if (!name.empty())
                    {
                        ApplyPreprocessorFeature(config, name, raw);
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
