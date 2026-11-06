#pragma once

#include "analysis/index/CallBindings.h"
#include "analysis/index/SymbolTable.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief What kind of declaration a LOCALS_QUERY @local.definition.* capture represents.
 */
enum class LocalDefinitionKind : uint8_t
{
    Parameter,
    Variable,
    Field,
    Function,
    Method,
    Type,
    Constant,
    Namespace,
    Import
};

/**
 * @brief Range of a template argument within a declared type.
 */
struct LocalTypeArgPosition
{
    std::string name;
    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;
};

/**
 * @brief Lexical definition occurrence (variable, parameter, loop variable) within a Scope.
 */
struct LocalDefinition
{
    LocalDefinition() = default;

    /**
     * @brief Constructs a LocalDefinition with name, kind, and optional definition and type ranges.
     * @param[in] name_ Identifier name.
     * @param[in] kind_ Local declaration kind.
     * @param[in] range Source range of the identifier.
     * @param[in] typeRange Source range of the declared type.
     */
    LocalDefinition(std::string name_, LocalDefinitionKind kind_, SourceRange range = {}, SourceRange typeRange = {})
        : name(std::move(name_)), kind(kind_), startLine(range.startLine), startCharacter(range.startCharacter),
          endLine(range.endLine), endCharacter(range.endCharacter), typeStartLine(typeRange.startLine),
          typeStartCharacter(typeRange.startCharacter), typeEndLine(typeRange.endLine),
          typeEndCharacter(typeRange.endCharacter)
    {
    }

    std::string name;
    std::string typeName;
    std::string defaultValue;

    std::vector<LocalTypeArgPosition> templateArgPositions;

    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;

    uint32_t startByte = 0;
    uint32_t endByte = 0;

    uint32_t typeStartLine = 0;
    uint32_t typeStartCharacter = 0;
    uint32_t typeEndLine = 0;
    uint32_t typeEndCharacter = 0;

    uint32_t fullStartLine = 0;
    uint32_t fullStartCharacter = 0;
    uint32_t fullEndLine = 0;
    uint32_t fullEndCharacter = 0;

    uint32_t visibleFromByte = 0;

    LocalDefinitionKind kind = LocalDefinitionKind::Variable;
    TypeKind typeKind = TypeKind::Unknown;

    bool isHandleType : 1 = false;
    bool hasNullInitializer : 1 = false;
    bool isConst : 1 = false;
    bool isReference : 1 = false;
};
static_assert(sizeof(LocalDefinition) <= 192);

/**
 * @brief A single identifier occurrence inside a Scope, not yet resolved to a definition.
 */
struct LocalReference
{
    std::string name;

    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;

    uint32_t startByte = 0;
    uint32_t endByte = 0;

    uint32_t argumentCount = 0;

    /**
     * @brief Byte range of the enclosing scoped_identifier or member_expression, if any.
     *        0 if no enclosing qualifier/member access expression exists.
     */
    uint32_t qualifierStartByte = 0;
    uint32_t qualifierEndByte = 0;

    uint32_t receiverStartByte = 0;
    uint32_t receiverEndByte = 0;

    uint8_t isMemberAccess : 1 = 0;
    uint8_t isTypeSpecifier : 1 = 0;
    uint8_t isNamedArgument : 1 = 0;
    uint8_t isCall : 1 = 0;
    uint8_t isScopedIdentifier : 1 = 0;
    uint8_t isSuperBaseCall : 1 = 0;
    uint8_t isLambdaFunctionKeyword : 1 = 0;
    uint8_t inMetadataOrError : 1 = 0;
};
static_assert(sizeof(LocalReference) <= 80);

/**
 * @brief A string literal call argument occurrence (e.g. SetTimeout("MyFunc", ...), CustomEntity("MyClass", ...)).
 */
struct StringLiteralCallRef
{
    std::string name;
    uint32_t line = 0;
    uint32_t column = 0;
};

/**
 * @brief What kind of syntactic construct created this lexical scope.
 */
enum class ScopeKind : uint8_t
{
    Block,
    Function,
    Closure,
    Class,
    Namespace,
    Global
};

/**
 * @brief A named expression and optional base captured for semantic receiver lookup.
 */
