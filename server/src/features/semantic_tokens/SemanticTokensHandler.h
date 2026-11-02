#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/PreprocessorRegions.h"
#include <ankerl/unordered_dense.h>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

#include "analysis/rules/RuleIndex.h"

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a semantic tokens request.
 */
struct SemanticTokensRequest
{
    SemanticTokensRequest(std::string u, std::string sc, TSTree* t, const analysis::SymbolTable& st)
        : uri(std::move(u)), sourceCode(std::move(sc)), tree(t), symbolTable(st)
    {
    }

    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    const analysis::SymbolTable& symbolTable;
    const analysis::NodeIndex* nodeIndex = nullptr;

    /**
     * @brief Scope tree of the document, used to tell what an identifier reference refers to.
     *
     * The highlights query is purely syntactic and cannot distinguish a parameter read from a
     * field read from a local read - all three are just an identifier in an expression. Given
     * the scope tree, each reference is resolved to its declaration and reported as whatever
     * that declaration is. Optional: left null, references fall back to plain "variable".
     */
    std::shared_ptr<const analysis::Scope> scopeRoot;

    /**
     * @brief Restricts the result to the tokens overlapping this range. Empty means the whole
     *        document, which is what textDocument/semanticTokens/full asks for.
     *
     * The token stream is delta-encoded against its own predecessor, so a client cannot slice
     * a full result itself - the first token of any slice would still be encoded relative to
     * whatever preceded it. Narrowing has to happen before encoding, which is why it is a
     * request field rather than post-processing.
     */
    std::optional<lsp::Range> range;

    /**
     * @brief Line ranges removed by the preprocessor (#if blocks whose condition is false).
     *
     * The compiler never sees code inside these lines, and the server already silences every
     * diagnostic there. Painting them as comments is the other half of that same truth: the
     * reader can tell at a glance that the compiler will never visit this code either.
     */
    std::vector<angel_lsp::utils::ExcludedLineRange> excludedLineRanges;
};

/**
 * @brief Computes full semantic tokens stream using HIGHLIGHTS_QUERY.
 * @param request Immutable context for semantic tokenization.
 * @return SemanticTokens struct containing encoded 5-tuple integer stream.
 */
lsp::SemanticTokens GetSemanticTokens(const SemanticTokensRequest& request);

/**
 * @brief Checks if an AST node is a `<` or `>` bracket in a template type list or cast.
 * @param node AST node to check.
 * @return True if node is template punctuation.
 */
[[nodiscard]] bool IsTemplatePunctuationNode(TSNode node) noexcept;

/**
 * @brief Returns server SemanticTokensLegend with token types and modifiers.
 */
const lsp::SemanticTokensLegend& GetSemanticTokensLegend();

/**
 * @brief Describes how to turn one token stream into another.
 *
 * A whole-document re-tokenisation is cheap here, but sending it is not: the payload is five
 * integers per token, and a typing session re-sends all of them on every keystroke. Since an
 * edit almost always leaves the run before it and the run after it untouched, the difference
 * is expressible as a single splice.
 *
 * @param previous Token data the client already has.
 * @param current Token data just computed.
 * @return Edits to apply, in the order given. Empty when the two streams are identical.
 */
std::vector<lsp::SemanticTokensEdit> ComputeSemanticTokensDelta(const std::vector<lsp::uint>& previous,
                                                                const std::vector<lsp::uint>& current);

/**
 * @brief Semantic token classification, modifiers bitmask, and grammar symbol mapping.
 */
struct TokenMeta
{
    uint32_t tokenType = 0;
    uint32_t tokenMod = 0;
    int priority = 0;
};

/**
 * @brief Raw semantic token with byte offsets, token type, and modifier flags.
 */
struct RawToken
{
    uint32_t line = 0;
    uint32_t startChar = 0;
    uint32_t length = 0;
    uint32_t tokenType = 0;
    uint32_t tokenModifiers = 0;
    int priority = 0;
};

enum TokenTypeIndex : uint32_t
{
    Type_Namespace = 0,
    Type_Type = 1,
    Type_Class = 2,
    Type_Enum = 3,
    Type_Interface = 4,
    Type_Struct = 5,
    Type_TypeParameter = 6,
    Type_Parameter = 7,
    Type_Variable = 8,
    Type_Property = 9,
    Type_EnumMember = 10,
    Type_Event = 11,
    Type_Function = 12,
    Type_Method = 13,
    Type_Macro = 14,
    Type_Keyword = 15,
    Type_Modifier = 16,
    Type_Comment = 17,
    Type_String = 18,
    Type_Number = 19,
    Type_Regexp = 20,
    Type_Operator = 21,
    Type_Decorator = 22,
    Type_TemplatePunctuation = 23
};

/**
 * @brief Tree-sitter query capture rule mapping capture names to LSP semantic token types.
 */
struct CaptureRule
{
    uint32_t tokenType = Type_Variable;
    uint32_t tokenMod = 0;
    int priority = 1;
    bool isOperatorOrPunctuation = false;
    bool valid = false;
};

/**
 * @brief Pre-compiled query and token mapping tables for syntax highlighting.
 */
struct HighlightsQueryData
{
    TSQuery* query = nullptr;
    std::vector<CaptureRule> rules;
};

/**
 * @brief Precomputed byte intervals and grammar symbol classifications for class declaration spans.
 */
class ClassSpanIndex
{
  public:
    /**
     * @brief Builds class span index from NodeIndex and source code.
     * @param[in] nodeIndex Pre-built AST node index, or nullptr.
     * @param[in] sourceCode Source text buffer.
     * @param[in] syms Grammar symbol identifiers.
     * @return Initialized ClassSpanIndex.
     */
    static ClassSpanIndex Build(const analysis::NodeIndex* nodeIndex, std::string_view sourceCode,
                                const parser::GrammarSymbols& syms);

    /**
     * @brief Finds enclosing class name for a byte offset.
     * @param[in] byteOffset Target byte position in source.
     * @return Enclosing class name view, or empty if outside classes.
     */
    [[nodiscard]] std::string_view FindEnclosingClass(uint32_t byteOffset) const;

  private:
    std::vector<parser::AstNodeSpan> m_spans;
};

/**
 * @brief Context for refining raw syntax token classifications using semantic analysis symbol tables.
 */
struct TokenRefinementContext
{
    TokenRefinementContext(const SemanticTokensRequest& req,
                           const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& refKinds,
                           const ankerl::unordered_dense::map<uint32_t, uint32_t>& enumUpgrades,
                           const ClassSpanIndex& spans, std::shared_ptr<const analysis::rules::RuleIndex> rules)
        : request(req), referenceKinds(refKinds), scopedEnumUpgrades(enumUpgrades), classSpans(spans),
          ruleIndex(std::move(rules))
    {
    }

    const SemanticTokensRequest& request;
    const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& referenceKinds;
    const ankerl::unordered_dense::map<uint32_t, uint32_t>& scopedEnumUpgrades;
    const ClassSpanIndex& classSpans;
    std::shared_ptr<const analysis::rules::RuleIndex> ruleIndex;
    mutable ankerl::unordered_dense::map<std::string_view, std::vector<std::string>> classHierarchyCache;
};
} // namespace angel_lsp::features
