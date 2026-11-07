#pragma once

#include "analysis/index/SymbolTable.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Half-open byte range within the document buffer.
 */
struct IndexedByteRange
{
    uint32_t startByte = 0; ///< Starting byte offset within the document.
    uint32_t endByte = 0;   ///< Non-inclusive ending byte offset within the document.

    [[nodiscard]] constexpr bool Empty() const noexcept
    {
        return startByte >= endByte;
    }
};

/**
 * @brief Information about an enclosing container in the AST hierarchy.
 */
struct ContainerInfo
{
    std::string name;          ///< Bare name, e.g. "Player" or "Game"
    std::string qualifiedName; ///< Fully qualified path, e.g. "Game::Player" or "Game"
    ContainerKind kind;
};

/**
 * @brief Classifies a Tree-Sitter AST symbol into an AST ContainerKind.
 * @param[in] sym Tree-Sitter grammar symbol to classify.
 * @return ContainerKind if symbol represents a container (class, interface, namespace); std::nullopt otherwise.
 */
[[nodiscard]] std::optional<ContainerKind> ClassifyContainerKind(TSSymbol sym) noexcept;

/**
 * @brief Extracts raw container names and qualified segments from a container name node.
 * @param[in] nameNode AST node containing the container identifier or scoped identifier.
 * @param[in] kind The container classification kind.
 * @param[in] sourceCode The source code buffer.
 * @param[in,out] rawContainers Destination collection for extracted container definitions.
 */
void AppendRawContainers(TSNode nameNode, ContainerKind kind, std::string_view sourceCode,
                         std::vector<ContainerInfo>& rawContainers);

/**
 * @brief Computes fully qualified scope names for a hierarchically ordered container stack.
 * @param[in,out] containers In-place ordered stack of enclosing containers.
 */
void ComputeContainerQualifiedNames(std::vector<ContainerInfo>& containers);

/**
 * @brief Precalculated byte interval and metadata for an AST container declaration.
 */
struct IndexedContainerSpan
{
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    uint32_t nameStartByte = 0;
    uint32_t nameEndByte = 0;
    uint32_t parentSpanIndex = UINT32_MAX;
    TSSymbol symbol : 15 = 0;
    uint16_t isMixin : 1 = 0;
};
static_assert(sizeof(IndexedContainerSpan) <= 24);

enum class IndexedFlowFindingKind : uint8_t
{
    Unreachable,
    VariableInCase,
    DefaultNotLast
};

struct IndexedFlowFinding
{
    SourceRange range;
    IndexedFlowFindingKind kind;
};

/// A document-local key into the extracted syntax links for this revision.
using SyntaxNodeId = size_t;
inline constexpr SyntaxNodeId NoSyntaxNode = SIZE_MAX;

/// A syntax fact whose child and sibling links are local to this extraction.
struct IndexedSyntaxNode
{
    size_t syntaxSlot = SIZE_MAX; ///< Index into the symbol bucket or the transient syntax side table.
    SyntaxNodeId first = NoSyntaxNode;
    SyntaxNodeId next = NoSyntaxNode;
    TSSymbol symbol = 0;
    TSFieldId field = 0;
    TSSymbol parentSymbol = 0;
    bool named = false;
    bool afterAssign = false;
    bool statement = false;
    bool defaultClause = false;
    bool inSymbolBucket = false;
    bool hasReturnReferenceToken = false;
};
static_assert(sizeof(IndexedSyntaxNode) <= 40);

enum class NodeIndexContents : uint8_t
{
    Full,
    Declarations
};

enum class IndexedDeclarationKind : uint8_t
{
    None,
    Function,
    Class,
    Interface,
    Namespace,
    Enum,
    Typedef,
    Funcdef,
    Property,
    Variable,
    Lambda,
    Call,
    Using
};

/**
 * @brief Precalculated range and parent index for hierarchical selection range lookup.
 */
struct IndexedSelectionRange
{
    SourceRange range{};
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    uint32_t parentSpanIndex = UINT32_MAX;
};
static_assert(sizeof(IndexedSelectionRange) <= 32);

/**
 * @brief Classification of foldable document intervals.
 */
enum class IndexedFoldableKind : uint8_t
{
    None,
    Comment,
    Imports,
    Region
};

/**
 * @brief Precalculated source coordinate interval eligible for folding.
 */
