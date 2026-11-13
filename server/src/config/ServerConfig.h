#pragma once

#include "config/EngineRuleConfig.h"
#include "utils/PreprocessorRegions.h"
#include "utils/TransparentHash.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace angel_lsp::config
{
/**
 * @brief Inlay hint display mode for omitted default arguments in function calls.
 */
enum class OmittedDefaultArgumentsMode
{
    Off,          ///< Do not show hints for omitted default arguments.
    NameAndValue, ///< Show parameter name and default value: 'param: defaultValue'.
    Declaration   ///< Show parameter type, name, and default value: 'type param = defaultValue'.
};

/**
 * @brief Feature flags for toggling individual LSP capabilities.
 */
struct FeatureFlags
{
    bool enableHover = true;
    bool enableDefinition = true;
    bool enableCompletion = true;
    bool completionSmartTypeRanking = true;
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
    bool inlayHintsSuppressWhenArgumentMatchesName = true;
    size_t inlayHintsMaxParameters = 0;
    size_t inlayHintsMaxLength = 0;
    OmittedDefaultArgumentsMode inlayHintsOmittedDefaultArguments = OmittedDefaultArgumentsMode::Off;
    bool enableCodeAction = true;
    bool enableFormatting = true;
    bool enableDocumentLink = true;
    bool enableTypeConversionChecks = true;
    bool enableImplementation = true;
    bool enableSelectionRange = true;
    bool enableCallHierarchy = true;
    bool enableTypeHierarchy = true;
    bool enableLinkedEditing = true;
    bool enableCodeLens = true;
    bool codeLensShowOnFunctions = true;
    bool codeLensShowOnClassesAndInterfaces = true;
    bool codeLensShowOnPropertiesAndVariables = true;
    bool codeLensShowOnEnums = true;
    bool codeLensShowZeroReferences = true;
    bool enableOnTypeFormatting = false;
    bool enablePullDiagnostics = false;
    bool inlayHintsEnableTooltip = true;
    bool inlayHintsEnableLocation = true;
    bool inlayHintsParameters = true;
    bool inlayHintsVariableTypes = true;
    bool inlayHintsFunctionReturnTypes = true;
    bool inlayHintsEnumMemberValues = true;
    bool inlayHintsParameterReferenceModifiers = true;
    bool completionCompleteFunctionParens = true;
    /**
     * @brief Automatically prefix enum name when completing enum values (e.g. 'Enum::Value').
     */
    bool completionQualifyEnumValues = true;
    bool hoverStringLiteralLength = true;
    bool hoverStringLiteralPathResolution = true;
    /**
     * @brief Whether to display the declaring file location footer in hover tooltips.
     */
    bool hoverShowFileLocation = false;
    bool enableCommentSuppressions = true;
    /**
     * @brief Whether CScriptBuilder metadata blocks '[]' before declarations are supported.
     */
    bool enableMetadata = false;
    std::vector<std::string> assetSearchPaths;
};

/**
 * @brief General server information and CLI options.
 */
struct Info
{
    std::string name = "AngelScript Language Server";
    /**
     * @brief Version reported by --version and in the LSP handshake.
     *        Defined by CMake from client/package.json, which is the file that actually reaches users
     */
    std::string version = ANGELLSP_VERSION;
    std::string fileExtension = ".as";
    std::string predefinedFileExtension = ".as.predefined";
    std::string locale = "en";

    /**
     * @brief Log threshold: error | warning | info | debug. Parsed by utils::ParseLogLevel.
     *        Defaults to info rather than debug because every record is a real window/logMessage
     */
    std::string logLevel = "info";

    /**
     * @brief Multi-sink disk logging (.vscode/lsp/logs/): master.log, hover.log, etc.
     *        Defaults to false so disk logs are never created by default. In release builds,
     */
    bool enableFileLogging = false;

    bool showHelp = false;
    bool showVersion = false;
};

/**
 * @brief Host engine options that change what a script is allowed to say.
 *        AngelScript is not one language but a family of them: the host picks its dialect with
 */
struct EngineProperties
{
    /**
     * @brief asEP_ALLOW_UNSAFE_REFERENCES (engine default: false).
     *        With it off, `&` on a parameter means `&inout` and is only legal for an object type that
     */
    bool allowUnsafeReferences = false;

    /**
     * @brief asEP_PRIVATE_PROP_AS_PROTECTED (engine default: false).
     *        When set, a private member follows the protected rule instead: a derived class may reach
     */
    bool privatePropAsProtected = false;

    /**
     * @brief asEP_DISALLOW_GLOBAL_VARS (engine default: false).
     *        Hosts that give each script its own isolated state turn this on, and then a global
     */
    bool disallowGlobalVars = false;

    /**
     * @brief asEP_PROPERTY_ACCESSOR_MODE (engine default: 3; this server's default: 2).
     *        Decides when `get_X()` / `set_X(v)` become the virtual property `X`:
     */
    int propertyAccessorMode = 2;

    /**
     * @brief asEP_BOOL_CONVERSION_MODE: whether a class can stand where a bool is expected.
     *        Measured against angelscript_oracle: under mode 0, the engine's own default, `if (h)` on
     */
    int boolConversionMode = 0;

    /**
     * @brief asEP_ALLOW_MULTILINE_STRINGS: whether a plain "..." may span lines.
     *        Off in the engine and off here. A `"..."` carrying a raw newline is rejected with
     */
    bool allowMultilineStrings = false;

    /**
     * @brief asEP_USE_CHARACTER_LITERALS: character literal interpretation (engine default: 0).
     *        0 = 'x' is treated as a one-character string literal (the default).
     */
    int useCharacterLiterals = 0;

    /**
     * @brief asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE (engine default: false).
     *        When enabled, disallows value assignment for reference types, requiring handle assignment instead.
     */
    bool disallowValueAssignForRef = false;

    /**
     * @brief asEP_ALTER_SYNTAX_NAMED_ARGS: syntax for named arguments (engine default: 0).
     *        0 = ':' only (standard syntax).
     */
    int alterSyntaxNamedArgs = 0;

    /**
     * @brief asEP_DISABLE_INTEGER_DIVISION: integer division behavior (engine default: false).
     *        When enabled, division between two integers yields a float rather than truncating to an integer.
     */
    bool disableIntegerDivision = false;

    /**
     * @brief asEP_DISALLOW_EMPTY_LIST_ELEMENTS (engine default: false).
     *        When enabled, empty elements in initialization lists (e.g. `{1, , 3}`) are disallowed.
     */
    bool disallowEmptyListElements = false;

    /**
     * @brief asEP_FOREACH_SUPPORT (engine default: true).
     *        Whether the `for each` / `foreach` loop syntax is supported. Enabled by default in the engine.
     */
    bool foreachSupport = true;

    /**
     * @brief asEP_REQUIRE_ENUM_SCOPE (engine default: false).
     *        With it on, an unqualified enumerator stops resolving - `enum E { Alpha = 1 } int v = Alpha;`
     */
    bool requireEnumScope = false;

    /**
     * @brief asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT (engine default: true).
     *        With it OFF, a class that declares only a non-default constructor can no longer be
     */
    bool alwaysImplDefaultConstruct = false;

    /**
     * @brief asEP_ALLOW_UNICODE_IDENTIFIERS (engine default: false).
     *        With it off, a non-ASCII identifier is a parse error - `int arbol` spelled with an accented
     */
    bool allowUnicodeIdentifiers = false;

    /**
     * @brief asEP_IGNORE_DUPLICATE_SHARED_INTF (engine default: false).
     *        Declaring the same shared interface twice answers "Name conflict. 'I' is an interface."
     */
    bool ignoreDuplicateSharedIntf = false;

    /**
     * @brief asEP_COMPILER_WARNINGS (engine default: 1).
     *        Three values, and all three were measured on a script that produces "Signed/Unsigned mismatch" -
     */
    int compilerWarnings = 1;

    /**
     * @brief asEP_MEMBER_INIT_MODE (engine default: 0).
     *        0 = Member variables are initialized before base class constructor.
     */
    int memberInitMode = 0;

    /**
     * @brief asEP_HEREDOC_TRIM_MODE (engine default: 1).
     *        0 = Never trim heredoc strings.
     */
    int heredocTrimMode = 1;

    bool RequiresEnumScope() const
    {
        return requireEnumScope;
    }
    bool AlwaysImplementsDefaultConstruct() const
    {
        return alwaysImplDefaultConstruct;
    }
    bool AllowsUnicodeIdentifiers() const
    {
        return allowUnicodeIdentifiers;
    }
    bool IgnoresDuplicateSharedInterface() const
    {
        return ignoreDuplicateSharedIntf;
    }
    int CompilerWarningMode() const
    {
        return compilerWarnings;
    }

    // asEP_HEREDOC_TRIM_MODE and asEP_DISABLE_INTEGER_DIVISION are the two properties in the
    // brief that are NOT here, and both are absent for a measured reason rather than an
    // oversight. The oracle reads them - see server/tools/oracle/main.cpp - which is how the
    // measurement was taken.
    //
    // Heredoc trim (legal values 0..2, SDK default 1) decides whether a """..."""'s leading and
    // trailing blank lines are stripped. Eight probes across both settings and both shapes of
    // heredoc: every one compiles, with identical output. It changes the string's CONTENT, and
    // nothing about the source text tells a reader which content was wanted, so there is no
    // diagnostic to emit and no surprise to warn about.
    //
    // Integer division is the near-miss that shows the difference: it also changes no verdict,
    // but `float f = 1 / 2;` being 0.0 rather than 0.5 IS a surprise worth naming, so it is
    // here as disableIntegerDivision and gates an opt-in hint. Heredoc trim has no equivalent.
};

/**
 * @brief Diagnostics this server can emit but does not by default.
 *        Not feature switches and not engine options: each one is a rule that is right for a workspace
 */
struct DiagnosticsConfig
{
    /**
     * @brief Report a parameter or return type that resolves to no declaration (default: on).
     *        `void f(TypoTypeName x)` is a compile error - "Identifier 'TypoTypeName' is not a data
     */
    bool reportUnknownTypes = true;

    /**
     * @brief Hint on a get_/set_ accessor that carries no `property` keyword (default: off).
     *        Not a defect. Under asEP_PROPERTY_ACCESSOR_MODE 2, which is this server's default, such an
     */
    bool reportAccessorPortability = false;

    /**
     * @brief Hint where a script accessor is used as a property but the host disabled those.
     *        asEP_PROPERTY_ACCESSOR_MODE 0 turns property accessors off and 1 keeps only the ones the
     */
    bool reportAccessorDisabled = false;

    /**
     * @brief Hint on a class used where a bool is expected (default: off).
     *        Only meaningful when engine.boolConversionMode is 0, where the compiler rejects it. It is
     */
    bool reportBoolConversion = false;

    /**
     * @brief Hint when a type position names a function instead of a type (default: off).
     *        `void Foo(int) {}` followed by `Foo@ h = @Foo;` is rejected - "Identifier 'Foo' is not a
     */
    bool reportMissingFuncdef = false;

    /**
     * @brief Hint on integer division expressions (default: off).
     *        Off by default because it only matters when the host sets the matching engine property
     */
    bool reportIntegerDivision = false;

    /**
     * @brief Warn on potential dereference of a null or unchecked handle (default: off).
     *        Detects when handle parameters, handle cast expressions, or uninitialized handles
     */
    bool reportPossibleNullDereference = false;

    /**
     * @brief Warn on all dereferences of unguarded handles, not only the first (default: off).
     */
    bool reportAllNullDereferences = false;

    /**
     * @brief Warn or error on equality/inequality comparison of handle with null (default: 1).
     *        0 = off (suppress)
     */
    int reportHandleComparisonEquality = 1;

    /**
     * @brief Severity for missing asset paths referenced in string literals (default: "off").
     *        Values can be "off", "hint", "warning", or "error".
     */
    std::string missingAssetPathSeverity = "off";

    /**
     * @brief Warn on unused global variables (default: true).
     */
    bool reportUnusedGlobalVariables = true;

    /**
     * @brief True when reportUnusedGlobalVariables was explicitly set via CLI or workspace configuration.
     */
    bool reportUnusedGlobalVariablesExplicit = false;

    /**
     * @brief Warn on unused global/namespace functions and class methods (default: false).
     */
    bool reportUnusedFunctions = false;

    /**
     * @brief True when reportUnusedFunctions was explicitly set via CLI or workspace configuration.
     */
    bool reportUnusedFunctionsExplicit = false;

    /**
     * @brief User-configured function names (short or qualified) exempt from unused-function warnings.
     */
    std::unordered_set<std::string> ignoredUnusedFunctions;

    /**
     * @brief User-configured regular expression patterns exempting matching function names from unused-function
     *        warnings.
     */
    std::vector<std::string> ignoredUnusedFunctionPatterns;

    /**
     * @brief Ignore unused global variables initialized with a call or constructor (default: false).
     */
    bool ignoreUnusedGlobalsWithCallInitializer = false;

    /**
     * @brief Warn on unused classes (default: false).
     */
    bool reportUnusedClasses = false;

    /**
     * @brief True when reportUnusedClasses was explicitly set via CLI or workspace configuration.
     */
    bool reportUnusedClassesExplicit = false;

    /**
     * @brief Warn on unused class members (fields & methods) (default: false).
     */
    bool reportUnusedMembers = false;

    /**
     * @brief True when reportUnusedMembers was explicitly set via CLI or workspace configuration.
     */
    bool reportUnusedMembersExplicit = false;
};

/**
 * @brief Type configuration for AngelScript analysis.
 */
struct TypeConfig
{
    std::string stringTypeName = "string";
    std::string arrayTypeName = "array";

    /**
     * @brief Templates whose initializer list is a plain repeat of their element type.
     *        Initializer lists are validated against target types (such as array, dictionary, grid,
     */
    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> arrayLikeTemplates;

    TransparentStringSet registeredSymbols;
};

/**
 * @brief Formatting preferences.
 */
struct FormatConfig
{
    /**
     * @brief Where a *block*'s opening brace goes.
     *        A brace that opens a value - an initializer list, a lambda body - is not governed by
     */
    std::string braceStyle = "allman";

    /**
     * @brief Format the whole document when the user saves it manually (default: off).
     *        Drives the answer to `textDocument/willSaveWaitUntil`. OFF by default and that matters:
     */
    bool formatOnSave = false;

    /**
     * @brief Whether to insert spaces inside parentheses (e.g. 'foo( bar )' vs 'foo(bar)') (default: false).
     */
    bool spacesInsideParentheses = false;

    /**
     * @brief Whether to keep empty blocks on a single line (e.g. 'ClassName() {}') (default: true).
     */
    bool keepEmptyBlocksOnSingleLine = true;

    /**
     * @brief Where the handle symbol '@' aligns in declarations ("left", "right", "middle").
     *        "left" formats as 'Type@ var', matching canonical AngelScript style.
     */
    std::string pointerAlignment = "left";
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

    /**
     * @brief Preprocessor extensions the host added to its copy of CScriptBuilder.
     *        Separate from EngineProperties on purpose: those are asEP_* values passed to
     */
    angel_lsp::utils::PreprocessorFeatures preprocessor;

    /**
     * @brief What to say about a `#pragma`, which the stock add-on rejects outright.
     *        Measured: with no pragma callback registered, scriptbuilder.cpp:533 substitutes -1 for
     */
    enum class PragmaMode
    {
        Accept, ///< Say nothing. The default.
        Hint,   ///< A hint, for a host that is not sure it registered a callback.
        Error   ///< The stock add-on's own answer: no callback, so no pragma compiles.
    };

    PragmaMode pragmaMode = PragmaMode::Accept;
    DiagnosticsConfig diagnostics;
    FormatConfig format;
    std::vector<std::string> searchDirectories;

    /**
     * @brief Directory globs the workspace scans do not descend into.
     *        Three recursive walks cross every workspace root - the include graph, the predefined-stub
     */
    std::vector<std::string> exclude = {"**/.git/**", "**/build/**", "**/node_modules/**"};

    /**
     * @brief Words treated as defined for `#if <word>`, mirroring CScriptBuilder::DefineWord.
     *        AngelScript's preprocessor lives in the CScriptBuilder add-on and has exactly one
     */
    std::vector<std::string> definedWords;

    /**
     * @brief Predefined stub files to load by path, on top of the workspace scan.
     *        The scan only ever finds stubs that live inside a workspace folder, but a host
     */
    std::vector<std::string> predefinedFiles;
    /**
     * @brief Dynamic list of active predefined stub files selected by the user.
     *        When non-empty, only stubs matching these filenames or paths are contributing stubs.
     */
    std::vector<std::string> activeStubFiles;

    /**
     * @brief Files to force-include before analyzing any module or script.
     *        In hosts like Sven Co-op or specialized game engines, common headers or API bridges
     */
    std::vector<std::string> forceIncludeFiles;

    /**
     * @brief Global module entry-point script path.
     *        When configured, restricts the compilation closure of scripts participating in this
     */
    std::string moduleEntryPoint;

    /**
     * @brief Whether `#include "helper"` may mean `helper.as`.
     *        Off by default, which is AngelScript's own behaviour: CScriptBuilder opens exactly the
     */
    bool implicitIncludeExtension = false;

    /**
     * @brief One script module: a name, and the `.as` the host builds it from.
     *        A module is AngelScript's own unit of compilation - `builder.StartNewModule(engine,
     */
    struct ModuleDefinition
    {
        std::string name;

        /**
         * @brief Absolute path to the module's entry-point script, or empty.
         *        Everything this file reaches through `#include` is in the module, and it is where a
         */
        std::string entry;

        /**
         * @brief Absolute path to a directory whose scripts are all in this module, or empty.
         *        Sven Co-op is the shape this exists for: `scripts/maps` is the module MapScript and
         */
        std::string folder = {};

        /**
         * @brief True if module was implicitly created from moduleEntryPoint rather than explicit user config.
         */
        bool isImplicitEntryPoint = false;
    };

    /**
     * @brief The modules this workspace builds, or empty when the host has not said.
     *        Empty is the default and changes nothing: every rule that reads this stays exactly as
     */
    std::vector<ModuleDefinition> modules;

    /**
     * @brief Predefined stub file to exclusively load during the workspace scan.
     *        When empty, the server preserves its default behaviour of ingesting every predefined
     */
    std::string activePredefined;

    /**
     * @brief Path to the engine rules configuration file (default: "angelscript.rules.json").
     */
    std::string rulesFile;

    /**
     * @brief Active data-driven engine rules loaded from angelscript.rules.json.
     */
    EngineRuleConfig engineRules;

    /**
     * @brief Per-rule diagnostic severity overrides, keyed by diagnostic code.
     *        Values are the LSP severity names ("error", "warning", "information", "hint"). Kept as
     */
    std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> diagnosticSeverities;
};

/**
 * @brief Applies populated native engine properties from engine rules config to Server EngineProperties.
 * @param[in] props Native engine properties parsed from rules JSON.
 * @param[in,out] engine Engine properties to update with present values.
 */
void ApplyEngineProperties(const NativeEngineProperties& props, EngineProperties& engine);

/**
 * @brief Parses CLI arguments into a ServerConfig instance.
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line argument strings.
 * @return Initialized ServerConfig with parsed options.
 */
ServerConfig FromArgs(int argc, char** argv);

/**
 * @brief Prints the CLI help message to standard output.
 */
void PrintHelp();

/**
 * @brief Prints the version information to standard output.
 */
void PrintVersion();

/**
 * @brief Defines a boolean engine property configuration entry.
 */
struct BoolEngineProp
{
    using PropMember = bool EngineProperties::*;
    std::string_view name;
    std::string_view alias;
    PropMember member;
};

/**
 * @brief CLI argument parsing context tracking flags, positional inputs, and error states.
 */
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

    bool GetBoolValue(bool defaultVal = true);
};

/**
 * @brief Maps configuration setting keys to internal engine feature flag enumerations.
 */
struct FeatureFlagMapping
{
    using FeatureMember = bool FeatureFlags::*;
    std::string_view enableKey;
    std::string_view enableAlias;
    std::string_view disableKey;
    std::string_view disableAlias;
    FeatureMember member;
};

/**
 * @brief Maps configuration setting keys to diagnostic severity overrides and toggle states.
 */
struct DiagnosticFlagMapping
{
    using DiagMember = bool DiagnosticsConfig::*;
    std::string_view enableKey;
    std::string_view disableKey;
    DiagMember member;
};

/**
 * @brief Defines a boolean engine property configuration setting and its default value.
 */
struct EngineBoolFlag
{
    using FlagMember = bool EngineProperties::*;
    std::string_view enableFlag;
    std::string_view disableFlag;
    FlagMember member;
};
} // namespace angel_lsp::config
