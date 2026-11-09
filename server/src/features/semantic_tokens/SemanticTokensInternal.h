#pragma once

#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SemanticTokenFacts.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Raw semantic token with 0-indexed line and column coordinates.
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

/**
 * @brief Classification metadata associated with an emitted token.
 */
struct TokenMeta
{
    uint32_t tokenType = 0;
    uint32_t tokenMod = 0;
    int priority = 0;
};

/**
 * @brief Context for semantic token refinement passes.
 */
struct TokenRefinementContext
{
    TokenRefinementContext(const SemanticTokensRequest& req,
                           const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& refKinds,
                           const ankerl::unordered_dense::map<uint32_t, uint32_t>& enumUpgrades,
                           std::shared_ptr<const analysis::rules::RuleIndex> rules)
        : request(req), referenceKinds(refKinds), scopedEnumUpgrades(enumUpgrades), ruleIndex(std::move(rules))
    {
    }

    const SemanticTokensRequest& request;
    const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& referenceKinds;
    const ankerl::unordered_dense::map<uint32_t, uint32_t>& scopedEnumUpgrades;
    std::shared_ptr<const analysis::rules::RuleIndex> ruleIndex;
    mutable analysis::InheritedHierarchyCache classHierarchyCache;
};

/**
 * @brief Packs a line and character into a 64-bit coordinate key.
 * @param[in] line Zero-based source line.
 * @param[in] character Zero-based column offset.
 * @return 64-bit packed coordinate key.
 */
[[nodiscard]] constexpr uint64_t PositionKey(uint32_t line, uint32_t character) noexcept
{
    return (static_cast<uint64_t>(line) << 32) | character;
}

/**
 * @brief Splits source buffer into views of individual lines.
 * @param[in] str Source buffer view.
 * @return Vector of line string views.
 */
[[nodiscard]] std::vector<std::string_view> SplitLinesView(std::string_view str);

/**
 * @brief Emits boundary tokens for template angle brackets.
 * @param[in] capture AST-free highlight capture item.
 * @param[in] meta Token metadata.
 * @param[in] sourceLines Views of individual source lines.
 * @param[in,out] rawTokens Vector accumulating raw tokens.
 */
void EmitTemplatePunctuationTokens(const analysis::IndexedHighlightCapture& capture, TokenMeta meta,
                                   const std::vector<std::string_view>& sourceLines, std::vector<RawToken>& rawTokens);

/**
 * @brief Emits a single token or splits multi-line tokens into line-delimited items.
 * @param[in] capture AST-free highlight capture item.
 * @param[in] meta Token metadata.
 * @param[in] sourceLines Views of individual source lines.
 * @param[in,out] rawTokens Vector accumulating raw tokens.
 */
void EmitTokenOrSplitLines(const analysis::IndexedHighlightCapture& capture, TokenMeta meta,
                           const std::vector<std::string_view>& sourceLines, std::vector<RawToken>& rawTokens);

/**
 * @brief Collects semantic tokens from pre-extracted captures using refinement rules.
 * @param[in] ctx Refinement context.
 * @param[in] sourceLines Views of individual source lines.
 * @param[in,out] rawTokens Vector accumulating raw tokens.
 */
void CollectHighlightsTokens(const TokenRefinementContext& ctx, const std::vector<std::string_view>& sourceLines,
                             std::vector<RawToken>& rawTokens);

/**
 * @brief Deduplicates overlapping raw tokens by coordinate and priority.
 * @param[in,out] rawTokens Vector of raw tokens to deduplicate.
 * @return Deduplicated and sorted vector of tokens.
 */
[[nodiscard]] std::vector<RawToken> DeduplicateTokens(std::vector<RawToken>& rawTokens);

/**
 * @brief Restricts tokens to those overlapping an optional request range.
 * @param[in,out] tokens Vector of tokens to filter.
 * @param[in] range Target LSP coordinate range.
 */
void NarrowTokensToRange(std::vector<RawToken>& tokens, const lsp::Range& range);

