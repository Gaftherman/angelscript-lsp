#include "features/folding_range/FoldingRangeHandler.h"

#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <array>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Strips leading whitespace from a string view.
 * @param[in] s Input string view.
 * @return String view without leading spaces, tabs, or carriage returns.
 */
static std::string_view TrimLeading(std::string_view s)
{
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r'))
    {
        start++;
    }
    return s.substr(start);
}

/**
 * @brief Checks if a grammar node type represents a foldable syntax block.
 * @param[in] type AST node type string.
 * @return True if the node type is foldable.
 */
static bool IsSyntaxFoldingSymbol(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    const std::array<TSSymbol, 24> kSymbols = {gs.symAccessor,
                                               gs.symArgumentList,
                                               gs.symCaseClause,
                                               gs.symClassDeclaration,
                                               gs.symDoWhileStatement,
                                               gs.symEnumDeclaration,
                                               gs.symForStatement,
                                               gs.symForeachStatement,
                                               gs.symFuncDeclaration,
                                               gs.symFuncdefDeclaration,
                                               gs.symIfStatement,
                                               gs.symInitializerList,
                                               gs.symInterfaceDeclaration,
                                               gs.symInterfaceMethod,
                                               gs.symLambdaExpression,
                                               gs.symMixinDeclaration,
                                               gs.symNamespaceDeclaration,
                                               gs.symParameterList,
                                               gs.symStatementBlock,
                                               gs.symSwitchStatement,
                                               gs.symTryStatement,
                                               gs.symTypedInitializerList,
                                               gs.symVirtualProperty,
                                               gs.symWhileStatement};
    return std::find(kSymbols.begin(), kSymbols.end(), sym) != kSymbols.end();
}

/**
 * @brief Attempts to extract a syntax folding range from an AST node.
 * @param[in] node AST node.
 * @param[out] outFr Resulting folding range if applicable.
 * @return True if a multi-line syntax folding range was extracted.
 */
static bool TryExtractSyntaxFoldingRange(TSNode node, lsp::FoldingRange& outFr)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (!IsSyntaxFoldingSymbol(ts_node_symbol(node), gs))
    {
        return false;
    }

    const TSPoint startPt = ts_node_start_point(node);
    const TSPoint endPt = ts_node_end_point(node);
    if (startPt.row >= endPt.row)
    {
        return false;
    }

    outFr.startLine = startPt.row;
    outFr.endLine = endPt.row;
    outFr.startCharacter = startPt.column;
    outFr.endCharacter = endPt.column;
    outFr.kind = std::nullopt;
    return true;
}

/**
 * @brief Attempts to extract a block comment folding range from an AST node.
 * @param[in] node AST node.
 * @param[in] sourceCode Document source text.
 * @param[out] outFr Resulting comment folding range if applicable.
 * @return True if a multi-line block comment folding range was extracted.
 */
static bool TryExtractCommentFoldingRange(TSNode node, std::string_view sourceCode, lsp::FoldingRange& outFr)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(node) != gs.symComment)
    {
        return false;
    }

    const uint32_t startByte = ts_node_start_byte(node);
    if (startByte + 2 > sourceCode.size() || sourceCode[startByte] != '/' || sourceCode[startByte + 1] != '*')
    {
        return false;
    }

    const TSPoint startPt = ts_node_start_point(node);
    const TSPoint endPt = ts_node_end_point(node);
    if (startPt.row >= endPt.row)
    {
        return false;
    }

    outFr.startLine = startPt.row;
    outFr.endLine = endPt.row;
    outFr.startCharacter = startPt.column;
    outFr.endCharacter = endPt.column;
    outFr.kind = lsp::FoldingRangeKind::Comment;
    return true;
}

/**
 * @brief Traverses the AST iteratively using TSTreeCursor and collects syntax-based folding ranges.
 * @param[in] rootNode Root AST node.
 * @param[in] sourceCode Document source text.
 * @param[in,out] outRanges Output accumulator for folding ranges.
 */
static void CollectAstFoldingRanges(TSNode rootNode, std::string_view sourceCode,
                                    std::vector<lsp::FoldingRange>& outRanges)
{
    if (ts_node_is_null(rootNode))
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(rootNode);
    bool visiting = true;

    while (visiting)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        lsp::FoldingRange fr;
        if (TryExtractSyntaxFoldingRange(node, fr) || TryExtractCommentFoldingRange(node, sourceCode, fr))
        {
            outRanges.push_back(fr);
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        bool backtracked = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                backtracked = true;
                break;
            }
        }
        if (!backtracked)
        {
            visiting = false;
        }
    }

    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Updates a contiguous line span matching a predicate.
 * @param[in] match Whether current line matches the predicate.
 * @param[in] lineIdx Current zero-based line index.
 * @param[in,out] start Span start line tracker.
 * @param[in,out] end Span end line tracker.
 * @param[in] kind LSP folding range kind.
 * @param[in,out] outRanges Accumulated folding ranges.
 */
static void UpdateContiguousSpan(bool match, uint32_t lineIdx, int& start, int& end,
                                 std::optional<lsp::FoldingRangeKind> kind, std::vector<lsp::FoldingRange>& outRanges)
{
    if (match)
    {
        if (start == -1)
        {
            start = static_cast<int>(lineIdx);
        }
        end = static_cast<int>(lineIdx);
    }
    else
    {
        if (start != -1 && end > start)
        {
            lsp::FoldingRange fr;
            fr.startLine = static_cast<uint32_t>(start);
            fr.endLine = static_cast<uint32_t>(end);
            fr.kind = kind;
            outRanges.push_back(fr);
        }
        start = -1;
        end = -1;
    }
}

