#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarSymbols.h"
#include "utils/Constants.h"
#include "utils/LspLogger.h"

#include <memory>
#include <optional>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Walks a parsed AngelScript tree-sitter AST and builds a lexical Scope tree from
 *        LOCALS_QUERY matches (nested scopes, definitions, and not-yet-resolved references).
 */
class LocalScopeCollector
{
  public:
    /**
     * @brief Constructs the collector and pre-compiles LOCALS_QUERY.
     */
    explicit LocalScopeCollector(angel_lsp::utils::LspLogger* logger = nullptr);

    /**
     * @brief Releases the compiled LOCALS_QUERY.
     */
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
    std::unique_ptr<Scope> CollectScopesFromTree(TSNode rootNode, const std::string& sourceCode,
                                                 const NodeIndex* nodeIndex = nullptr) const;

    /**
     * @brief Builds a Scope tree from a precomputed NodeIndex without exposing raw AST nodes.
     * @param nodeIndex Precomputed node index of the document.
     * @param sourceCode Full text of the document.
     * @return The document's root Scope, or nullptr on failure.
     */
    std::unique_ptr<Scope> CollectScopesFromIndex(const NodeIndex& nodeIndex,
                                                  const std::string& sourceCode) const;

    struct ByteSpan
    {
        uint32_t startByte = 0;
        uint32_t endByte = 0;
    };

    struct DocumentContext
    {
        bool treeHasError = false;
        bool hasMetadata = false;
        std::vector<ByteSpan> metadataOrErrorSpans;
        std::shared_ptr<IndexedReferenceReceivers> receivers = std::make_shared<IndexedReferenceReceivers>();
        ankerl::unordered_dense::map<uint64_t, uint32_t> receiverIds;

        [[nodiscard]] bool Contains(uint32_t start, uint32_t end) const noexcept
        {
            if (metadataOrErrorSpans.empty())
            {
                return false;
            }
            auto it = std::lower_bound(metadataOrErrorSpans.begin(), metadataOrErrorSpans.end(), start,
                                       [](const ByteSpan& span, uint32_t val) { return span.endByte < val; });
            return it != metadataOrErrorSpans.end() && it->startByte <= start && end <= it->endByte;
        }
    };

    /**
     * @brief Tracks an open lexical scope, its enclosing parent, and boundary byte offsets during scope hierarchy
     *        construction.
     */
    struct OpenScope
    {
        Scope* scope = nullptr;
        uint32_t endByte = 0;
        uint32_t ownNameStartByte = constants::InvalidByteOffset;
        uint32_t ownNameEndByte = 0;
    };

  private:
    utils::LspLogger* m_logger;
    const TSQuery* m_localsQuery;

    const parser::GrammarSymbols& m_gs;

    /**
     * @brief What a LOCALS_QUERY capture index means, resolved once in the constructor by capture name.
     */
    enum class CaptureKind
    {
        None,
        Scope,
        Definition,
        Reference,
        StringCallRef
    };

    /**
     * @brief Maps each LOCALS_QUERY capture index to its CaptureKind, built once in the constructor.
     */
    std::vector<CaptureKind> m_captureKinds;

    /**
     * @brief Maps each LOCALS_QUERY capture index to a LocalDefinitionKind; only meaningful where m_captureKinds[i] ==
     *        Definition.
     */
    std::vector<LocalDefinitionKind> m_definitionKinds;

    /**
     * @brief One flattened LOCALS_QUERY match, before matches are sorted and nested into a Scope tree.
     */
    struct RawCapture
    {
        TSNode node;
        TSNode parent;
        TSNode grandparent;
        uint32_t startByte = 0;
        uint32_t endByte = 0;
        CaptureKind kind = CaptureKind::None;
        LocalDefinitionKind definitionKind = LocalDefinitionKind::Variable;
        TSSymbol enclosingCallableSym = 0;
    };

    /**
     * @brief Nests a byte-order-sorted capture list into a Scope tree.
     * @param captures Raw captures extracted by LOCALS_QUERY.
     * @param sourceCode Source text of the document.
     * @param docCtx Pre-collected document context for error and metadata spans.
     * @return Unique pointer to the constructed root Scope, or nullptr on failure.
     */
    std::unique_ptr<Scope> BuildScopeTree(std::vector<RawCapture>& captures, const std::string& sourceCode,
                                          DocumentContext& docCtx) const;

    /**
     * @brief Executes the locals query and collects raw capture records.
     * @param rootNode Root AST node of the document.
     * @param[out] captures Destination vector for raw captures.
     */
    void CollectRawCaptures(TSNode rootNode, std::vector<RawCapture>& captures) const;

    /**
     * @brief Reads declared type and initializer information for variable-like definitions.
     * @param capture Raw capture containing node, parent, and grandparent AST nodes.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving extracted type info.
     */
    void ReadVariableTypeInfo(const RawCapture& capture, const std::string& sourceCode, LocalDefinition& def) const;

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

    /**
     * @brief Processes a scope-opening capture node.
     * @param capture Raw capture representing the scope.
     * @param[in,out] stack Open scope tracking stack.
     * @param[in,out] root Root scope pointer.
     * @param sourceCode Source text of the document.
     */
    void ProcessScopeCapture(const RawCapture& capture, std::vector<OpenScope>& stack, std::unique_ptr<Scope>& root,
                             const std::string& sourceCode) const;

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
     * @param docCtx Pre-collected document context for error and metadata spans.
     */
    void ProcessReferenceCapture(const RawCapture& capture, Scope* current, const std::string& sourceCode,
                                 DocumentContext& docCtx) const;