struct IndexedFoldableSpan
{
    SourceRange range{};
    IndexedFoldableKind kind = IndexedFoldableKind::None;
};

/**
 * @brief Pre-extracted #include directive facts captured during pass 1.
 */
struct IndexedIncludeDirective
{
    std::string rawPath;
    uint32_t line = 0;
    uint32_t spanStart = 0;
    uint32_t spanEnd = 0;
    bool isAngled = false;
};

/**
 * @brief Configuration options bundle for building a NodeIndex.
 */
struct NodeIndexOptions
{
    const TSLanguage* lang = nullptr;
    TraversalBudget* budget = nullptr;
    NodeIndexContents contents = NodeIndexContents::Full;
    std::string_view source{};
};

/**
 * @brief Single-pass index of AST nodes grouped by Tree-Sitter symbol for O(1) retrieval.
 *        Traverses an AST once in preorder, bucketing nodes by their Tree-Sitter TSSymbol id
 */
class NodeIndex
{
  public:
    NodeIndex() = default;

    /**
     * @brief Constructs a NodeIndex and builds it from the given root node and optional source.
     * @param root The root AST node of the document.
     * @param source Optional source code buffer for include directives and folding hints.
     */
    explicit NodeIndex(TSNode root, std::string_view source = {});

    /**
     * @brief Constructs a NodeIndex with configuration options.
     * @param root The root AST node of the document.
     * @param options Build options bundle.
     */
    explicit NodeIndex(TSNode root, const NodeIndexOptions& options);

    /**
     * @brief Constructs a NodeIndex and builds it with language, budget, and content mode.
     * @param root The root AST node of the document.
     * @param lang The language definition (defaults to tree_sitter_angelscript() if null).
     * @param budget Optional traversal budget to track and enforce node visits.
     * @param contents Node index contents mode.
     */
    explicit NodeIndex(TSNode root, const TSLanguage* lang, TraversalBudget* budget,
                       NodeIndexContents contents = NodeIndexContents::Full);

    ~NodeIndex() = default;
    NodeIndex(const NodeIndex&) = default;
    NodeIndex& operator=(const NodeIndex&) = default;
    NodeIndex(NodeIndex&&) noexcept = default;
    NodeIndex& operator=(NodeIndex&&) noexcept = default;

    /**
     * @brief Builds or rebuilds the index from an AST root node and optional source.
     * @param root The root node of the syntax tree.
     * @param source Optional source text buffer.
     */
    void Build(TSNode root, std::string_view source = {});

    /**
     * @brief Builds the index with full configuration options.
     * @param root The root node of the syntax tree.
     * @param options Build options bundle.
     */
    void Build(TSNode root, const NodeIndexOptions& options);

    /**
     * @brief Builds or rebuilds the index from an AST root node.
     * @param root The root node of the syntax tree.
     * @param lang The language definition (defaults to tree_sitter_angelscript() if null).
     * @param budget Optional traversal budget to track and enforce node visits.
     * @param contents Content mode for indexing.
     */
    void Build(TSNode root, const TSLanguage* lang, TraversalBudget* budget,
               NodeIndexContents contents = NodeIndexContents::Full);

    /// Visits declaration and validation candidates in cursor order using the stored symbol slots.
    template <typename Visitor> void ForEachDeclaration(Visitor&& visitor) const
    {
        if (m_flowFacts)
        {
            for (const auto& entry : m_flowFacts->declarations)
            {
                visitor(m_nodesBySymbol[entry.symbol][entry.slot], entry.kind);
            }
        }
    }

    /**
     * @brief Returns all nodes of a given grammar symbol id in document (preorder) order.
     * @param symbol The Tree-Sitter TSSymbol id.
     * @return Span of matching nodes. Empty if symbol is out of bounds or has no occurrences.
     */
    [[nodiscard]] std::span<const TSNode> Nodes(TSSymbol symbol) const noexcept;

    /**
     * @brief Returns all nodes of a given node type name in document order.
     * @param typeName The grammar node type name (e.g. "call_expression").
     * @return Span of matching nodes. Empty if type name has no occurrences.
     */
    [[nodiscard]] std::span<const TSNode> Nodes(std::string_view typeName) const noexcept;

