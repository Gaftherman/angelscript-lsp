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
        bool enableCallHierarchy = true;
        bool enableTypeHierarchy = true;
        bool enableLinkedEditing = true;
        bool enableCodeLens = true;
        bool enableOnTypeFormatting = true;
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

        /**
         * @brief Log threshold: error | warning | info | debug. Parsed by utils::ParseLogLevel.
         *
         * Defaults to info rather than debug because every record is a real window/logMessage
         * notification over the same connection as the responses - the debug level dumps every
         * symbol in a document on every analysis and is a genuine throughput cost, not free
         * instrumentation.
         */
        std::string logLevel = "info";

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

        /**
         * @brief asEP_PROPERTY_ACCESSOR_MODE (engine default: 3; this server's default: 2).
         *
         * Decides when `get_X()` / `set_X(v)` become the virtual property `X`:
         *
         *   2  always, whether or not the declaration carries the `property` keyword
         *   3  only where it does - the engine's own default
         *
         * The engine values 0 (disabled) and 1 (app-registered accessors only) are not modelled:
         * neither changes what a *script* declaration means, which is the only thing this analyzer
         * reads.
         *
         * This server defaults to 2, not to the engine's 3, and the difference is deliberate. Under
         * 3 a `c.V` whose accessor lacks the keyword is an error the compiler does report, so
         * defaulting to 3 would be more faithful - and would hand a new diagnostic to every
         * workspace whose host sets 2, which many do, for code that compiles for them today. Being
         * lenient here misses an error; being strict invents one. See PARITY-BACKLOG.md.
         */
        int propertyAccessorMode = 2;
    };

    /**
     * @brief Diagnostics this server can emit but does not by default.
     *
     * Not feature switches and not engine options: each one is a rule that is right for a workspace
     * whose declarations are complete and wrong for one whose host registers types in C++. The
     * default is the safe reading in every case, so a workspace that sets nothing keeps the
     * silent-unless-fully-visible policy intact.
     */
    struct DiagnosticsConfig
    {
        /**
         * @brief Report a parameter or return type that resolves to no declaration (default: off).
         *
         * `void f(TypoTypeName x)` is a compile error - "Identifier 'TypoTypeName' is not a data
         * type in namespace 'TEST' or parent" - and it is also exactly what a legitimate
         * engine-registered type looks like from here: neither resolves to anything this analyzer
         * can read. A Sven Co-op script naming `CBaseEntity` in every handler would light up on
         * every function.
         *
         * Measured on the 1061-file corpus: 7096 findings with this off, 13999 with it on, and the
         * sample is `Vector` and `CBaseEntity` all the way down - types a Sven Co-op host registers
         * in C++ and no stub declares. That is the whole argument for the default.
         *
         * So it is opt-in, for a workspace whose API is fully declared in `as.predefined` or covered
         * by an engine profile. Turn it on there and a typo in a signature is caught at the
         * declaration instead of surfacing as silence at every call site.
         *
         * See tests/FunctionRulesTest.cpp, "Unknown Type Corpus Audit", to take that measurement
         * again.
         *
         * Local variable declarations are reported regardless, and always were: a local's type is
         * almost always a script type, where a parameter's is very often the engine's.
         */
        bool reportUnknownTypes = false;
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
         *
         * This is the shorthand. The general mechanism is a `@listpattern` tag in the stub itself,
         * carrying the pattern from the type's own `asBEHAVE_LIST_FACTORY` registration - see
         * analysis/ListPattern.h. That expresses shapes this set cannot, such as `dictionary`'s
         * `{repeat {string, ?}}`; this exists for a host that would rather not edit a stub it does
         * not own.
         *
         * Either way it has to be stated rather than inferred. Reading "one type parameter" as
         * "array-like" would be wrong: AS-Harness declares `optional<T>` in exactly the same shape
         * as `array<T>`, yet the real compiler answers `optional<int> o = {1};` with
         * "Initialization lists cannot be used with 'optional<int>'". Left alone, this holds
         * arrayTypeName, whose `T[]` spelling the language settles on its own.
         */
        std::unordered_set<std::string> arrayLikeTemplates;

        std::unordered_set<std::string> registeredSymbols;

        /** @brief arrayLikeTemplates, with arrayTypeName included whether or not it was listed. */
        std::unordered_set<std::string> ArrayLikeTemplateNames() const
        {
            std::unordered_set<std::string> names = arrayLikeTemplates;
            if (!arrayTypeName.empty())
            {
                names.insert(arrayTypeName);
            }
            return names;
        }
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
        DiagnosticsConfig diagnostics;
        std::vector<std::string> searchDirectories;

        /**
         * @brief Words treated as defined for `#if <word>`, mirroring CScriptBuilder::DefineWord.
         *
         * AngelScript's preprocessor lives in the CScriptBuilder add-on and has exactly one
         * conditional: `#if <identifier>` ... `#endif`. The identifier is not evaluated - it is
         * looked up in a set the *host application* populates, and a block whose word is absent is
         * blanked out before compilation.
         *
         * Empty is the correct default: an unconfigured builder defines nothing, so every `#if`
         * block is excluded. A host that calls DefineWord should list the same words here, or the
         * server will stay silent about code that really is compiled.
         */
        std::vector<std::string> definedWords;

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
         * @brief Built-in predefined engine profile identifier (e.g. standard, svencoop, urho3d, openxray, ootp, none, auto).
         */
        std::string engineProfile = "none";

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

