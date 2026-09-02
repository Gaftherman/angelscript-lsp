#pragma once

#include "utils/PreprocessorRegions.h"

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
        bool enablePullDiagnostics = true;
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

        /**
         * @brief asEP_BOOL_CONVERSION_MODE: whether a class can stand where a bool is expected.
         *
         * Measured against angelscript_oracle: under mode 0, the engine's own default, `if (h)` on
         * a *script class* is rejected - "Expression must be of boolean type, instead found 'H&'" -
         * whether the class declares opImplConv, opConv, or both. Under mode 1 both forms are
         * accepted.
         *
         * The claim that used to sit here, that mode 0 means "never", was broader than the
         * measurement. Every probe used a script class, which in AngelScript is a reference type,
         * and the SDK's own comment says mode 0 still allows a **value type** to convert through
         * opImplConv. So it is "never for reference types, opImplConv only for value types" against
         * "either operator, either kind" - and the rule behind it cannot tell the two apart,
         * because no predefined stub can declare a type to be a value type.
         *
         * Defaults to 0 to match the engine. A host that sets 1 must say so here, or the analyzer
         * would report legal code.
         */
        int boolConversionMode = 0;

        /**
         * @brief asEP_ALLOW_MULTILINE_STRINGS: whether a plain "..." may span lines.
         *
         * Off in the engine and off here. A `"..."` carrying a raw newline is rejected with
         * "Multiline strings are not allowed in this application" - verified against
         * angelscript_oracle - while a `"""heredoc"""` spans lines under any setting.
         *
         * Reported by default rather than opt-in, and the corpus is why: a scanner that tracks
         * comments, escapes and heredocs found zero true multiline plain strings across all 1,061
         * files. Nothing legal is at risk, so silence would only cost the user the diagnostic.
         * A host that sets the property turns this off and the rule goes quiet.
         */
        bool allowMultilineStrings = false;

        /**
         * @brief asEP_USE_CHARACTER_LITERALS: character literal interpretation (engine default: 0).
         *
         * 0 = 'x' is treated as a one-character string literal (the default).
         * 1 = 'x' is treated as an integer value representing the character code.
         */
        int useCharacterLiterals = 0;

        /**
         * @brief asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE (engine default: false).
         *
         * When enabled, disallows value assignment for reference types, requiring handle assignment instead.
         */
        bool disallowValueAssignForRef = false;

        /**
         * @brief asEP_ALTER_SYNTAX_NAMED_ARGS: syntax for named arguments (engine default: 0).
         *
         * 0 = ':' only (standard syntax).
         * 1 = '=' allowed with a compiler warning.
         * 2 = '=' allowed silently.
         */
        int alterSyntaxNamedArgs = 0;

        /**
         * @brief asEP_DISABLE_INTEGER_DIVISION: integer division behavior (engine default: false).
         *
         * When enabled, division between two integers yields a float rather than truncating to an integer.
         */
        bool disableIntegerDivision = false;

        /**
         * @brief asEP_DISALLOW_EMPTY_LIST_ELEMENTS (engine default: false).
         *
         * When enabled, empty elements in initialization lists (e.g. `{1, , 3}`) are disallowed.
         */
        bool disallowEmptyListElements = false;

        /**
         * @brief asEP_FOREACH_SUPPORT (engine default: true).
         *
         * Whether the `for each` / `foreach` loop syntax is supported. Enabled by default in the engine.
         */
        bool foreachSupport = true;

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
     *
     * Not feature switches and not engine options: each one is a rule that is right for a workspace
     * whose declarations are complete and wrong for one whose host registers types in C++. The
     * default is the safe reading in every case, so a workspace that sets nothing keeps the
     * silent-unless-fully-visible policy intact.
     */
    struct DiagnosticsConfig
    {
        /**
         * @brief Report a parameter or return type that resolves to no declaration (default: on).
         *
         * `void f(TypoTypeName x)` is a compile error - "Identifier 'TypoTypeName' is not a data
         * type in namespace 'TEST' or parent" - and left unreported it surfaces as silence at every
         * call site instead, since a call whose parameter types are unknown cannot be judged
         * either. That is what makes it worth reporting: the cost is not one missing diagnostic but
         * a whole function's worth.
         *
         * It is also, from here, indistinguishable from a legitimate engine-registered type -
         * neither resolves to anything this analyzer can read - which is why this is a setting at
         * all. Measured on the 1061-file corpus, with the Sven Co-op profile loaded the way such a
         * workspace would: 1581 findings with this off, 3850 with it on. Without a profile, 4896
         * against 9791. A workspace whose host registers types in C++ and declares none of them
         * should turn this off, or better, name its engine profile.
         *
         * Local variable declarations are reported regardless, and always were. This aligns
         * parameters and return types with them.
         *
         * See tests/FunctionRulesTest.cpp, "Unknown Type Corpus Audit", to take that measurement
         * again.
         */
        bool reportUnknownTypes = true;

        /**
         * @brief Hint on a get_/set_ accessor that carries no `property` keyword (default: off).
         *
         * Not a defect. Under asEP_PROPERTY_ACCESSOR_MODE 2, which is this server's default, such an
         * accessor is a property and the code compiles. Under mode 3, the engine's own default, it is
         * not, and `c.X` becomes "'X' is not a member of 'C'". Adding the keyword is accepted under
         * both, so the hint is portability advice with a fix that cannot break the current build.
         *
         * Off by default because a workspace that has settled on mode 2 would otherwise see one hint
         * per accessor for a decision it already made.
         */
        bool reportAccessorPortability = false;

        /**
         * @brief Hint on a class used where a bool is expected (default: off).
         *
         * Only meaningful when engine.boolConversionMode is 0, where the compiler rejects it. It is
         * a hint rather than an error because the analyzer cannot see the host's engine setup: a
         * host running mode 1 makes the same code legal, and reporting it as an error there would
         * be a false positive on working code.
         */
        bool reportBoolConversion = false;

        /**
         * @brief Hint when a type position names a function instead of a type (default: off).
         *
         * `void Foo(int) {}` followed by `Foo@ h = @Foo;` is rejected - "Identifier 'Foo' is not a
         * data type", verified against angelscript_oracle - because a function handle needs a
         * funcdef to name its signature. The intent is unmistakable and the fix is mechanical, so
         * this is worth offering rather than leaving as a bare unresolved-type complaint.
         *
         * Off by default: the name could also belong to a host type this analyzer cannot see, and a
         * workspace whose engine registers one would get a hint about a type that exists.
         */
        bool reportMissingFuncdef = false;

        /**
         * @brief Hint on integer division expressions (default: off).
         *
         * Off by default because it only matters when the host sets the matching engine property
         * asEP_DISABLE_INTEGER_DIVISION.
         */
        bool reportIntegerDivision = false;



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
     * @brief Formatting preferences.
     */
    struct FormatConfig
    {
        /**
         * @brief Where a *block*'s opening brace goes.
         *
         * A brace that opens a value - an initializer list, a lambda body - is not governed by
         * this and never was: it stays on its line in either style. Kept as a string here because
         * this is Layer 1 and the enum that names the two styles belongs to the formatting
         * feature; the server maps it once, the way it already does for diagnostic severities.
         * Anything other than "kr" reads as Allman, so a typo is the default rather than an error.
         */
        std::string braceStyle = "allman";

        /**
         * @brief Format the whole document when the user saves it manually (default: off).
         *
         * Drives the answer to `textDocument/willSaveWaitUntil`. OFF by default and that matters:
         * the editor already has its own format-on-save setting, and a language server that
         * reformats every manual save regardless would override a choice the user made somewhere
         * else - silently, and on a file they were only trying to save.
         *
         * Only manual saves, never an autosave timer or a focus change, whatever this is set to.
         * Rewriting a file while the user is still typing in it is not something to offer as an
         * option.
         */
        bool formatOnSave = false;
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
         *
         * Separate from EngineProperties on purpose: those are asEP_* values passed to
         * SetEngineProperty and the SDK decides what they mean, while these describe a source file
         * the host is free to have patched. Every one is off by default and the defaults reproduce
         * the stock add-on exactly - see utils/PreprocessorRegions.h for what each measured.
         */
        angel_lsp::utils::PreprocessorFeatures preprocessor;

        /**
         * @brief What to say about a `#pragma`, which the stock add-on rejects outright.
         *
         * Measured: with no pragma callback registered, scriptbuilder.cpp:533 substitutes -1 for
         * the callback result, writes "Invalid #pragma directive" and fails the whole section. So
         * on a stock host every `#pragma` is a build failure.
         *
         * The default here is Accept anyway, and the reason is the zero-false-positives rule rather
         * than fidelity: a host that registers a callback is the common case, and defaulting to
         * Error would put a red squiggle on a pragma that builds fine for most of the people who
         * write one. Error is available for a host that really registered nothing.
         */
        enum class PragmaMode
        {
            Accept,  ///< Say nothing. The default.
            Hint,    ///< A hint, for a host that is not sure it registered a callback.
            Error    ///< The stock add-on's own answer: no callback, so no pragma compiles.
        };

        PragmaMode pragmaMode = PragmaMode::Accept;
        DiagnosticsConfig diagnostics;
        FormatConfig format;
        std::vector<std::string> searchDirectories;

        /**
         * @brief Directory globs the workspace scans do not descend into.
         *
         * Three recursive walks cross every workspace root - the include graph, the predefined-stub
         * scan and the engine-profile detector - and none of them could be told to stop. On a
         * repository with a build tree that is most of the work, and the profile detector was the
         * worst of the three: it collected the name of EVERY file it saw, with no extension filter
         * at all.
         *
         * Applied by pruning the directory rather than filtering the result, which is the whole
         * point: a filter still walks what it then throws away.
         *
         * `?`, `*` within a segment and `**` across segments, which is what the exclude settings
         * every editor ships already use, so a user can paste what they have. The defaults are the
         * three directories that are never source and are always large.
         */
        std::vector<std::string> exclude = { "**/.git/**", "**/build/**", "**/node_modules/**" };

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