    /**
     * @brief Iterates multiple node types in document order (sorted by start byte).
     * @tparam Callback Visitor callable with signature void(TSNode).
     * @param[in] types List of Tree-Sitter node type names to merge.
     * @param[in] callback Visitor invoked for each node in sorted order.
     */
    template <typename Callback>
    void ForEachNodeOrdered(std::span<const std::string_view> types, Callback&& callback) const
    {
        struct Cursor
        {
            std::span<const TSNode> nodes;
            size_t index = 0;
            [[nodiscard]] uint32_t currentByte() const noexcept
            {
                return (index < nodes.size()) ? ts_node_start_byte(nodes[index]) : UINT32_MAX;
            }
        };

        std::vector<Cursor> cursors;
        cursors.reserve(types.size());
        for (std::string_view typeName : types)
        {
            cursors.push_back(Cursor{Nodes(typeName), 0});
        }

        while (true)
        {
            size_t best = 0;
            uint32_t minByte = UINT32_MAX;
            for (size_t c = 0; c < cursors.size(); ++c)
            {
                uint32_t b = cursors[c].currentByte();
                if (b < minByte)
                {
                    minByte = b;
                    best = c;
                }
            }
            if (minByte == UINT32_MAX)
            {
                break;
            }

            TSNode node = cursors[best].nodes[cursors[best].index++];
            callback(node);
        }
    }

    /**
     * @brief Iterates multiple grammar symbols in document order (sorted by start byte).
     * @deprecated Second-pass code must consume AST-free facts; remaining callers are pass-1 collectors and folders not
     * yet migrated (Rule 4.11).
     * @tparam Callback Visitor callable with signature void(TSNode).
     * @param[in] symbols List of Tree-Sitter grammar symbol IDs to merge.
     * @param[in] callback Visitor invoked for each node in sorted order.
     */
    template <typename Callback> void ForEachNodeOrdered(std::span<const TSSymbol> symbols, Callback&& callback) const
    {
        struct Cursor
        {
            std::span<const TSNode> nodes;
            size_t index = 0;
            [[nodiscard]] uint32_t currentByte() const noexcept
            {
                return (index < nodes.size()) ? ts_node_start_byte(nodes[index]) : UINT32_MAX;
            }
        };

        std::vector<Cursor> cursors;
        cursors.reserve(symbols.size());
        for (TSSymbol sym : symbols)
        {
            cursors.push_back(Cursor{Nodes(sym), 0});
        }

        while (true)
        {
            size_t best = 0;
            uint32_t minByte = UINT32_MAX;
            for (size_t c = 0; c < cursors.size(); ++c)
            {
                uint32_t b = cursors[c].currentByte();
                if (b < minByte)
                {
                    minByte = b;
                    best = c;
                }
            }
            if (minByte == UINT32_MAX)
            {
                break;
            }

            TSNode node = cursors[best].nodes[cursors[best].index++];
            callback(node);
        }
    }

    /**
     * @brief Iterates over multiple node types in document order (initializer_list overload).
     */
    template <typename Callback>
    void ForEachNodeOrdered(std::initializer_list<std::string_view> types, Callback&& callback) const
    {
        ForEachNodeOrdered(std::span<const std::string_view>(types.begin(), types.size()),
                           std::forward<Callback>(callback));
    }

    /**
     * @brief Resolves a node type name to its TSSymbol id.
     * @param typeName The grammar node type name.
     * @return The TSSymbol id, or 0 if unknown.
     */
    [[nodiscard]] TSSymbol SymbolForName(std::string_view typeName) const noexcept;

    /**
     * @brief Process-wide static mapping from grammar symbol name to TSSymbol ID.
     */
    [[nodiscard]] static const ankerl::unordered_dense::map<std::string_view, TSSymbol>& GetSymbolNameMap() noexcept;

    /**
     * @brief Returns the root node the index was built from.
     * @deprecated Second-pass code must consume AST-free facts; remaining callers are pass-1 collectors and folders not
     * yet migrated (Rule 4.11).
     */
    [[nodiscard]] TSNode Root() const noexcept;

    /**
     * @brief Returns the total count of indexed nodes.
     */
    [[nodiscard]] size_t NodeCount() const noexcept;

    /**
     * @brief True if index is empty.
     */
    [[nodiscard]] bool Empty() const noexcept;

