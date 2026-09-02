#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "config/ServerConfig.h"
#include "utils/PreprocessorRegions.h"

#include <memory>
#include <string>
#include <string_view>
#include <ankerl/unordered_dense.h>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Context and configuration passed into the semantic analysis process.
     */
    struct SemanticAnalysisRequest
    {
        SemanticAnalysisRequest(const SymbolTable &st,
                                std::string uri,
                                std::string predefinedExt = "",
                                const i18n::I18n *i18nPtr = nullptr)
            : symbolTable(st), fileUri(std::move(uri)), predefinedFileExtension(std::move(predefinedExt)), i18n(i18nPtr) {}

        const SymbolTable &symbolTable;
        std::string fileUri;
        std::string predefinedFileExtension;
        const i18n::I18n *i18n = nullptr;
        const config::TypeConfig *typeConfig = nullptr;
        const ankerl::unordered_dense::map<std::string, DiagnosticSeverity> *severityOverrides = nullptr;

        /**
         * @brief The host engine's compile-time options, or nullptr for the engine's own defaults.
         *
         * Several AngelScript rules are decided by asIScriptEngine::SetEngineProperty rather than
         * by anything in the script, so a rule that depends on one is undecidable from text alone.
         * This is where the answer arrives. Read through the accessors below, which fall back to
         * the engine's documented defaults when no configuration was supplied - the same shape as
         * typeConfig, and for the same reason: every test that does not care about engine options
         * should not have to construct one.
         */
        const config::EngineProperties *engineProperties = nullptr;

        /**
         * @brief Opt-in diagnostics, or nullptr for the defaults. Owned by the caller, like the two
         * above and for the same reason: a test that does not care should not have to build one.
         */
        const config::DiagnosticsConfig *diagnostics = nullptr;

        /** @brief Kill-switch for the conversion rules (see TypeConversionChecker.h). */
        bool enableTypeConversionChecks = true;

        /**
         * @brief Line ranges the preprocessor removes, so no diagnostic is reported inside them.
         *
         * CScriptBuilder blanks out every `#if <word>` block whose word the host has not defined
         * before the compiler sees the file, so code in such a block is not code at all. Analysing
         * it produced diagnostics about text that never compiles - AS-Harness's json.as keeps
         * deliberately-broken code in `#if FALSE` for exactly that purpose, and every diagnostic
         * this analyzer produced for that file came from there.
         *
         * Empty means nothing is excluded. Populate with utils::FindExcludedLineRanges.
         */
        std::vector<utils::ExcludedLineRange> excludedLineRanges;

        /**
         * @brief Directives the preprocessor leaves in the source for the compiler to choke on.
         *
         * Populated from the same pass as excludedLineRanges, because the two answers depend on
         * each other: a `#define` inside an excluded block compiles fine and one line outside it
         * does not. Left empty for a predefined stub, where `#define` is this server's own syntax
         * and never reaches AngelScript at all.
         */
        std::vector<utils::UnsupportedDirective> unsupportedDirectives;

        /**
         * @brief Severity for an unsupported `#pragma`. Every other directive is a Warning.
         *
         * Its own knob because a pragma is the one case where the stock behaviour and the safe
         * behaviour disagree: the add-on rejects every pragma when no callback is registered, but
         * a host that registered one accepts anything, so this defaults to saying nothing at all
         * and the user chooses hint or error. See ServerConfig::PragmaMode.
         */
        DiagnosticSeverity pragmaSeverity = DiagnosticSeverity::Warning;

        /**
         * @brief Member and enum-member index for the declaration rules, built on first use.
         *
         * Mutable and lazy because most passes never ask for it. The build itself lives on the
         * SymbolTable, which rebuilds only when its version has moved - so an edit costs one walk
         * and the analyses in between cost none. Held by shared_ptr here so the index stays alive
         * for the whole request even if a writer supersedes it midway.
         */
        const rules::RuleIndex &GetRuleIndex() const
        {
            if (!ruleIndex)
            {
                ruleIndex = symbolTable.GetRuleIndex();
            }
            return *ruleIndex;
        }

        /** @brief Root of the document's lexical Scope tree (see ScopeTree.h), or nullptr if none was collected. */
        std::shared_ptr<const Scope> scopeRoot;

        /**
         * @brief The same tree as scopeRoot, non-null only while this caller exclusively owns it.
         *
         * One rule - `auto` inference in TypeConversionChecker - has to write the deduced type back
         * into the LocalDefinition, because every consumer downstream (hover, completion, inlay
         * hints, the other checkers) reads the concrete type from there rather than re-deducing it.
         *
         * That write used to go through a const_cast on scopeRoot, which was a data race: the
         * analysis thread published the tree to ScopeIndex *before* running Analyze, so it was
         * mutating a std::string that the message loop could be reading for a hover at the same
         * instant. Setting this field is the caller stating that the tree has not been published
         * yet and no other thread can reach it; leaving it null makes the rule skip the write.
         *
         * @warning Set this only for a tree you have not yet handed to ScopeIndex::SetScopeTree.
         *          Publish after Analyze() returns, never before.
         */
        Scope *mutableScopeRoot = nullptr;

        /**
         * @brief Document source text, owned by the caller and required to outlive Analyze().
         *
         * Empty when the caller has no text to offer. Rules that need to read an expression back
         * (type conversions) are skipped rather than guessed at when it is.
         */
        std::string_view sourceCode;

        /**
         * @brief Parsed syntax tree of the document, or nullptr.
         *
         * The SymbolTable records declarations, not expressions, so a rule about what an
         * initializer or a cast actually contains has no other source of truth. Also owned by the
         * caller: it must not be deleted until Analyze() returns.
         */
        const TSTree *tree = nullptr;

        /**
         * @brief Gets configured name for the string type or 'string' default.
         */
        std::string_view GetStringTypeName() const
        {
            return (typeConfig && !typeConfig->stringTypeName.empty()) ? std::string_view(typeConfig->stringTypeName) : std::string_view("");
        }

        /**
         * @brief Gets configured name for the array type or empty if not configured.
         */
        std::string_view GetArrayTypeName() const
        {
            return (typeConfig && !typeConfig->arrayTypeName.empty()) ? std::string_view(typeConfig->arrayTypeName) : std::string_view("");
        }

        /**
         * @brief Templates whose initializer list repeats their element type.
         * @see config::TypeConfig::arrayLikeTemplates for why this is configured and not inferred.
         */
        std::unordered_set<std::string> GetArrayLikeTemplateNames() const
        {
            return typeConfig ? typeConfig->ArrayLikeTemplateNames() : std::unordered_set<std::string>{};
        }

        /**
         * @brief True when the host built its engine with asEP_ALLOW_UNSAFE_REFERENCES.
         */
        bool AllowsUnsafeReferences() const
        {
            return engineProperties && engineProperties->allowUnsafeReferences;
        }

        /**
         * @brief True when the host built its engine with asEP_PRIVATE_PROP_AS_PROTECTED.
         */
        bool TreatsPrivateAsProtected() const
        {
            return engineProperties && engineProperties->privatePropAsProtected;
        }

        /**
         * @brief True when `get_X`/`set_X` is only the property `X` if it carries `property`.
         *
         * asEP_PROPERTY_ACCESSOR_MODE. Under 3 - the SDK's own default - an accessor without the
         * keyword is an ordinary method and `c.V` is "'V' is not a member of 'C'"; under 2 the name
         * alone is enough. Both measured, in tests/parity/doc_r07_accessor_without_kw.as:
         * `angelscript_oracle --property-accessor-mode=3` rejects it and `=2` accepts it.
         *
         * This server defaults to 2, the more permissive of the two, and the reasoning is at
         * EngineProperties::propertyAccessorMode: under 2 the analyzer misses a diagnostic a
         * mode-3 host would give, and under 3 it would invent one for a mode-2 host. Missing beats
         * inventing.
         *
         * Every rule that treats an accessor as a property has to ask this, or the setting is only
         * honoured in the one place that remembered to.
         */
        bool RequiresAccessorKeyword() const
        {
            return engineProperties && engineProperties->propertyAccessorMode == 3;
        }

        /**
         * @brief False when the host disabled script-defined property accessors entirely.
         *
         * asEP_PROPERTY_ACCESSOR_MODE 0 turns accessors off, and 1 keeps only the ones the
         * application registered in C++ - as_compiler.cpp:14077 skips a candidate with
         * `if (ep.propertyAccessorMode == 1 && f->funcType == asFUNC_SCRIPT) continue;`. Under
         * either, `c.X` backed by a script `get_X`/`set_X` is rejected, with the `property` keyword
         * and without it. Measured across all four modes, both spellings.
         *
         * True when nothing is configured, which keeps the default behaviour: resolution still
         * treats an accessor as a property, and the disagreement is reported as an opt-in hint
         * rather than by making the member vanish. A host whose configuration here is wrong would
         * otherwise see errors on code that compiles for it, which is the asymmetry the
         * zero-false-positives rule exists to prevent.
         */
        bool ScriptAccessorsAreProperties() const
        {
            return !engineProperties || engineProperties->propertyAccessorMode >= 2;
        }

        /**
         * @brief asEP_BOOL_CONVERSION_MODE: 0 when a class may never stand where a bool is expected.
         *
         * Answers 0 with no engine properties at all, which is both the engine's own default and
         * the conservative reading - a rule that fires under 0 is one the analyzer has to be asked
         * for anyway, and one that stays silent under 1 cannot be wrong by defaulting to 0.
         */
        int BoolConversionMode() const
        {
            return engineProperties ? engineProperties->boolConversionMode : 0;
        }

        /**
         * @brief True when the host built its engine with asEP_ALLOW_MULTILINE_STRINGS.
         *
         * False with no engine properties at all, matching the engine's own default: a plain
         * "..." spanning lines is rejected unless the host asked for it.
         */
        bool AllowsMultilineStrings() const
        {
            return engineProperties && engineProperties->allowMultilineStrings;
        }

        /**
         * @brief asEP_USE_CHARACTER_LITERALS: 0 = 'x' is a string, 1 = 'x' is an integer.
         *
         * Answers 0 with no engine properties, matching the engine's default.
         */
        int CharacterLiteralMode() const
        {
            return engineProperties ? engineProperties->useCharacterLiterals : 0;
        }

        /**
         * @brief True when the host built its engine with asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE.
         */
        bool DisallowsValueAssignForRef() const
        {
            return engineProperties && engineProperties->disallowValueAssignForRef;
        }

        /**
         * @brief asEP_ALTER_SYNTAX_NAMED_ARGS: 0 = ':' only, 1 = '=' with warning, 2 = '=' silently.
         *
         * Answers 0 with no engine properties, matching the engine's default.
         */
        int NamedArgumentSyntaxMode() const
        {
            return engineProperties ? engineProperties->alterSyntaxNamedArgs : 0;
        }

        /**
         * @brief True when the host built its engine with asEP_DISABLE_INTEGER_DIVISION.
         */
        bool DisablesIntegerDivision() const
        {
            return engineProperties && engineProperties->disableIntegerDivision;
        }

        /**
         * @brief True when the host built its engine with asEP_DISALLOW_EMPTY_LIST_ELEMENTS.
         */
        bool DisallowsEmptyListElements() const
        {
            return engineProperties && engineProperties->disallowEmptyListElements;
        }

        /**
         * @brief True when the host built its engine with asEP_FOREACH_SUPPORT (default: true).
         *
         * Answers true with no engine properties, matching the engine's default.
         */
        bool SupportsForeach() const
        {
            return engineProperties ? engineProperties->foreachSupport : true;
        }

        /**
         * @brief True when the host built its engine with asEP_DISALLOW_GLOBAL_VARS.
         */
        bool DisallowsGlobalVars() const
        {
            return engineProperties && engineProperties->disallowGlobalVars;
        }

        /**
         * @brief Checks if a symbol name is in the registered engine symbol allowlist.
         */
        /** @brief Whether an unresolved parameter or return type should be reported. See
         *         config::DiagnosticsConfig::reportUnknownTypes for why this is off by default. */
        bool ReportsUnknownTypes() const
        {
            return diagnostics && diagnostics->reportUnknownTypes;
        }

        bool IsRegisteredSymbol(const std::string &name) const
        {
            return typeConfig && typeConfig->registeredSymbols.contains(name);
        }

        /** @brief Cache behind GetRuleIndex(). Never set by a caller; see that accessor. */
        mutable std::shared_ptr<const rules::RuleIndex> ruleIndex;
    };
}
