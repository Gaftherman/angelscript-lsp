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