    /**
     * @brief Estimates total dynamic and structure memory used by this index in bytes.
     */
    [[nodiscard]] size_t EstimateMemoryBytes() const noexcept
    {
        return sizeof(NodeIndex) + m_errorNodes.capacity() * sizeof(TSNode) +
               m_containerSpans.capacity() * sizeof(IndexedContainerSpan) +
               m_errorRanges.capacity() * sizeof(ErrorRange) + m_commentRanges.capacity() * sizeof(CommentRange);
    }

    /**
     * @brief Half-open byte range [startByte, endByte) containing a syntax error.
     */
    struct ErrorRange
    {
        uint32_t startByte = 0;
        uint32_t endByte = 0;
    };

    /**
     * @brief Half-open byte range [startByte, endByte) containing a comment node.
     */
    struct CommentRange
    {
        uint32_t startByte = 0;
        uint32_t endByte = 0;
        uint32_t startLine = 0;
        uint32_t startColumn = 0;
        uint32_t endLine = 0;
        uint32_t endColumn = 0;
    };

    /**
     * @brief Checks in O(log E) whether the given byte offset falls inside any syntax error node.
     * @param[in] byteOffset Byte offset to test.
     * @return True if offset is inside a syntax error range; false otherwise (O(1) if no errors).
     */
    [[nodiscard]] bool IsInsideError(uint32_t byteOffset) const noexcept;

    /**
     * @brief Checks in O(log C) whether the given byte offset falls inside any comment node.
     * @param[in] byteOffset Byte offset to test.
     * @return True if offset is inside a comment range; false otherwise (O(1) if no comments).
     */
    [[nodiscard]] bool IsInsideComment(uint32_t byteOffset) const noexcept;

    /**
     * @brief Checks in O(log C) whether the given line and column fall inside any comment node.
     * @param[in] line 0-based line index.
     * @param[in] col 0-based column index.
     * @return True if position is inside a comment range; false otherwise.
     */
    [[nodiscard]] bool IsInsideCommentLineCol(uint32_t line, uint32_t col) const noexcept;

    /**
     * @brief Checks whether the given node is an error, has errors, or is enclosed in a syntax error.
     * @param[in] node AST node to test.
     * @return True if node is or is inside an error; false otherwise.
     */
    [[nodiscard]] bool NodeHasSyntaxError(TSNode node) const noexcept;

    /**
     * @brief Returns all tracked disjoint syntax error byte ranges.
     * @return Span of error ranges.
     */
    [[nodiscard]] std::span<const ErrorRange> ErrorRanges() const noexcept;

    /**
     * @brief Returns all tracked disjoint comment byte ranges.
     * @return Span of comment ranges.
     */
    [[nodiscard]] std::span<const CommentRange> CommentRanges() const noexcept;

    /**
     * @brief Finds the innermost class, interface, or mixin declaration enclosing the given byte offset.
     * @param[in] byteOffset Document byte offset to test.
     * @return Innermost enclosing class/interface/mixin TSNode or null node if outside any class.
     */
    [[nodiscard]] TSNode FindEnclosingClassOrMixin(uint32_t byteOffset) const noexcept;

    /**
     * @brief Finds the innermost class or mixin declaration enclosing byteOffset in O(log K).
     *        If an intermediate lambda closure barrier is encountered, returns nullptr.
     * @param[in] byteOffset Document byte offset to test.
     * @return Pointer to innermost IndexedContainerSpan, or nullptr if none or inside lambda.
     */
    [[nodiscard]] const IndexedContainerSpan* FindInnermostClassSpan(uint32_t byteOffset) const noexcept;

    /**
     * @brief Tests whether byteOffset is enclosed by an intermediate lambda expression closure barrier.
     * @param[in] byteOffset Document byte offset to test.
     * @return True if the innermost callable enclosing byteOffset is a lambda expression.
     */
    [[nodiscard]] bool IsInsideLambdaClosure(uint32_t byteOffset) const noexcept;

    /**
     * @brief Reconstructs the enclosing container hierarchy (namespaces, classes, interfaces) for byteOffset.
     *        Operates in O(K) where K is the number of containers in the document, with zero GetParent calls.
     * @param[in] byteOffset Document byte offset to test.
     * @param[in] sourceCode Document source text buffer.
     * @return Vector of ContainerInfo ordered from innermost to outermost container.
     */
    [[nodiscard]] std::vector<ContainerInfo> CollectEnclosingContainers(uint32_t byteOffset,
                                                                        std::string_view sourceCode) const;