/**
 * @brief Filters out invalid standalone operator tokens that do not match genuine AngelScript operators.
 * @param[in,out] tokens Vector of tokens to inspect.
 * @param[in] sourceLines Views of individual source lines.
 */
void FilterInvalidOperators(std::vector<RawToken>& tokens, const std::vector<std::string_view>& sourceLines);

/**
 * @brief Collects definition kinds for reference identifiers by walking the lexical scope tree.
 * @param[in] scope Pointer to scope root.
 * @param[out] referenceKinds Output map from coordinate key to definition kind.
 */
void CollectReferenceKinds(const analysis::Scope* scope,
                           ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& referenceKinds);

/**
 * @brief Resolves variable reference token type to Parameter or Variable based on definition kind.
 * @param[in] line 0-indexed line.
 * @param[in] column 0-indexed character.
 * @param[in] referenceKinds Map of definition kinds.
 * @param[in] tokenType Default token type.
 * @return Resolved token type index.
 */
[[nodiscard]] uint32_t
ResolveReferenceTokenType(uint32_t line, uint32_t column,
                          const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& referenceKinds,
                          uint32_t tokenType);

/**
 * @brief Classifies a token as a reserved keyword or declaration modifier.
 * @param[in] tokenText Identifier text view.
 * @return Classified token type if keyword or modifier; std::nullopt otherwise.
 */
[[nodiscard]] std::optional<uint32_t> ClassifyReservedKeywordOrModifier(std::string_view tokenText) noexcept;

/**
 * @brief Suppresses semantic token emission if a scoped leaf does not exist in qualifier scope.
 * @param[in] capture AST-free highlight capture item.
 * @param[in] tokenText Identifier text view.
 * @param[in] ctx Refinement context.
 * @return True if token should be suppressed.
 */
[[nodiscard]] bool ShouldSuppressScopedLeafToken(const analysis::IndexedHighlightCapture& capture,
                                                 std::string_view tokenText, const TokenRefinementContext& ctx);

/**
 * @brief Refines token type against scoped enums, class members, and symbol table.
 * @param[in] tokenText Token identifier text view.
 * @param[in] startByte Token start byte offset.
 * @param[in] tokenType Current token type.
 * @param[in] ctx Refinement context.
 * @return Refined token type index.
 */
[[nodiscard]] uint32_t RefineSyntaxContext(std::string_view tokenText, uint32_t startByte, uint32_t tokenType,
                                           const TokenRefinementContext& ctx);

/**
 * @brief Checks whether an identifier is an unqualified member of the enclosing class.
 * @param[in] tokenText Identifier text view.
 * @param[in] startByte Token start byte offset.
 * @param[in] tokenType Current token type.
 * @param[in] ctx Refinement context.
 * @return Upgraded token type index.
 */
[[nodiscard]] uint32_t RefineEnclosingClassMember(std::string_view tokenText, uint32_t startByte, uint32_t tokenType,
                                                  const TokenRefinementContext& ctx);

/**
 * @brief Refines captured token type using semantic analysis context.
 * @param[in] capture AST-free highlight capture item.
 * @param[in] tokenType Current token type.
 * @param[in] ctx Refinement context.
 * @return Refined token type or UINT32_MAX if suppressed.
 */
[[nodiscard]] uint32_t RefineCapturedTokenType(const analysis::IndexedHighlightCapture& capture, uint32_t tokenType,
                                               const TokenRefinementContext& ctx);

/**
 * @brief Pre-calculates scoped enum qualifier and member token upgrades.
 * @param[in] facts Pre-extracted semantic token facts.
 * @param[in] sourceCode Source text buffer view.
 * @param[in] symbolTable Global symbol table.
 * @return Map from start byte to upgraded token type.
 */
[[nodiscard]] ankerl::unordered_dense::map<uint32_t, uint32_t>
PrecalculateScopedEnumUpgrades(const analysis::SemanticTokenFacts& facts, std::string_view sourceCode,
                               const analysis::SymbolTable& symbolTable);

} // namespace angel_lsp::features
