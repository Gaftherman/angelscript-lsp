#include "config/ServerConfig.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on" || lower == "false" || lower == "0" ||
           lower == "no" || lower == "off";
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
bool ApplyPreprocessorFeature(ServerConfig& config, std::string_view name, std::string_view raw)
{
    if (name == "pragmaMode")
    {
        if (raw == "accept")
        {
            config.pragmaMode = ServerConfig::PragmaMode::Accept;
            return true;
        }
        if (raw == "hint")
        {
            config.pragmaMode = ServerConfig::PragmaMode::Hint;
            return true;
        }
        if (raw == "error")
        {
            config.pragmaMode = ServerConfig::PragmaMode::Error;
            return true;
        }
        return false;
    }

    if (!IsBoolLiteral(raw))
    {
        return false;
    }
    const bool value = ParseBoolValue(raw, true);

    if (name == "elseSupport")
    {
        config.preprocessor.elseSupport = value;
        return true;
    }
    if (name == "elifSupport")
    {
        config.preprocessor.elifSupport = value;
        return true;
    }
    if (name == "ifdefSupport")
    {
        config.preprocessor.ifdefSupport = value;
        return true;
    }
    if (name == "defineInScripts")
    {
        config.preprocessor.defineInScripts = value;
        return true;
    }

    return false;
}

bool ParseDigitInRange(std::string_view raw, char minDigit, char maxDigit, int& out)
{
    if (raw.size() == 1 && raw[0] >= minDigit && raw[0] <= maxDigit)
    {
        out = raw[0] - '0';
        return true;
    }
    return false;
}

bool ApplyNumericEngineProperty(EngineProperties& engine, std::string_view name, std::string_view raw)
{
    if (name == "propertyAccessorMode")
    {
        return ParseDigitInRange(raw, '0', '3', engine.propertyAccessorMode);
    }
    if (name == "boolConversionMode")
    {
        return ParseDigitInRange(raw, '0', '1', engine.boolConversionMode);
    }
    if (name == "useCharacterLiterals")
    {
        return ParseDigitInRange(raw, '0', '1', engine.useCharacterLiterals);
    }
    if (name == "alterSyntaxNamedArgs")
    {
        return ParseDigitInRange(raw, '0', '2', engine.alterSyntaxNamedArgs);
    }
    if (name == "compilerWarnings" || name == "compiler-warnings")
    {
        return ParseDigitInRange(raw, '0', '2', engine.compilerWarnings);
    }
    return false;
}

struct ZeroOneBoolProp
{
    std::string_view name;
    std::string_view alias;
    bool EngineProperties::* member;
};

static constexpr ZeroOneBoolProp kZeroOneBoolProps[] = {
    {"allowMultilineStrings", "", &EngineProperties::allowMultilineStrings},
    {"disallowValueAssignForRef", "", &EngineProperties::disallowValueAssignForRef},
    {"disableIntegerDivision", "", &EngineProperties::disableIntegerDivision},
    {"disallowEmptyListElements", "", &EngineProperties::disallowEmptyListElements},
    {"foreachSupport", "", &EngineProperties::foreachSupport},
    {"requireEnumScope", "require-enum-scope", &EngineProperties::requireEnumScope},
    {"alwaysImplDefaultConstruct", "always-impl-default-construct", &EngineProperties::alwaysImplDefaultConstruct},
    {"allowUnicodeIdentifiers", "allow-unicode-identifiers", &EngineProperties::allowUnicodeIdentifiers},
    {"ignoreDuplicateSharedIntf", "ignore-duplicate-shared-intf", &EngineProperties::ignoreDuplicateSharedIntf},
};

bool ApplyZeroOneBoolEngineProperty(EngineProperties& engine, std::string_view name, std::string_view raw)
{
    for (const auto& prop : kZeroOneBoolProps)
    {
        if (name == prop.name || (!prop.alias.empty() && name == prop.alias))
        {
            if (raw == "0" || raw == "1")
            {
                engine.*(prop.member) = (raw == "1");
                return true;
            }
            return false;
        }
    }
    return false;
}

