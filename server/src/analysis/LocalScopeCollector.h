#pragma once

#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"
#include "utils/LspLogger.h"

#include <tree_sitter/api.h>
#include <memory>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
    /**
     * @brief Walks a parsed AngelScript tree-sitter AST and builds a lexical Scope tree from
     *        LOCALS_QUERY matches (nested scopes, definitions, and not-yet-resolved references).
     *
     * LocalScopeCollector is a Layer 2 (Analysis) component, same constraints as SymbolCollector:
     * depends only on Layer 1 and the standard library, holds no static/global state, and never
     * throws. It produces a Scope tree, not a SymbolTable - see ScopeTree.h for why the two are
     * kept separate. This collector only builds the tree; it does not resolve references or emit
     * diagnostics.
     */
    class LocalScopeCollector
    {
    public:
        /** @brief Constructs the collector and pre-compiles LOCALS_QUERY. */
        explicit LocalScopeCollector(angel_lsp::utils::LspLogger *logger = nullptr);

        /** @brief Releases the compiled LOCALS_QUERY. */
        ~LocalScopeCollector();

        /**
         * @brief Parses sourceCode and builds its Scope tree.
         * @param sourceCode Full text of the document.
         * @param parser Tree-sitter parser used to produce the AST (and immediately discarded).
         * @return The document's root Scope, or nullptr if parsing failed.
         */
        std::unique_ptr<Scope> CollectScopes(const std::string &sourceCode, angel_lsp::parser::AngelScriptParser &parser) const;

        /**
         * @brief Builds a Scope tree from an already-parsed tree, without owning or freeing it.
         * @param rootNode Root node of a pre-parsed tree-sitter tree; the caller retains ownership.
         * @see CollectScopes for sourceCode.
         */
        std::unique_ptr<Scope> CollectScopesFromTree(TSNode rootNode, const std::string &sourceCode) const;

    private:
        utils::LspLogger *m_logger;
        TSQuery *m_localsQuery;

        /** @brief Cached grammar symbol for member_expression, used to flag a reference as a member access (see ScopeTree.h::LocalReference::isMemberAccess). */
        TSSymbol m_symMemberExpression = 0;

        /** @brief Cached grammar symbols used to set Scope::isFunctionScope (see ScopeTree.h). */
        TSSymbol m_symFuncDeclaration = 0;
        TSSymbol m_symLambdaExpression = 0;

        /** @brief Cached grammar symbol used to confirm a Variable-kind definition came from a
         *         variable_declarator (as opposed to a foreach_variable, which shares
         *         LocalDefinitionKind::Variable but has no declared-type/initializer node),
         *         before reading LocalDefinition::isHandleType/hasNullInitializer from it. */
        TSSymbol m_symVariableDeclarator = 0;

        /** @brief What a LOCALS_QUERY capture index means, resolved once in the constructor by capture name. */
        enum class CaptureKind
        {
            None,
            Scope,
            Definition,
            Reference
        };

        /** @brief Maps each LOCALS_QUERY capture index to its CaptureKind, built once in the constructor. */
        std::vector<CaptureKind> m_captureKinds;

        /** @brief Maps each LOCALS_QUERY capture index to a LocalDefinitionKind; only meaningful where m_captureKinds[i] == Definition. */
        std::vector<LocalDefinitionKind> m_definitionKinds;

        /** @brief One flattened LOCALS_QUERY match, before matches are sorted and nested into a Scope tree. */
        struct RawCapture
        {
            TSNode node;
            CaptureKind kind;
            LocalDefinitionKind definitionKind;
        };

        /**
         * @brief Nests a byte-order-sorted capture list into a Scope tree: a Scope capture pushes a
         *        new child scope, a definition/reference attaches to the innermost currently-open
         *        scope. See LocalScopeCollector.cpp for the full algorithm description.
         */
        std::unique_ptr<Scope> BuildScopeTree(std::vector<RawCapture> &captures, const std::string &sourceCode) const;

        /** @brief Returns the source text spanned by node, or an empty string for a null/degenerate node. */
        std::string GetNodeText(TSNode node, const std::string &sourceCode) const;

        /**
         * @brief If nameNode is a variable_declarator's "name" (not a foreach_variable, which has
         *        no declared-type node), fills def.isHandleType/hasNullInitializer from the
         *        enclosing variable_declaration's "var_type" field and the declarator's
         *        initializer. Leaves both fields at their default (false) otherwise.
         */
        void ReadVariableTypeInfo(TSNode nameNode, const std::string &sourceCode, LocalDefinition &def) const;
    };
}