/**
 * @brief Processes preprocessor #region, #if, and matching end directives.
 * @param[in] trimmed Trimmed line text.
 * @param[in] lineIdx Current line index.
 * @param[in,out] regionStack Stack of unclosed #region line indices.
 * @param[in,out] ifStack Stack of unclosed #if line indices.
 * @param[in,out] outRanges Accumulated folding ranges.
 */
static void HandlePreprocessorLine(std::string_view trimmed, uint32_t lineIdx, std::vector<uint32_t>& regionStack,
                                   std::vector<uint32_t>& ifStack, std::vector<lsp::FoldingRange>& outRanges)
{
    if (trimmed.starts_with("#region"))
    {
        regionStack.push_back(lineIdx);
    }
    else if (trimmed.starts_with("#endregion") && !regionStack.empty())
    {
        const uint32_t rStart = regionStack.back();
        regionStack.pop_back();
        if (rStart < lineIdx)
        {
            lsp::FoldingRange fr;
            fr.startLine = rStart;
            fr.endLine = lineIdx;
            fr.kind = lsp::FoldingRangeKind::Region;
            outRanges.push_back(fr);
        }
    }
    else if (trimmed.starts_with("#if") || trimmed.starts_with("#ifdef") || trimmed.starts_with("#ifndef"))
    {
        ifStack.push_back(lineIdx);
    }
    else if (trimmed.starts_with("#endif") && !ifStack.empty())
    {
        const uint32_t ifStart = ifStack.back();
        ifStack.pop_back();
        if (ifStart < lineIdx)
        {
            lsp::FoldingRange fr;
            fr.startLine = ifStart;
            fr.endLine = lineIdx;
            fr.kind = std::nullopt;
            outRanges.push_back(fr);
        }
    }
}

/**
 * @brief Collects comment, import, and preprocessor folding ranges in a single zero-allocation pass.
 * @param[in] source Document source text.
 * @param[in,out] outRanges Accumulated folding ranges.
 */
static void CollectTextBasedFoldingRanges(std::string_view source, std::vector<lsp::FoldingRange>& outRanges)
{
    int commentStart = -1;
    int commentEnd = -1;
    int importStart = -1;
    int importEnd = -1;
    std::vector<uint32_t> regionStack;
    std::vector<uint32_t> ifStack;

    size_t start = 0;
    uint32_t lineIdx = 0;
    while (start < source.size())
    {
        size_t end = source.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = source.size();
        }
        size_t len = end - start;
        if (len > 0 && source[start + len - 1] == '\r')
        {
            --len;
        }
        const std::string_view trimmed = TrimLeading(source.substr(start, len));
        UpdateContiguousSpan(trimmed.starts_with("//"), lineIdx, commentStart, commentEnd,
                             lsp::FoldingRangeKind::Comment, outRanges);
        UpdateContiguousSpan(trimmed.starts_with("import ") || trimmed.starts_with("#include"), lineIdx, importStart,
                             importEnd, lsp::FoldingRangeKind::Imports, outRanges);
        HandlePreprocessorLine(trimmed, lineIdx, regionStack, ifStack, outRanges);

        start = (end < source.size()) ? end + 1 : source.size();
        ++lineIdx;
    }

    UpdateContiguousSpan(false, lineIdx, commentStart, commentEnd, lsp::FoldingRangeKind::Comment, outRanges);
    UpdateContiguousSpan(false, lineIdx, importStart, importEnd, lsp::FoldingRangeKind::Imports, outRanges);
}

/**
 * @brief Deduplicates and orders raw folding ranges by start line ascending and end line descending.
 * @param[in] rawRanges Unordered list of candidate folding ranges.
 * @return Deduplicated and sorted folding ranges.
 */
static FoldingRangeResult DeduplicateAndSortFoldingRanges(const std::vector<lsp::FoldingRange>& rawRanges)
{
    std::map<std::pair<uint32_t, uint32_t>, lsp::FoldingRange> uniqueMap;

    for (const auto& fr : rawRanges)
    {
        if (fr.startLine >= fr.endLine)
        {
            continue;
        }

        const auto key = std::make_pair(fr.startLine, fr.endLine);
        auto it = uniqueMap.find(key);
        if (it == uniqueMap.end())
        {
            uniqueMap[key] = fr;
        }
        else if (fr.kind.has_value() && !it->second.kind.has_value())
        {
            it->second = fr;
        }
    }

    FoldingRangeResult result;
    result.reserve(uniqueMap.size());
    for (auto& [k, fr] : uniqueMap)
    {
        result.push_back(fr);
    }

    std::sort(result.begin(), result.end(),
              [](const lsp::FoldingRange& a, const lsp::FoldingRange& b)
              {
                  if (a.startLine != b.startLine)
                  {
                      return a.startLine < b.startLine;
                  }
                  return a.endLine > b.endLine;
              });

    return result;
}

std::optional<FoldingRangeResult> GetFoldingRanges(const FoldingRangeRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::FoldingRange> rawRanges;
    const TSNode rootNode = ts_tree_root_node(request.tree);
    CollectAstFoldingRanges(rootNode, request.sourceCode, rawRanges);
    CollectTextBasedFoldingRanges(request.sourceCode, rawRanges);

    return DeduplicateAndSortFoldingRanges(rawRanges);
}
} // namespace angel_lsp::features