bool ApplyBooleanLiteralEngineProperty(EngineProperties& engine, std::string_view name, std::string_view raw)
{
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

/**
 * @brief Applies one `--engine-property=<name>=<value>` pair.
 * @param[in,out] engine Engine properties to update.
 * @param[in] name Property identifier in lowerCamel or kebab-case.
 * @param[in] raw Text value to parse and assign.
 * @return False when the name is unrecognised or value is invalid.
 */
bool ApplyEngineProperty(EngineProperties& engine, std::string_view name, std::string_view raw)
{
    if (ApplyNumericEngineProperty(engine, name, raw))
    {
        return true;
    }
    if (ApplyZeroOneBoolEngineProperty(engine, name, raw))
    {
        return true;
    }
    return ApplyBooleanLiteralEngineProperty(engine, name, raw);
}
} // namespace

void PrintFeatureFlagsHelp()
{
    std::cout
        << "AngelScript Language Server (AngelLSP) v" ANGELLSP_VERSION "\n"
        << "Usage: angel_lsp [options]\n\n"
        << "Feature Flags:\n"
        << "  --enable-hover[=true|false]             Enable/disable hover tooltips (default: true)\n"
        << "  --disable-hover                         Disable hover tooltips\n"
        << "  --enable-definition[=true|false]        Enable/disable Go to Definition / Type Definition (default: "
           "true)\n"
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
        << "  --inlay-hints-suppress-when-argument-matches-name[=true|false] Suppress inlay hints when arg matches "
           "param name (default: false)\n"
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
        << "  --disable-linked-editing                Disable linked editing\n"
        << "  --enable-virtual-mixin-documents[=true|false] Enable/disable virtual mixin documents (default: false)\n"
        << "  --disable-virtual-mixin-documents       Disable virtual mixin documents\n\n";
}

void PrintOptionsHelp()
{
    std::cout
        << "Options:\n"
        << "  --locale=<string>                       Set diagnostic language/locale (default: en)\n"
        << "  -D, --define=<word>                     Treat <word> as defined for #if (repeatable).\n"
        << "                                          Mirrors CScriptBuilder::DefineWord. With none set,\n"
        << "                                          every #if block is excluded, as the builder does.\n"
        << "  --log-level=<level>                     error|warn|info|debug|trace (default: info).\n"
        << "                                          debug logs every symbol on every analysis and\n"
        << "                                          costs real throughput; it is not free.\n"
        << "  --file-ext=<string>                     Set script file extension (default: .as)\n"
        << "  --predefined-ext=<string>               Set predefined symbols file extension (default: .as.predefined)\n"
        << "  --predefined-file=<path>                Load a predefined stub by path, even outside the workspace "
           "(repeatable)\n"
        << "  --predefined-active=<path>              Select the single predefined stub workspace scan will\n"
        << "                                          load (leaving it empty loads all discovered stubs)\n"
        << "  --module=<name>=<path>                  Name one script module and the .as it is built from\n"
        << "  --module-folder=<name>=<dir>            Name one script module and a directory whose scripts\n"
        << "                                          all belong to it (repeatable). The deepest folder wins.\n"
        << "                                          (repeatable). Lets external shared be checked.\n"
        << "  --implicit-include-extension=<bool>     Let #include \"helper\" mean helper.as, for hosts that\n"
        << "                                          resolve the name themselves (e.g. Sven Co-op).\n"
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
        << "  --diagnostic-severity=<code>=<severity> Override one diagnostic's severity: "
           "error|warning|information|hint (repeatable)\n"
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

void PrintHelp()
{
    PrintFeatureFlagsHelp();
    PrintOptionsHelp();
}

void PrintVersion()
{
    std::cout << "AngelScript Language Server (AngelLSP) version " ANGELLSP_VERSION "\n";
}

namespace
{
struct ArgParseContext
{
    int& i;
    int argc;
    char** argv;
    std::string_view key;
    std::optional<std::string_view> inlineVal;

    bool GetStringValue(std::string_view& out)
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
    }

    bool GetBoolValue(bool defaultVal = true)
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
    }
};