struct IndexedReceiverExpression
{
    std::string name;
    uint32_t base = UINT32_MAX;
    uint32_t argumentCount = UINT32_MAX;
    uint32_t startByte = 0;
};

/**
 * @brief Document-owned receiver facts shared by its lexical scopes.
 */
struct IndexedReferenceReceivers
{
    std::vector<IndexedReceiverExpression> expressions;
    ankerl::unordered_dense::map<uint32_t, uint32_t> byReference;
    ankerl::unordered_dense::map<uint32_t, std::string> qualifiers;
};

/**
 * @brief Context for enclosing class, namespace, caller and receiver facts shared by nested scopes.
 */
struct ScopeEnclosingContext
{
    std::string enclosingClass;
    std::string enclosingNamespace;
    std::string caller;
    std::shared_ptr<const IndexedReferenceReceivers> receivers;
    std::shared_ptr<const CallBindings> callBindings;
};

/**
 * @brief One lexical scope (function body, statement block, loop header, class body, ...) in the
 *        tree built from a document's LOCALS_QUERY matches. Scopes nest by source range: every
 */
struct Scope
{
    Scope* parent = nullptr;
    std::vector<std::unique_ptr<Scope>> children;
    std::vector<LocalDefinition> definitions;
    std::vector<LocalReference> references;
    std::unique_ptr<std::vector<StringLiteralCallRef>> stringLiteralCallRefs;

    uint32_t startLine = 0;
    uint32_t startCharacter = 0;
    uint32_t endLine = 0;
    uint32_t endCharacter = 0;

    /**
     * @brief Syntactic classification of this scope.
     */
    ScopeKind kind = ScopeKind::Block;
    bool isFunctionScope : 1 = false;
    bool isConstMethod : 1 = false;
    uint32_t containerSpanIndex = UINT32_MAX;

    std::shared_ptr<const ScopeEnclosingContext> enclosingContext;

    [[nodiscard]] const CallBindings* GetCallBindings() const noexcept
    {
        return enclosingContext ? enclosingContext->callBindings.get() : nullptr;
    }

    [[nodiscard]] std::string_view GetEnclosingClass() const noexcept
    {
        return enclosingContext ? std::string_view(enclosingContext->enclosingClass) : std::string_view{};
    }

    [[nodiscard]] std::string_view GetEnclosingNamespace() const noexcept
    {
        return enclosingContext ? std::string_view(enclosingContext->enclosingNamespace) : std::string_view{};
    }

    [[nodiscard]] std::string_view GetCaller() const noexcept
    {
        return enclosingContext ? std::string_view(enclosingContext->caller) : std::string_view{};
    }

    /**
     * @brief Destroys the subtree iteratively rather than by recursive unique_ptr teardown.
     *        The default destructor destroys `children`, each element of which destroys its own
     */
    ~Scope()
    {
        std::vector<std::unique_ptr<Scope>> pending;
        pending.reserve(children.size());
        for (auto& child : children)
            pending.push_back(std::move(child));
        children.clear();

        while (!pending.empty())
        {
            std::unique_ptr<Scope> node = std::move(pending.back());
            pending.pop_back();
            if (!node)
                continue;

            for (auto& grandChild : node->children)
                pending.push_back(std::move(grandChild));
            node->children.clear();
            // node is destroyed here with no children left, so no recursion.
        }
    }

    Scope() = default;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    /**
     * @brief Estimates total dynamic and structure memory used by this scope in bytes.
     */
    [[nodiscard]] size_t EstimateMemoryBytes() const noexcept
    {
        return sizeof(Scope) + definitions.capacity() * sizeof(LocalDefinition) +
               references.capacity() * sizeof(LocalReference) +
               (stringLiteralCallRefs ? sizeof(std::vector<StringLiteralCallRef>) +
                                            stringLiteralCallRefs->capacity() * sizeof(StringLiteralCallRef)
                                      : 0);
    }
};
static_assert(sizeof(Scope) <= 128);

/// Copies extracted lexical records into a mutable tree with its own enclosing links.
[[nodiscard]] std::shared_ptr<Scope> CopyLexicalScopeTree(const Scope& root);

/**
 * @brief Detects cycles in consecutive parent traversal without imposing a depth limit.
 */