    /**
     * @brief Clears all indexed nodes and symbol maps.
     */
    void Clear() noexcept;

    /// Uses the parent context of container names and base-class clauses when resolving declarations.
    [[nodiscard]] std::vector<ContainerInfo>
    CollectLookupContainers(uint32_t startByte, uint32_t endByte, TSSymbol symbol, std::string_view sourceCode) const;

    /// Lists file imports, or just imports in the enclosing scopes at a byte position, innermost first.
    [[nodiscard]] std::vector<std::string> CollectUsingNamespaces(std::string_view sourceCode,
                                                                  std::optional<uint32_t> byteOffset = {}) const;

    /// Looks up a callable body start byte in the first-pass return facts.
    [[nodiscard]] bool CallableBodyDefinitelyReturns(uint32_t bodyStartByte) const noexcept;

    [[nodiscard]] std::span<const IndexedFlowFinding> FlowFindings() const noexcept;

    /// Returns transient expression handles for semantic checks of a switch's case labels.
    [[nodiscard]] std::span<const TSNode> SwitchLabels(uint32_t switchStartByte) const noexcept;

    /// Returns the function or lambda entry points in source order within that category.
    [[nodiscard]] std::span<const SyntaxNodeId> AssignmentCallables(bool lambdas) const noexcept;
    [[nodiscard]] const IndexedSyntaxNode* SyntaxNode(SyntaxNodeId id) const noexcept;

    /// A name inside a declarator, with the full spelling of its optional scoped identifier.
    struct InitializerIdentifier
    {
        ErrorRange name;
        ErrorRange scopedName{};
    };

    /**
     * @brief Returns candidate unqualified names inside an initializer's byte interval.
     * @param[in] startByte First byte of the initializer.
     * @param[in] endByte Exclusive end of the initializer.
     * @return Source-ordered candidates, found by two binary searches. Candidates with a scoped spelling
     *         still need to be checked for a scope separator in the source.
     */
    [[nodiscard]] std::span<const InitializerIdentifier> InitializerIdentifiers(uint32_t startByte,
                                                                                uint32_t endByte) const noexcept;

    /// Finds captured operands for a syntax site from the same tree.
    [[nodiscard]] const IndexedSyntaxNode* FindOperandSyntax(TSNode node) const noexcept;
    [[nodiscard]] TSNode SyntaxHandle(const IndexedSyntaxNode& node) const noexcept;

    /// Returns the nearest function or lambda for a return, or the nearest class for a function.
    [[nodiscard]] TSNode EnclosingDeclaration(TSNode node) const noexcept;

    /// Reports whether the parsed root has a syntax error anywhere in the document.
    [[nodiscard]] bool HasSyntaxErrors() const noexcept
    {
        return m_hasSyntaxErrors;
    }

    /// Tests a byte position against disjoint outermost if, while and for statement spans.
    [[nodiscard]] bool IsInsideConditionalStatement(uint32_t byteOffset) const noexcept;

    /**
     * @brief Finds enclosing syntactic selection ranges for a cursor position ordered from innermost to outermost.
     * @param[in] line 0-indexed line number.
     * @param[in] character 0-indexed character column.
     * @return Ordered enclosing syntax ranges, or empty if index has no records.
     */
    [[nodiscard]] std::vector<SourceRange> FindEnclosingSelectionRanges(uint32_t line, uint32_t character) const;

    /**
     * @brief Finds enclosing syntactic selection ranges for a document byte offset ordered from innermost to outermost.
     * @param[in] byteOffset Document byte offset.
     * @return Ordered enclosing syntax ranges, or empty if index has no records.
     */
    [[nodiscard]] std::vector<SourceRange> FindEnclosingSelectionRanges(uint32_t byteOffset) const;

    /**
     * @brief Returns pre-computed foldable syntax and comment spans.
     * @return Span of foldable ranges.
     */
    [[nodiscard]] std::span<const IndexedFoldableSpan> FoldableSpans() const noexcept;

    /**
     * @brief Returns pre-computed foldable syntax and comment spans (alias).
     * @return Span of foldable ranges.
     */
    [[nodiscard]] std::span<const IndexedFoldableSpan> GetFoldableSpans() const noexcept
    {
        return FoldableSpans();
    }