bool TryParseHelpOrVersion(ServerConfig& config, const ArgParseContext& ctx)
{
    if (ctx.key == "--help" || ctx.key == "-h")
    {
        config.info.showHelp = true;
        PrintHelp();
        return true;
    }
    if (ctx.key == "--version" || ctx.key == "-v")
    {
        config.info.showVersion = true;
        PrintVersion();
        return true;
    }
    return false;
}

struct FeatureFlagMapping
{
    std::string_view enableKey;
    std::string_view enableAlias;
    std::string_view disableKey;
    std::string_view disableAlias;
    bool FeatureFlags::* member;
};

static constexpr FeatureFlagMapping kFeatureFlags[] = {
    {"--enable-hover", "", "--disable-hover", "", &FeatureFlags::enableHover},
    {"--enable-definition", "", "--disable-definition", "", &FeatureFlags::enableDefinition},
    {"--enable-completion", "", "--disable-completion", "", &FeatureFlags::enableCompletion},
    {"--enable-semantic-tokens", "--enable-semantictokens", "--disable-semantic-tokens", "--disable-semantictokens",
     &FeatureFlags::enableSemanticTokens},
    {"--enable-signature-help", "--enable-signaturehelp", "--disable-signature-help", "--disable-signaturehelp",
     &FeatureFlags::enableSignatureHelp},
    {"--enable-predefined-loader", "--enable-predefinedloader", "--disable-predefined-loader",
     "--disable-predefinedloader", &FeatureFlags::enablePredefinedLoader},
    {"--enable-document-symbols", "--enable-documentsymbols", "--disable-document-symbols", "--disable-documentsymbols",
     &FeatureFlags::enableDocumentSymbols},
    {"--enable-workspace-symbols", "--enable-workspacesymbols", "--disable-workspace-symbols",
     "--disable-workspacesymbols", &FeatureFlags::enableWorkspaceSymbols},
    {"--enable-references", "", "--disable-references", "", &FeatureFlags::enableReferences},
    {"--enable-rename", "", "--disable-rename", "", &FeatureFlags::enableRename},
    {"--enable-document-highlight", "--enable-documenthighlight", "--disable-document-highlight",
     "--disable-documenthighlight", &FeatureFlags::enableDocumentHighlight},
    {"--enable-folding-range", "--enable-foldingrange", "--disable-folding-range", "--disable-foldingrange",
     &FeatureFlags::enableFoldingRange},
    {"--enable-inlay-hints", "--enable-inlayhints", "--disable-inlay-hints", "--disable-inlayhints",
     &FeatureFlags::enableInlayHints},
    {"--enable-code-action", "--enable-codeaction", "--disable-code-action", "--disable-codeaction",
     &FeatureFlags::enableCodeAction},
    {"--enable-pull-diagnostics", "--enable-pulldiagnostics", "--disable-pull-diagnostics", "--disable-pulldiagnostics",
     &FeatureFlags::enablePullDiagnostics},
    {"--enable-document-link", "--enable-documentlink", "--disable-document-link", "--disable-documentlink",
     &FeatureFlags::enableDocumentLink},
    {"--enable-type-conversion-checks", "--enable-typeconversionchecks", "--disable-type-conversion-checks",
     "--disable-typeconversionchecks", &FeatureFlags::enableTypeConversionChecks},
    {"--enable-implementation", "", "--disable-implementation", "", &FeatureFlags::enableImplementation},
    {"--enable-selection-range", "--enable-selectionrange", "--disable-selection-range", "--disable-selectionrange",
     &FeatureFlags::enableSelectionRange},
    {"--enable-call-hierarchy", "--enable-callhierarchy", "--disable-call-hierarchy", "--disable-callhierarchy",
     &FeatureFlags::enableCallHierarchy},
    {"--enable-type-hierarchy", "--enable-typehierarchy", "--disable-type-hierarchy", "--disable-typehierarchy",
     &FeatureFlags::enableTypeHierarchy},
    {"--enable-linked-editing", "--enable-linkedediting", "--disable-linked-editing", "--disable-linkedediting",
     &FeatureFlags::enableLinkedEditing},
    {"--enable-code-lens", "--enable-codelens", "--disable-code-lens", "--disable-codelens",
     &FeatureFlags::enableCodeLens},
    {"--enable-formatting", "", "--disable-formatting", "", &FeatureFlags::enableFormatting},
    {"--enable-on-type-formatting", "--enable-ontypeformatting", "--disable-on-type-formatting",
     "--disable-ontypeformatting", &FeatureFlags::enableOnTypeFormatting},
    {"--enable-virtual-mixin-documents", "--enable-virtualmixindocuments", "--disable-virtual-mixin-documents",
     "--disable-virtualmixindocuments", &FeatureFlags::enableVirtualMixinDocuments},
};

