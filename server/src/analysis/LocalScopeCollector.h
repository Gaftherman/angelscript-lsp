#pragma once

#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

#include <memory>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

// Forward-declared to keep lexical scope analysis free of protocol message headers from LspLogger.h.
namespace angel_lsp::utils
{
class LspLogger;
}

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
    explicit LocalScopeCollector(angel_lsp::utils::LspLogger* logger = nullptr);

    /** @brief Releases the compiled LOCALS_QUERY. */
    ~LocalScopeCollector();

    /**
     * @brief Parses sourceCode and builds its Scope tree.
     * @param sourceCode Full text of the document.
     * @param parser Tree-sitter parser used to produce the AST (and immediately discarded).
     * @return The document's root Scope, or nullptr if parsing failed.
     */
    std::unique_ptr<Scope> CollectScopes(const std::string& sourceCode,
                                         angel_lsp::parser::AngelScriptParser& parser) const;

    /**
     * @brief Builds a Scope tree from an already-parsed tree, without owning or freeing it.
     * @param rootNode Root node of a pre-parsed tree-sitter tree; the caller retains ownership.
     * @see CollectScopes for sourceCode.
     */
    std::unique_ptr<Scope> CollectScopesFromTree(TSNode rootNode, const std::string& sourceCode) const;

  private:
    utils::LspLogger* m_logger;
    const TSQuery* m_localsQuery;

    /** @brief Cached grammar symbol for member_expression, used to flag a reference as a member access (see
     * ScopeTree.h::LocalReference::isMemberAccess). */
    TSSymbol m_symMemberExpression = 0;

    /** @brief Cached grammar symbols used to set Scope::kind and Scope::isFunctionScope (see ScopeTree.h). */
    TSSymbol m_symFuncDeclaration = 0;
    TSSymbol m_symLambdaExpression = 0;
    TSSymbol m_symClassBody = 0;
    TSSymbol m_symInterfaceBody = 0;
    TSSymbol m_symNamespaceBody = 0;
    TSSymbol m_symScript = 0;

    /** @brief Cached grammar symbol used to confirm a Variable-kind definition came from a
     *         variable_declarator (as opposed to a foreach_variable, which shares
     *         LocalDefinitionKind::Variable but has no declared-type/initializer node),
     *         before reading LocalDefinition::isHandleType/hasNullInitializer from it. */
    TSSymbol m_symVariableDeclarator = 0;
    TSSymbol m_symParameter = 0;
    TSSymbol m_symForeachVariable = 0;
    TSSymbol m_symLambdaParameterList = 0;

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

    /** @brief Maps each LOCALS_QUERY capture index to a LocalDefinitionKind; only meaningful where m_captureKinds[i] ==
     * Definition. */
    std::vector<LocalDefinitionKind> m_definitionKinds;

    /** @brief One flattened LOCALS_QUERY match, before matches are sorted and nested into a Scope tree. */
    struct RawCapture
    {
        TSNode node;
        CaptureKind kind;
        LocalDefinitionKind definitionKind;
    };

    /**
     * @brief Nests a byte-order-sorted capture list into a Scope tree.
     * @param captures Raw captures extracted by LOCALS_QUERY.
     * @param sourceCode Source text of the document.
     * @return Unique pointer to the constructed root Scope, or nullptr on failure.
     */
    std::unique_ptr<Scope> BuildScopeTree(std::vector<RawCapture>& captures, const std::string& sourceCode) const;

    /** @brief Returns the source text spanned by node, or an empty string for a null/degenerate node. */
    std::string GetNodeText(TSNode node, const std::string& sourceCode) const;

    /**
     * @brief Reads declared type and initializer information for variable-like definitions.
     * @param nameNode AST node representing the definition name.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving extracted type info.
     */
    void ReadVariableTypeInfo(TSNode nameNode, const std::string& sourceCode, LocalDefinition& def) const;

    /**
     * @brief Initializes capture classifications from the compiled LOCALS_QUERY.
     * @param[out] kinds Output vector of capture kinds.
     * @param[out] defKinds Output vector of definition kinds.
     */
    void InitializeCaptureKinds(std::vector<CaptureKind>& kinds, std::vector<LocalDefinitionKind>& defKinds);

    /**
     * @brief Deduplicates overlapping capture matches, preferring more specific kinds.
     * @param[in,out] captures Vector of raw captures to deduplicate in place.
     */
    static void DeduplicateCaptures(std::vector<RawCapture>& captures);

    struct OpenScope;

    /**
     * @brief Processes a scope-opening capture node.
     * @param capture Raw capture representing the scope.
     * @param[in,out] stack Open scope tracking stack.
     * @param[in,out] root Root scope pointer.
     */
    void ProcessScopeCapture(const RawCapture& capture, std::vector<OpenScope>& stack,
                             std::unique_ptr<Scope>& root) const;

    /**
     * @brief Processes a definition capture node.
     * @param capture Raw capture representing the definition.
     * @param[in,out] stack Open scope tracking stack.
     * @param sourceCode Source text of the document.
     */
    void ProcessDefinitionCapture(const RawCapture& capture, const std::vector<OpenScope>& stack,
                                  const std::string& sourceCode) const;

    /**
     * @brief Processes a reference capture node.
     * @param capture Raw capture representing the reference.
     * @param[in,out] current Innermost open scope receiving the reference.
     * @param sourceCode Source text of the document.
     */
    void ProcessReferenceCapture(const RawCapture& capture, Scope* current, const std::string& sourceCode) const;

    /**
     * @brief Resolves call expression and argument details for a reference using a flat cursor.
     * @param refNode AST node of the reference.
     * @param parent AST parent node.
     * @param[out] ref Reference struct to update.
     */
    static void DetermineCallReferenceInfo(TSNode refNode, TSNode parent, LocalReference& ref);

    /**
     * @brief Reads type information and ranges for a parameter AST node.
     * @param declaratorNode Parameter AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type info.
     */
    void ReadParameterTypeInfo(TSNode declaratorNode, const std::string& sourceCode, LocalDefinition& def) const;
 
    /**
     * @brief Reads declared type information for lambda parameters.
     * @param[in] nameNode AST node of the lambda parameter identifier.
     * @param[in] declaratorNode AST node of the enclosing lambda_parameter_list.
     * @param[in] sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving extracted type info.
     */
    void ReadLambdaParameterTypeInfo(TSNode nameNode, TSNode declaratorNode, const std::string& sourceCode,
                                     LocalDefinition& def) const;

    /**
     * @brief Reads type information and ranges for a foreach loop variable AST node.
     * @param declaratorNode Foreach variable AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type info.
     */
    void ReadForeachVariableTypeInfo(TSNode declaratorNode, const std::string& sourceCode, LocalDefinition& def) const;

    /**
     * @brief Reads type and initializer details for a variable declarator AST node.
     * @param declaratorNode Variable declarator AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type info.
     */
    void ReadVariableDeclaratorTypeInfo(TSNode declaratorNode, const std::string& sourceCode,
                                        LocalDefinition& def) const;

    /**
     * @brief Populates type span coordinates and template argument positions.
     * @param tNode Type AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type ranges.
     */
    void PopulateTypeRanges(TSNode tNode, const std::string& sourceCode, LocalDefinition& def) const;
};
} // namespace angel_lsp::analysis