    /**
     * @brief Returns pre-extracted include directives.
     * @return Span of include directive facts.
     */
    [[nodiscard]] std::span<const IndexedIncludeDirective> IncludeDirectives() const noexcept;

    /**
     * @brief Returns pre-extracted include directives (alias).
     * @return Span of include directive facts.
     */
    [[nodiscard]] std::span<const IndexedIncludeDirective> GetIncludeDirectives() const noexcept
    {
        return IncludeDirectives();
    }

  private:
    struct IndexedSwitch
    {
        uint32_t startByte = 0;
        std::vector<TSNode> labels;
    };

    struct IndexedUsingName
    {
        ErrorRange name;
        size_t next = SIZE_MAX;
    };

    struct IndexedUsingScope
    {
        uint32_t startByte;
        uint32_t endByte;
        size_t firstName = SIZE_MAX;
        size_t parentSpanIndex = SIZE_MAX;
    };

    struct IndexedDeclaration
    {
        size_t slot;
        TSSymbol symbol;
        IndexedDeclarationKind kind;
    };

    struct FlowFacts
    {
        std::vector<IndexedDeclaration> declarations;
        std::vector<uint32_t> returningBodies;
        std::vector<IndexedFlowFinding> findings;
        std::vector<IndexedSwitch> switches;
        std::vector<ErrorRange> baseClassLists;
        std::vector<IndexedUsingName> usingNames;
        std::vector<IndexedUsingScope> usingScopes;
        std::vector<InitializerIdentifier> initializerIdentifiers;
        std::vector<IndexedSyntaxNode> syntaxNodes;
        std::vector<TSNode> unbucketedSyntax;
        ankerl::unordered_dense::map<std::pair<const void*, uint32_t>, SyntaxNodeId> operandSyntax;
        std::vector<SyntaxNodeId> assignmentFunctions;
        std::vector<SyntaxNodeId> assignmentLambdas;
        std::vector<SyntaxNodeId> classByFunctionSlot;
        std::vector<SyntaxNodeId> callableByReturnSlot;
        std::vector<ErrorRange> conditionalStatements;
        std::vector<IndexedSelectionRange> selectionSpans;
        std::vector<IndexedFoldableSpan> foldableSpans;
        std::vector<IndexedIncludeDirective> includeDirectives;
    };

    FlowFacts& MutableFlowFacts();

    class FlowFactCollector
    {
      public:
        explicit FlowFactCollector(NodeIndex& index);
        void Enter(TSNode node, const TSTreeCursor& cursor, size_t syntaxSlot);
        bool Advance(TSTreeCursor& cursor, bool full);
        void FinishUsingScopes();

      private:
        struct Frame
        {
            size_t bodySlot = SIZE_MAX;
            size_t switchSlot = SIZE_MAX;
            size_t usingScope = SIZE_MAX;
            size_t lastUsingName = SIZE_MAX;
            SyntaxNodeId syntaxId = NoSyntaxNode;
            SyntaxNodeId lastSyntaxChild = NoSyntaxNode;
            SyntaxNodeId enclosingClass = NoSyntaxNode;
            SyntaxNodeId enclosingCallable = NoSyntaxNode;
            SourceRange clauseRange;
            uint32_t namedChildren = 0;
            TSSymbol symbol = 0;
            TSSymbol parentSymbol = 0;
            TSFieldId field = 0;
            bool isNamed = false;
            bool inCallable = false;
            bool inDeclarator = false;
            bool inConditionalStatement = false;
            bool captureOperands = false;
            bool takeInitializer = false;
            bool hasChild = false;
            bool isDefault = false;
            bool isCase = false;
            bool defaultIsLast = true;
            bool checkUnreachable = false;
            bool terminated = false;
            bool returnsFromChild = false;
            bool allChildrenReturn = true;
            bool hasEscapingBreak = false;
            bool hasDefault = false;
            bool defaultReturns = false;
            bool consequenceReturns = false;
            bool alternativeReturns = false;
        };

        const parser::GrammarSymbols& m_grammar = parser::GetGrammarSymbols();
        std::vector<Frame> m_frames;
        std::vector<IndexedUsingScope> m_usingScopes;
        NodeIndex& m_index;