bool TryParseFeatureFlag(ServerConfig& config, ArgParseContext& ctx)
{
    for (const auto& entry : kFeatureFlags)
    {
        if (ctx.key == entry.enableKey || (!entry.enableAlias.empty() && ctx.key == entry.enableAlias))
        {
            config.features.*(entry.member) = ctx.GetBoolValue(true);
            return true;
        }
        if (ctx.key == entry.disableKey || (!entry.disableAlias.empty() && ctx.key == entry.disableAlias))
        {
            config.features.*(entry.member) = ctx.inlineVal.has_value() ? !ParseBoolValue(*ctx.inlineVal, true) : false;
            return true;
        }
    }

    if (ctx.key == "--inlay-hints-suppress-when-argument-matches-name")
    {
        config.features.inlayHintsSuppressWhenArgumentMatchesName = ctx.GetBoolValue(true);
        return true;
    }
    if (ctx.key == "--no-inlay-hints-suppress-when-argument-matches-name" ||
        ctx.key == "--disable-inlay-hints-suppress-when-argument-matches-name")
    {
        config.features.inlayHintsSuppressWhenArgumentMatchesName =
            ctx.inlineVal.has_value() ? !ParseBoolValue(*ctx.inlineVal, true) : false;
        return true;
    }
    return false;
}

struct DiagnosticFlagMapping
{
    std::string_view enableKey;
    std::string_view disableKey;
    bool DiagnosticsConfig::* member;
};

static constexpr DiagnosticFlagMapping kDiagFlags[] = {
    {"--report-unknown-types", "--no-report-unknown-types", &DiagnosticsConfig::reportUnknownTypes},
    {"--report-accessor-portability", "--no-report-accessor-portability",
     &DiagnosticsConfig::reportAccessorPortability},
    {"--report-bool-conversion", "--no-report-bool-conversion", &DiagnosticsConfig::reportBoolConversion},
    {"--report-missing-funcdef", "--no-report-missing-funcdef", &DiagnosticsConfig::reportMissingFuncdef},
    {"--report-integer-division", "--no-report-integer-division", &DiagnosticsConfig::reportIntegerDivision},
};

bool TryParseDiagnosticFlag(ServerConfig& config, ArgParseContext& ctx)
{
    for (const auto& entry : kDiagFlags)
    {
        if (ctx.key == entry.enableKey)
        {
            config.diagnostics.*(entry.member) = ctx.GetBoolValue(true);
            return true;
        }
        if (ctx.key == entry.disableKey)
        {
            config.diagnostics.*(entry.member) =
                ctx.inlineVal.has_value() ? !ParseBoolValue(*ctx.inlineVal, true) : false;
            return true;
        }
    }
    if (ctx.key == "--report-accessor-disabled")
    {
        config.diagnostics.reportAccessorDisabled = ctx.GetBoolValue(true);
        return true;
    }
    return false;
}

