#pragma once

#include "analysis/SymbolTable.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::analysis
{
    /** @brief What kind of declaration a LOCALS_QUERY @local.definition.* capture represents. */
    enum class LocalDefinitionKind
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

    /** @brief A single named declaration site inside a Scope (function parameter, local variable, class field, ...). */
    struct LocalDefinition
    {
        std::string name;
        LocalDefinitionKind kind;

        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;

        /**
         * @brief Only meaningful when kind == Variable and the definition came from a
         *        variable_declarator (not a foreach_variable, which has no declared type node):
         *        true if the declared type has a trailing '@' handle marker. See
         *        SemanticAnalyzer::CheckNullAssignedToNonHandleInScope.
         */
        bool isHandleType = false;

        /**
         * @brief Same condition as isHandleType: the declared type's TypeKind, e.g. TypeKind::Int32
         *        for a VM-intrinsic primitive vs TypeKind::Unknown/Object for a class type (whose
         *        null-compatibility depends on app/engine registration this analyzer can't see).
         *        Defaults to Unknown, same as an un-populated VariableSignature::typeKind.
         */
        TypeKind typeKind = TypeKind::Unknown;

        /**
         * @brief Only meaningful under the same condition as isHandleType: true if the
         *        initializer's own top-level value is a 'null' literal (see
         *        TypeExtraction.h::IsNullInitializer).
         */
        bool hasNullInitializer = false;

        /** @brief Same condition as isHandleType: the declared type's source text, e.g. "int". */
        std::string typeName;
    };

    /** @brief A single identifier occurrence inside a Scope, not yet resolved to a definition. */
    struct LocalReference
    {
        std::string name;

        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;

        /**
         * @brief True when this identifier is the member field of a member_expression (the "y" in
         *        "x.y"). LOCALS_QUERY's bare (identifier) @local.reference matches every identifier
         *        node including these, but a member name isn't a lexical reference - resolving it
         *        needs type-aware member lookup, which nothing here does. Consumers that walk scope
         *        chains/SymbolTable to resolve references (e.g. undefined-identifier checks) must
         *        skip these to avoid flagging ordinary member access as undefined.
         */
        bool isMemberAccess = false;
    };

    /**
     * @brief One lexical scope (function body, statement block, loop header, class body, ...) in the
     *        tree built from a document's LOCALS_QUERY matches. Scopes nest by source range: every
     *        definition/reference is attached to the innermost scope whose range contains it, and
     *        every child scope's range is contained within its parent's.
     */
    struct Scope
    {
        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;

        /**
         * @brief True when this scope was opened by a func_declaration or lambda_expression (as
         * opposed to script/namespace_body/class_body/interface_body/statement_block/for_statement/
         * foreach_statement/switch_statement). Lets consumers distinguish a true function-body local
         * from a module/namespace/class-scope declaration - LOCALS_QUERY's @local.definition.var
         * captures both under the identical LocalDefinitionKind::Variable, with no other way to
         * tell them apart from a Scope/LocalDefinition alone.
         */
        bool isFunctionScope = false;

        Scope *parent = nullptr;
        std::vector<std::unique_ptr<Scope>> children;
        std::vector<LocalDefinition> definitions;
        std::vector<LocalReference> references;
    };

    /**
     * @brief Resolves name to the nearest enclosing definition starting from scope and walking up
     *        through Scope::parent, returning the first match found in the closest scope. This is a
     *        plain nearest-enclosing-scope lookup with no declare-before-use/hoisting rules - it is
     *        the primitive a future language-aware resolver would build on, not a complete binder.
     * @return The matching definition, or nullptr if no enclosing scope declares name.
     */
    const LocalDefinition *ResolveInScope(const Scope *scope, std::string_view name);

    /**
     * @brief Owns one Scope tree per open document, keyed by file URI. Mirrors SymbolTable's
     *        per-document lifecycle (clear-then-recollect on every didOpen/didChange/didSave) but,
     *        unlike SymbolTable's flat qualifiedName map, keys directly by file URI since a scope
     *        tree only ever describes a single file.
     */
    class ScopeIndex
    {
    public:
        ScopeIndex() = default;

        /** @brief Replaces (or sets for the first time) the scope tree for fileUri. */
        void SetScopeTree(const std::string &fileUri, std::unique_ptr<Scope> root);

        /** @brief Discards the scope tree for fileUri, if any. */
        void ClearDocument(const std::string &fileUri);

        /**
         * @brief Returns a snapshot of the root scope for fileUri, or nullptr if none is recorded.
         *        The returned handle keeps the tree alive even if a concurrent SetScopeTree/
         *        ClearDocument replaces or removes the index's own entry for fileUri afterward.
         */
        std::shared_ptr<const Scope> GetRoot(const std::string &fileUri) const;

    private:
        mutable std::shared_mutex m_mutex;
        ankerl::unordered_dense::map<std::string, std::shared_ptr<Scope>> m_roots;
    };
}
