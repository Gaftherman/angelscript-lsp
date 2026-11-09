#pragma once

#include "analysis/diagnostics/Diagnostics.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/VirtualMixinContext.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/TraversalBudget.h"
#include "utils/PreprocessorRegions.h"
#include "utils/StopFlag.h"
#include "utils/TransparentHash.h"

#include <ankerl/unordered_dense.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Context and configuration passed into the semantic analysis process.
 */
struct SemanticAnalysisRequest
{
    SemanticAnalysisRequest(const SymbolTable& st, std::string uri, std::string predefinedExt = "",
                            const i18n::I18n* i18nPtr = nullptr)
        : symbolTable(st), fileUri(std::move(uri)), predefinedFileExtension(std::move(predefinedExt)), i18n(i18nPtr)
    {
    }

    /**
     * @brief Module compilation boundary context for external shared entities and cross-module imports.
     * @note When unconfigured, rules remain conservative and silence cross-module diagnostics.
     */
    struct ModuleContext
    {
        /**
         * @brief Module this document belongs to; empty when it is in none of them.
         */
        std::string name;

        /**
         * @brief Names declared `shared` by modules OTHER than this one.
         *        What an `external shared` declaration is allowed to refer to. Built by the server,
         */
        ankerl::unordered_dense::set<std::string> sharedElsewhere;

        /**
         * @brief Every configured module name, for the `import ... from "name"` hint.
         */
        std::vector<std::string> moduleNames;

        /**
         * @brief Modules that also claimed this file and lost, or empty when only one did.
         *        A file belongs to exactly one module here, and AngelScript really does allow one file
         */
        std::vector<std::string> alsoClaimedBy;
    };

    /**
     * @brief Module scoping, or empty when the host has not described its modules.
     */
    std::optional<ModuleContext> moduleContext;
    std::optional<CallBindingRevision> callBindingRevision;

    const SymbolTable& symbolTable;
    std::string fileUri;
    std::string predefinedFileExtension;

    /**
     * @brief Every file in the same module as this one, this one included, keyed as URIs.
     *        AngelScript compiles a module out of several sections, and a name declared twice across
     */
    ankerl::unordered_dense::set<std::string> moduleFileUris;
    const i18n::I18n* i18n = nullptr;
    const config::TypeConfig* typeConfig = nullptr;
    const ankerl::unordered_dense::map<std::string, DiagnosticSeverity, TransparentStringHash, std::equal_to<>>*
        severityOverrides = nullptr;

    /**
     * @brief The host engine's compile-time options, or nullptr for the engine's own defaults.
     *        Several AngelScript rules are decided by asIScriptEngine::SetEngineProperty rather than
     */
    const config::EngineProperties* engineProperties = nullptr;

    /**
     * @brief Opt-in diagnostics, or nullptr for the defaults. Owned by the caller, like the two
     *        above and for the same reason: a test that does not care should not have to build one.
     */
    const config::DiagnosticsConfig* diagnostics = nullptr;
    const config::EngineRuleConfig* engineRules = nullptr;

    /**
     * @brief Optional traversal budget to enforce bounded AST node visits across checkers.
     */
    parser::TraversalBudget* traversalBudget = nullptr;

    /**
     * @brief Optional cancellation flag to abort long-running semantic passes early.
     */
    const utils::StopFlag* stopFlag = nullptr;

    /**
     * @brief Optional memoization cache for resolved expression types across analysis passes.
     */
    mutable ExpressionTypeCache* exprCache = nullptr;

    /**
     * @brief Optional memoization cache for resolved mixin ASTs across class checks.
     */
    mutable rules::MixinFileCache* mixinCache = nullptr;

    /**
     * @brief Kill-switch for the conversion rules (see TypeConversionChecker.h).
     */
    bool enableTypeConversionChecks = true;

    /**
     * @brief Whether comment-based suppressions (e.g. // disable W156) are enabled.
     */
    bool enableCommentSuppressions = true;

    /**
     * @brief Whether CScriptBuilder metadata blocks '[]' are supported.
     */
    bool enableMetadata = false;

    /**
     * @brief Line ranges the preprocessor removes, so no diagnostic is reported inside them.
     *        CScriptBuilder blanks out every `#if <word>` block whose word the host has not defined
     */
    std::vector<utils::ExcludedLineRange> excludedLineRanges;