bool TryParseModuleOption(ServerConfig& config, ArgParseContext& ctx)
{
    if (ctx.key == "--module")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            const size_t sep = val.find('=');
            if (sep != std::string_view::npos)
            {
                std::string_view name = val.substr(0, sep);
                std::string_view entry = val.substr(sep + 1);
                if (!name.empty() && !entry.empty())
                {
                    config.modules.push_back(
                        ServerConfig::ModuleDefinition{std::string(name), std::string(entry), std::string()});
                }
            }
        }
        return true;
    }
    if (ctx.key == "--module-folder")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            const size_t sep = val.find('=');
            if (sep != std::string_view::npos)
            {
                const std::string_view name = val.substr(0, sep);
                const std::string_view folder = val.substr(sep + 1);
                if (!name.empty() && !folder.empty())
                {
                    ServerConfig::ModuleDefinition definition;
                    definition.name = std::string(name);
                    definition.folder = std::string(folder);
                    config.modules.push_back(std::move(definition));
                }
            }
        }
        return true;
    }
    return false;
}

bool TryParsePredefinedOptions(ServerConfig& config, ArgParseContext& ctx)
{
    if (ctx.key == "--predefined-ext" || ctx.key == "--predefined-extension")
    {
        std::string_view val;
        if (ctx.GetStringValue(val))
        {
            config.info.predefinedFileExtension = std::string(val);
        }
        return true;
    }
    if (ctx.key == "--array-like-type" || ctx.key == "--array-like-template")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            config.types.arrayLikeTemplates.insert(std::string(val));
        }
        return true;
    }
    if (ctx.key == "--predefined-file" || ctx.key == "--predefined-path")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            config.predefinedFiles.push_back(std::string(val));
        }
        return true;
    }
    if (ctx.key == "--predefined-active")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            config.activePredefined = std::string(val);
        }
        return true;
    }
    return false;
}

bool TryParseDirectoryOptions(ServerConfig& config, ArgParseContext& ctx, bool& sawExclude)
{
    if (ctx.key == "--exclude")
    {
        std::string_view val;
        if (ctx.GetStringValue(val))
        {
            if (!sawExclude)
            {
                config.exclude.clear();
                sawExclude = true;
            }
            config.exclude.push_back(std::string(val));
        }
        return true;
    }
    if (ctx.key == "--search-dir" || ctx.key == "--search-directory" || ctx.key == "--search-path")
    {
        std::string_view val;
        if (ctx.GetStringValue(val))
        {
            config.searchDirectories.push_back(std::string(val));
        }
        return true;
    }
    return false;
}

bool TryParsePathAndLocaleOptions(ServerConfig& config, ArgParseContext& ctx)
{
    if (ctx.key == "--locale")
    {
        std::string_view val;
        if (ctx.GetStringValue(val))
        {
            config.info.locale = std::string(val);
        }
        return true;
    }
    if (ctx.key == "--define" || ctx.key == "-D")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            config.definedWords.push_back(std::string(val));
        }
        return true;
    }
    if (ctx.key == "--log-level")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            config.info.logLevel = ToLower(val);
        }
        return true;
    }
    if (ctx.key == "--file-ext" || ctx.key == "--file-extension")
    {
        std::string_view val;
        if (ctx.GetStringValue(val))
        {
            config.info.fileExtension = std::string(val);
        }
        return true;
    }
    return false;
}

bool TryParseToolOptions(ServerConfig& config, ArgParseContext& ctx)
{
    if (ctx.key == "--implicit-include-extension")
    {
        config.implicitIncludeExtension = ctx.GetBoolValue(true);
        return true;
    }
    if (ctx.key == "--format-brace-style" || ctx.key == "--brace-style")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            config.format.braceStyle = std::string(val);
        }
        return true;
    }
    if (ctx.key == "--format-on-save")
    {
        config.format.formatOnSave = ctx.GetBoolValue(true);
        return true;
    }
    if (ctx.key == "--engine-profile" || ctx.key == "--profile")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && !val.empty())
        {
            config.engineProfile = std::string(val);
        }
        return true;
    }
    return false;
}