class ScopeTraversalGuard
{
  public:
    /**
     * @brief Advances the fast pointer along the same parent chain as the caller.
     * @param[in] scope The caller's current scope, starting at the first visited scope.
     * @return True while the parent chain can still be traversed safely.
     */
    bool CheckAndInsert(const Scope* scope) noexcept
    {
        if (!scope)
        {
            return false;
        }
        if (!m_started)
        {
            m_fast = scope;
            m_started = true;
        }
        else if (m_fast == scope)
        {
            return false;
        }
        m_fast = m_fast && m_fast->parent ? m_fast->parent->parent : nullptr;
        return true;
    }

  private:
    const Scope* m_fast = nullptr;
    bool m_started = false;
};

/**
 * @brief Resolves name to the nearest enclosing definition starting from scope and walking up
 *        through Scope::parent, enforcing sequential byte-offset visibility for local variables.
 * @param scope Scope to start resolution from.
 * @param name Name of symbol to resolve.
 * @param refByte Byte offset of reference token.
 * @param owner Optional out-parameter receiving the Scope declaring the definition.
 * @param respectClosureBarrier When true (default), closure scopes cannot capture outer local
 *        variables or parameters from enclosing functions/closures.
 * @return The matching definition, or nullptr if no enclosing scope declares name visible at refByte.
 */
const LocalDefinition* ResolveInScopeAt(const Scope* scope, std::string_view name, uint32_t refByte,
                                        const Scope** owner = nullptr, bool respectClosureBarrier = true);

const LocalDefinition* ResolveInScopeAt(const Scope* scope, std::string_view name, uint32_t refByte,
                                        bool respectClosureBarrier);

/**
 * @brief Resolves name to the nearest enclosing definition starting from scope and walking up
 *        through Scope::parent, returning the first match found in the closest scope.
 * @param scope Scope to start resolution from.
 * @param name Name of symbol to resolve.
 * @param owner Optional out-parameter receiving the Scope declaring the definition.
 * @param respectClosureBarrier When true (default), closure scopes cannot capture outer local
 *        variables or parameters from enclosing functions/closures.
 * @return The matching definition, or nullptr if no enclosing scope declares name.
 */
const LocalDefinition* ResolveInScope(const Scope* scope, std::string_view name, const Scope** owner = nullptr,
                                      bool respectClosureBarrier = true);

const LocalDefinition* ResolveInScope(const Scope* scope, std::string_view name, bool respectClosureBarrier);

/// Tests the enclosing scope chain for a function without allocating a visited set.
[[nodiscard]] bool IsDeclaredInFunctionScope(const Scope* scope) noexcept;

/// Checks whether the visible name resolves to a variable or parameter declared in a function.
[[nodiscard]] bool IsShadowedByFunctionLocal(const Scope* scope, std::string_view name);

/**
 * @brief Searches the scope tree starting from root for the scope that declares def.
 * @param[in] root Root scope to begin search.
 * @param[in] def Definition to locate.
 * @return Pointer to declaring Scope if found, nullptr otherwise.
 */
const Scope* FindScopeDeclaringDefinition(const Scope* root, const LocalDefinition& def);

/**
 * @brief Checks if a scope's range contains the given source line and character.
 */
[[nodiscard]] inline bool ScopeContains(const Scope& scope, uint32_t line, uint32_t character) noexcept
{
    if (line < scope.startLine || line > scope.endLine)
    {
        return false;
    }
    if (line == scope.startLine && character < scope.startCharacter)
    {
        return false;
    }
    if (line == scope.endLine && character > scope.endCharacter)
    {
        return false;
    }
    return true;
}

/**
 * @brief Finds the direct child scope of parent containing the point, using binary search when child count > 8.
 */
[[nodiscard]] const Scope* FindContainingChild(const Scope& parent, uint32_t line, uint32_t character) noexcept;

/**
 * @brief Finds the innermost Scope containing the given source line and character.
 * @note Returns `root` when no child contains the point, even if `root` does not contain it
 *        either. FindInnermostScope below is the stricter variant; the two are not
 */
const Scope* FindEnclosingScope(const Scope* root, uint32_t line, uint32_t character);

/**
 * @brief Innermost Scope containing the point, or nullptr when `root` does not contain it.
 *        Differs from FindEnclosingScope in the out-of-range case: this reports "no scope" rather than
 */