    /**
     * @brief Directives the preprocessor leaves in the source for the compiler to choke on.
     *        Populated from the same pass as excludedLineRanges, because the two answers depend on
     */
    std::vector<utils::UnsupportedDirective> unsupportedDirectives;

    /**
     * @brief Severity for an unsupported `#pragma`. Every other directive is a Warning.
     *        Its own knob because a pragma is the one case where the stock behaviour and the safe
     */
    DiagnosticSeverity pragmaSeverity = DiagnosticSeverity::Warning;

    /**
     * @brief Member and enum-member index for the declaration rules, built on first use.
     *        Mutable and lazy because most passes never ask for it. The build itself lives on the
     */
    const rules::RuleIndex& GetRuleIndex() const
    {
        if (!ruleIndex)
        {
            ruleIndex = symbolTable.GetRuleIndex();
        }
        return *ruleIndex;
    }

    /**
     * @brief Root of the document's lexical Scope tree (see ScopeTree.h), or nullptr if none was collected.
     */
    std::shared_ptr<const Scope> scopeRoot;

    /**
     * @brief Optional scope index of workspace documents for cross-file reference checking.
     */
    const ScopeIndex* scopeIndex = nullptr;

    /**
     * @brief The same tree as scopeRoot, non-null only while this caller exclusively owns it.
     *        One rule - `auto` inference in TypeConversionChecker - has to write the deduced type back
     * @warning Set this only for a tree you have not yet handed to ScopeIndex::SetScopeTree.
     *        Publish after Analyze() returns, never before.
     */
    Scope* mutableScopeRoot = nullptr;

    /**
     * @brief Document source text, owned by the caller and required to outlive Analyze().
     *        Empty when the caller has no text to offer. Rules that need to read an expression back
     */
    std::string_view sourceCode;

    /**
     * @brief Parsed syntax tree of the document, or nullptr.
     *        The SymbolTable records declarations, not expressions, so a rule about what an
     */
    const TSTree* tree = nullptr;

    /**
     * @brief Optional precomputed single-pass AST node index.
     *        When null, SemanticAnalyzer builds a local NodeIndex from tree on first use.
     */
    mutable const NodeIndex* nodeIndex = nullptr;

    /**
     * @brief Returns the precomputed NodeIndex, or nullptr if none was set.
     */
    [[nodiscard]] const NodeIndex* GetNodeIndex() const noexcept
    {
        return nodeIndex;
    }

    /**
     * @brief Gets configured name for the string type or empty if not configured.
     */
    std::string_view GetStringTypeName() const noexcept
    {
        return (typeConfig && !typeConfig->stringTypeName.empty()) ? std::string_view(typeConfig->stringTypeName)
                                                                   : std::string_view("");
    }

    /**
     * @brief Gets configured name for the array type or empty if not configured.
     */
    std::string_view GetArrayTypeName() const noexcept
    {
        return (typeConfig && !typeConfig->arrayTypeName.empty()) ? std::string_view(typeConfig->arrayTypeName)
                                                                  : std::string_view("");
    }

    /**
     * @brief Gets effective string type name, defaulting to 'string' if unconfigured.
     */
    std::string_view GetEffectiveStringTypeName() const noexcept
    {
        const auto configured = GetStringTypeName();
        return configured.empty() ? symbolTable.GetStringType() : configured;
    }

    /**
     * @brief Gets effective array type name, defaulting to 'array' if unconfigured.
     */
    std::string_view GetEffectiveArrayTypeName() const noexcept
    {
        const auto configured = GetArrayTypeName();
        return configured.empty() ? symbolTable.GetDefaultArrayType() : configured;
    }