bool TryParseDiagnosticSeverity(ServerConfig& config, ArgParseContext& ctx)
{
    if (ctx.key != "--diagnostic-severity" && ctx.key != "--severity")
    {
        return false;
    }
    std::string_view val;
    if (ctx.GetStringValue(val))
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
    return true;
}

bool TryParseEngineFlags(ServerConfig& config, ArgParseContext& ctx)
{
    auto apply01 = [&](bool& target)
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && (val == "0" || val == "1"))
        {
            target = (val == "1");
        }
        return true;
    };

    if (ctx.key == "--require-enum-scope")
    {
        return apply01(config.engine.requireEnumScope);
    }
    if (ctx.key == "--always-impl-default-construct")
    {
        return apply01(config.engine.alwaysImplDefaultConstruct);
    }
    if (ctx.key == "--allow-unicode-identifiers")
    {
        return apply01(config.engine.allowUnicodeIdentifiers);
    }
    if (ctx.key == "--ignore-duplicate-shared-intf")
    {
        return apply01(config.engine.ignoreDuplicateSharedIntf);
    }
    if (ctx.key == "--compiler-warnings")
    {
        std::string_view val;
        if (ctx.GetStringValue(val) && (val == "0" || val == "1" || val == "2"))
        {
            config.engine.compilerWarnings = val == "2" ? 2 : (val == "1" ? 1 : 0);
        }
        return true;
    }
    return false;
}

bool TryParseEnginePropertiesAndPreproc(ServerConfig& config, ArgParseContext& ctx)
{
    if (ctx.key == "--engine-property" || ctx.key == "--engine-prop")
    {
        std::string_view val;
        if (ctx.GetStringValue(val))
        {
            const size_t sep = val.find('=');
            const std::string_view name = val.substr(0, sep == std::string_view::npos ? val.size() : sep);
            const std::string_view raw = sep == std::string_view::npos ? std::string_view("true") : val.substr(sep + 1);
            if (!name.empty())
            {
                ApplyEngineProperty(config.engine, name, raw);
            }
        }
        return true;
    }
    if (ctx.key == "--preprocessor-feature" || ctx.key == "--preproc")
    {
        std::string_view val;
        if (ctx.GetStringValue(val))
        {
            const size_t sep = val.find('=');
            const std::string_view name = val.substr(0, sep == std::string_view::npos ? val.size() : sep);
            const std::string_view raw = sep == std::string_view::npos ? std::string_view("true") : val.substr(sep + 1);
            if (!name.empty())
            {
                ApplyPreprocessorFeature(config, name, raw);
            }
        }
        return true;
    }
    return false;
}

bool TryParseArg(ServerConfig& config, ArgParseContext& ctx)
{
    return TryParseHelpOrVersion(config, ctx) || TryParseFeatureFlag(config, ctx) ||
           TryParseDiagnosticFlag(config, ctx) || TryParseModuleOption(config, ctx) ||
           TryParsePredefinedOptions(config, ctx);
}

bool TryParseArgExtended(ServerConfig& config, ArgParseContext& ctx, bool& sawExclude)
{
    return TryParsePathAndLocaleOptions(config, ctx) || TryParseDirectoryOptions(config, ctx, sawExclude) ||
           TryParseToolOptions(config, ctx) || TryParseDiagnosticSeverity(config, ctx) ||
           TryParseEngineFlags(config, ctx) || TryParseEnginePropertiesAndPreproc(config, ctx);
}
} // namespace

ServerConfig FromArgs(int argc, char** argv)
{
    ServerConfig config;

    if (argc <= 1 || argv == nullptr)
    {
        return config;
    }

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

        const size_t eqPos = arg.find('=');
        if (eqPos != std::string_view::npos)
        {
            key = arg.substr(0, eqPos);
            inlineVal = arg.substr(eqPos + 1);
        }

        ArgParseContext ctx{i, argc, argv, key, inlineVal};
        if (TryParseArg(config, ctx) || TryParseArgExtended(config, ctx, sawExclude))
        {
            continue;
        }
    }

    return config;
}
} // namespace angel_lsp::config