const Scope* FindInnermostScope(const Scope* root, uint32_t line, uint32_t character);

/**
 * @brief Finds the innermost enclosing closure scope containing `scope`, if any.
 * @param scope Scope to inspect; chain is traversed up through parents.
 * @return The enclosing closure Scope, or nullptr if not within a closure.
 */
const Scope* FindEnclosingClosure(const Scope* scope);

/**
 * @brief Owns one Scope tree per open document, keyed by file URI. Mirrors SymbolTable's
 *        per-document lifecycle (clear-then-recollect on every didOpen/didChange/didSave) but,
 */
class ScopeIndex
{
  public:
    ScopeIndex() = default;

    /**
     * @brief Replaces (or sets for the first time) the scope tree for fileUri.
     */
    void SetScopeTree(const std::string& fileUri, std::unique_ptr<Scope> root);
    void SetScopeTree(const std::string& fileUri, std::shared_ptr<const Scope> root);

    /**
     * @brief Discards the scope tree for fileUri, if any.
     */
    void ClearDocument(const std::string& fileUri);

    /**
     * @brief Returns a snapshot of the root scope for fileUri, or nullptr if none is recorded.
     *        The returned handle keeps the tree alive even if a concurrent SetScopeTree/
     */
    std::shared_ptr<const Scope> GetRoot(const std::string& fileUri) const;
    std::shared_ptr<const Scope> GetRoot(std::string_view fileUri) const;

    /// Retires the published occurrences while keeping the last lexical scope available for editing features.
    void InvalidateDocumentOccurrences(const std::string& fileUri);

    [[nodiscard]] std::shared_ptr<const Scope>
    GetOccurrenceRoot(const std::string& fileUri, const std::optional<std::string>& module = std::nullopt) const;
    [[nodiscard]] std::shared_ptr<const Scope>
    GetOccurrenceRoot(std::string_view fileUri, const std::optional<std::string>& module = std::nullopt) const;

    template <typename Fn> void ForEachOccurrenceScope(const std::optional<std::string>& module, Fn&& callback) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        for (const auto& [uri, entry] : m_roots)
        {
            if (entry.occurrencesCurrent && MatchesModule(*entry.root, module))
            {
                callback(uri, entry.root);
            }
        }
    }

    /// Visits the current physical call ranges for a declaration under the index's shared lock.
    template <typename Fn>
    void ForEachBoundUse(const CallDeclaration& declaration, const std::optional<std::string>& module,
                         Fn&& callback) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        const auto visitModule = [&](const ModuleCallUses& calls)
        {
            const auto group = calls.find(declaration);
            if (group != calls.end())
            {
                for (const auto& [uri, ranges] : group->second)
                {
                    callback(*uri, ranges);
                }
            }
        };
        if (module)
        {
            if (const auto calls = m_callUses.find(*module); calls != m_callUses.end())
            {
                visitModule(calls->second);
            }
        }
        else
        {
            for (const auto& [name, calls] : m_callUses)
            {
                visitModule(calls);
            }
        }
    }

  private:
    struct ScopeEntry
    {
        std::shared_ptr<const Scope> root;
        bool occurrencesCurrent = false;
    };

    using DocumentCallUses = ankerl::unordered_dense::map<const std::string*, std::span<const SourceRange>>;
    using ModuleCallUses = ankerl::unordered_dense::map<CallDeclaration, DocumentCallUses, CallDeclarationHash>;

    static bool MatchesModule(const Scope& scope, const std::optional<std::string>& module);
    void RemoveCallUses(const std::string& uri, const ScopeEntry& entry);
    void AddCallUses(const std::string& uri, const Scope& scope);

    mutable std::shared_mutex m_mutex;
    // Node storage keeps URI addresses stable until their postings are retired.
    std::unordered_map<std::string, ScopeEntry, TransparentStringHash, std::equal_to<>> m_roots;
    ankerl::unordered_dense::map<std::string, ModuleCallUses> m_callUses;
};

/**
 * @brief Carries symbol lookup parameters, active scope node, and search boundary rules.
 */
struct ResolutionContext
{
    uint32_t refByte = 0;
    bool crossedClosure = false;
    bool respectClosureBarrier = false;
};
} // namespace angel_lsp::analysis