    /**
     * @brief Checks whether a template name matches the configured array type or an array-like template.
     * @param[in] templateName Template identifier to check.
     * @return True if templateName is an array or array-like template.
     */
    bool IsArrayLikeTemplate(std::string_view templateName) const
    {
        if (templateName.empty())
        {
            return false;
        }
        if (templateName == GetEffectiveArrayTypeName())
        {
            return true;
        }
        return typeConfig && typeConfig->arrayLikeTemplates.contains(templateName);
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
     *        asEP_PROPERTY_ACCESSOR_MODE. Under 3 - the SDK's own default - an accessor without the
     */
    bool RequiresAccessorKeyword() const
    {
        return engineProperties && engineProperties->propertyAccessorMode == 3;
    }

    /**
     * @brief False when the host disabled script-defined property accessors entirely.
     *        asEP_PROPERTY_ACCESSOR_MODE 0 turns accessors off, and 1 keeps only the ones the
     */
    bool ScriptAccessorsAreProperties() const
    {
        return !engineProperties || engineProperties->propertyAccessorMode >= 2;
    }

    /**
     * @brief asEP_BOOL_CONVERSION_MODE: 0 when a class may never stand where a bool is expected.
     *        Answers 0 with no engine properties at all, which is both the engine's own default and
     */
    int BoolConversionMode() const
    {
        return engineProperties ? engineProperties->boolConversionMode : 0;
    }

    /**
     * @brief True when the host built its engine with asEP_ALLOW_MULTILINE_STRINGS.
     *        False with no engine properties at all, matching the engine's own default: a plain
     */
    bool AllowsMultilineStrings() const
    {
        return engineProperties && engineProperties->allowMultilineStrings;
    }

    /**
     * @brief asEP_USE_CHARACTER_LITERALS: 0 = 'x' is a string, 1 = 'x' is an integer.
     *        Answers 0 with no engine properties, matching the engine's default.
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
     *        Answers 0 with no engine properties, matching the engine's default.
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
     *        Answers true with no engine properties, matching the engine's default.
     */
    bool SupportsForeach() const
    {
        return engineProperties ? engineProperties->foreachSupport : true;
    }

    /**
     * @brief What the host's engine does with a warning: 0 drop, 1 emit, 2 promote to error.
     *        asEP_COMPILER_WARNINGS, and the only engine property that moves a severity rather than
     */
    int CompilerWarningMode() const
    {
        return engineProperties ? engineProperties->compilerWarnings : 1;
    }

    /**
     * @brief True when the host built its engine with asEP_REQUIRE_ENUM_SCOPE.
     *        With it on an unqualified enumerator stops resolving: `enum E { Alpha = 1 } int v = Alpha;`
     */
    bool RequiresEnumScope() const
    {
        return engineProperties && engineProperties->requireEnumScope;
    }

    /**
     * @brief True when the engine implements a default constructor for every class (default: true).
     *        With it OFF, a class declaring only a non-default constructor can no longer be
     */
    bool AlwaysImplementsDefaultConstruct() const
    {
        return engineProperties && engineProperties->alwaysImplDefaultConstruct;
    }

    /**
     * @brief True when the host built its engine with asEP_ALLOW_UNICODE_IDENTIFIERS.
     *        With it off a non-ASCII identifier is a parse error - an accented `arbol` answers
     */
    bool AllowsUnicodeIdentifiers() const
    {
        return engineProperties && engineProperties->allowUnicodeIdentifiers;
    }

    /**
     * @brief True when the host built its engine with asEP_IGNORE_DUPLICATE_SHARED_INTF.
     *        Declaring the same shared interface twice answers "Name conflict. 'I' is an interface."
     */
    bool IgnoresDuplicateSharedInterface() const
    {
        return engineProperties && engineProperties->ignoreDuplicateSharedIntf;
    }

    /**
     * @brief True when the host built its engine with asEP_DISALLOW_GLOBAL_VARS.
     */
    bool DisallowsGlobalVars() const
    {
        return engineProperties && engineProperties->disallowGlobalVars;
    }

    /**
     * @brief Whether an unresolved parameter or return type should be reported. See
     *        config::DiagnosticsConfig::reportUnknownTypes for why this is off by default.
     */
    bool ReportsUnknownTypes() const
    {
        return diagnostics && diagnostics->reportUnknownTypes;
    }

    /**
     * @brief Checks if a symbol name is in the registered engine symbol allowlist.
     */
    bool IsRegisteredSymbol(std::string_view name) const
    {
        return typeConfig && typeConfig->registeredSymbols.contains(std::string(name));
    }

    /**
     * @brief Cache behind GetRuleIndex(). Never set by a caller; see that accessor.
     */
    mutable std::shared_ptr<const rules::RuleIndex> ruleIndex;

    /**
     * @brief Lazily populated cache for using namespace strings in this document.
     */
    mutable std::optional<std::vector<std::string>> usingNamespacesCache;
};
} // namespace angel_lsp::analysis