    /**
     * @brief Processes a string literal call reference capture node.
     * @param capture Raw capture representing the string literal.
     * @param[in,out] current Innermost open scope receiving the string call reference.
     * @param sourceCode Source text of the document.
     */
    void ProcessStringCallRefCapture(const RawCapture& capture, Scope* current, const std::string& sourceCode) const;

    /**
     * @brief Recovers local variable declarations from broken control flow or error nodes.
     * @param rootNode Pre-parsed Tree-Sitter root node.
     * @param rootScope Root lexical scope.
     * @param sourceCode Source text of the document.
     */
    void RecoverDeclarationsFromErrorNodes(TSNode rootNode, Scope* rootScope, const std::string& sourceCode) const;

    /**
     * @brief Registers implicit 'value' parameter in virtual property 'set' accessor scopes.
     * @param rootNode Pre-parsed Tree-Sitter root node.
     * @param rootScope Root lexical scope.
     * @param sourceCode Source text of the document.
     */
    void RegisterVirtualPropertySetParameters(const NodeIndex& nodeIndex, Scope* rootScope,
                                              const std::string& sourceCode) const;

    /**
     * @brief Enriches reference metadata with lambda, constructor, and error context.
     * @param[in] captureNode AST node of the captured reference.
     * @param[in] parent Parent AST node.
     * @param[in] sourceCode Source text of the document.
     * @param[in,out] ref Reference to update.
     * @param[in] docCtx Document error and metadata context.
     */
    void EnrichReferenceContext(TSNode captureNode, TSNode parent, std::string_view sourceCode, LocalReference& ref,
                                const DocumentContext& docCtx) const;

    /**
     * @brief Ancestor nodes and enclosing symbol cached during capture processing.
     */
    struct CaptureAncestors
    {
        TSNode parent = TSNode{};
        TSNode grandparent = TSNode{};
        TSSymbol enclosingCallableSym = 0;
    };

    /**
     * @brief Resolves parent, grandparent, and callable ancestor context for a captured node.
     * @param kind CaptureKind classification of the node.
     * @param defKind LocalDefinitionKind if applicable.
     * @param node AST node being captured.
     * @return Resolved CaptureAncestors bundle.
     */
    CaptureAncestors ResolveCaptureAncestors(CaptureKind kind, LocalDefinitionKind defKind, TSNode node) const;

    /**
     * @brief Checks if a grammar symbol belongs to a declaration whose name is a definition rather than a reference.
     * @param sym Grammar symbol of parent node.
     * @return True if parent node is a declaration.
     */
    [[nodiscard]] bool IsDeclarationContextSymbol(TSSymbol sym) const noexcept;

    /**
     * @brief Resolves call expression and argument details for a reference in O(1).
     * @param refNode AST node of the reference.
     * @param parent AST parent node.
     * @param grandparent AST grandparent node.
     * @param[out] ref Reference struct to update.
     */
    static void DetermineCallReferenceInfo(TSNode refNode, TSNode parent, TSNode grandparent, LocalReference& ref);

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
     * @param declarationNode Enclosing variable declaration AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type info.
     */
    void ReadVariableDeclaratorTypeInfo(TSNode declaratorNode, TSNode declarationNode, const std::string& sourceCode,
                                        LocalDefinition& def) const;

    /**
     * @brief Reads declared type information for a virtual property AST node.
     * @param propertyNode Virtual property AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type info.
     */
    void ReadVirtualPropertyTypeInfo(TSNode propertyNode, const std::string& sourceCode, LocalDefinition& def) const;

    /**
     * @brief Reads declared type information for an empty-parentheses func declaration.
     * @param funcNode Function AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type info.
     */
    void ReadEmptyParenVariableTypeInfo(TSNode funcNode, const std::string& sourceCode, LocalDefinition& def) const;

    /**
     * @brief Populates type metadata and full source range from a named child field.
     * @param[in] parentNode Parent AST node.
     * @param[in] fieldSymbol Grammar field symbol for the type child.
     * @param[in] sourceCode Source text of the document.
     * @param[in,out] def LocalDefinition receiving type info.
     */
    void PopulateVariableFromField(TSNode parentNode, TSSymbol fieldSymbol, const std::string& sourceCode,
                                   LocalDefinition& def) const;

    /**
     * @brief Populates type span coordinates and template argument positions.
     * @param tNode Type AST node.
     * @param sourceCode Source text of the document.
     * @param[out] def LocalDefinition receiving type ranges.
     */
    void PopulateTypeRanges(TSNode tNode, const std::string& sourceCode, LocalDefinition& def) const;
};

/**
 * @brief Context for synthesizing implicit 'value' parameters within virtual property accessor bodies.
 */
struct VirtualPropertyContext
{
    const std::string& propTypeStr;
    const TypeExtractionResult& typeInfo;
    Scope* root = nullptr;
    const parser::GrammarSymbols& gs;
    std::string_view sourceCode;
};
} // namespace angel_lsp::analysis