        void Leave();
        void ObserveChild(Frame& parent, const Frame& child, TSNode node);
        void ObserveUnreachable(Frame& parent, TSSymbol symbol, TSNode node);
        void ObserveSwitchChild(const Frame& parent, TSSymbol symbol, TSNode node);
        void CaptureUsingFacts(Frame& frame, TSNode node);
        void CaptureUsingName(TSNode node);
        void CaptureSyntaxNode(Frame& frame, TSNode node, size_t syntaxSlot);
        void CaptureEnclosingContext(Frame& frame, TSNode node);
        void ObserveFunctionReturnToken(const Frame& parent, TSSymbol child);
        void CaptureInitializerIdentifier(Frame& frame, TSNode node);
        [[nodiscard]] bool CapturesOperands(TSSymbol symbol) const noexcept;
        [[nodiscard]] size_t StoreUnbucketedSyntax(TSNode node);
        [[nodiscard]] SyntaxNodeId AddSyntaxFact(Frame& frame, TSNode node, size_t syntaxSlot, bool afterAssign);
        bool ObserveAssignmentChild(Frame& parent, TSSymbol symbol, TSFieldId field = 0);
        void LinkSyntaxChild(Frame& parent, SyntaxNodeId child);
        [[nodiscard]] bool IsAssignmentStatement(TSSymbol symbol) const noexcept;
        [[nodiscard]] bool IsBreakBoundary(TSSymbol symbol) const;
        void AddChild(Frame& parent, const Frame& child, bool returns) const;
        void AddSwitchClause(Frame& parent, const Frame& clause, bool returns) const;
        [[nodiscard]] bool Returns(const Frame& frame) const;
    };

    void PopulatePredefinedSymbols(const TSLanguage* lang);
    size_t IndexNode(TSNode node, TraversalBudget* budget, uint32_t parentSpanIdx, bool full);
    size_t IndexBucketNode(TSNode node, TSSymbol symbol, IndexedDeclarationKind kind);
    void RecordContainerSpan(TSNode node, TSSymbol sym, uint32_t parentSpanIdx = UINT32_MAX);
    void RecordSelectionSpan(TSNode node, std::vector<uint32_t>& activeSelectionSpans);
    void RecordFoldableAndDirectiveSpan(TSNode node, std::string_view source);
    void ExtractIncludeDirective(TSNode node, std::string_view source);
    void TraverseTree(TSNode root, TraversalBudget* budget, bool full, std::string_view source = {});
    void PostProcessErrorRanges();
    void PostProcessCommentRanges();
    [[nodiscard]] bool IsInsideBaseClassList(uint32_t startByte, uint32_t endByte) const noexcept;
    [[nodiscard]] std::vector<ContainerInfo> CollectContainersFrom(const IndexedContainerSpan* span,
                                                                   std::string_view sourceCode) const;

    /**
     * @brief Finds the innermost node matching any of the specified grammar symbols enclosing byteOffset in O(log K).
     * @param[in] byteOffset Document byte offset to test.
     * @param[in] targetSyms Span of target grammar symbols.
     * @return Innermost enclosing TSNode or null node if not enclosed.
     */
    [[nodiscard]] TSNode FindEnclosingNodeAny(uint32_t byteOffset, std::span<const TSSymbol> targetSyms) const noexcept;

    /**
     * @brief Finds the innermost callable declaration (function or lambda expression) enclosing byteOffset.
     * @param[in] byteOffset Document byte offset to test.
     * @return Innermost enclosing function or lambda TSNode or null node if none found.
     */
    [[nodiscard]] TSNode FindEnclosingCallable(uint32_t byteOffset) const noexcept;

    TSNode m_root{};
    const TSLanguage* m_language = nullptr;
    size_t m_nodeCount = 0;
    std::vector<TSNode> m_errorNodes;
    std::vector<std::vector<TSNode>> m_nodesBySymbol;
    std::vector<ErrorRange> m_errorRanges;
    std::vector<CommentRange> m_commentRanges;
    std::vector<IndexedContainerSpan> m_containerSpans;
    std::shared_ptr<FlowFacts> m_flowFacts;
    TSSymbol m_symComment = 0;
    bool m_hasSyntaxErrors = false;
};
static_assert(sizeof(NodeIndex) <= 248);
} // namespace angel_lsp::analysis
