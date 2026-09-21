#include "features/folding_range/FoldingRangeHandler.h"
#include <algorithm>
#include <array>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{
namespace
{
/**
 * @brief Strips leading whitespace from a string view.
 * @param[in] s Input string view.
 * @return String view without leading spaces, tabs, or carriage returns.
 */
std::string_view TrimLeading(std::string_view s)
{
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r'))
    {
        start++;
    }
    return s.substr(start);
}

/**
 * @brief Splits source code into lines (views without trailing \r or \n).
 * @param[in] source Document source text.
 * @return Vector of string views representing each line.
 */
std::vector<std::string_view> SplitLines(std::string_view source)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < source.size(); ++i)
    {
        if (source[i] == '\n')
        {
            size_t len = i - start;
            if (len > 0 && source[start + len - 1] == '\r')
            {
                len--;
            }
            lines.push_back(source.substr(start, len));
            start = i + 1;
        }
    }
    if (start <= source.size())
    {
        size_t len = source.size() - start;
        if (len > 0 && source[start + len - 1] == '\r')
        {
            len--;
        }
        lines.push_back(source.substr(start, len));
    }
    return lines;
}

/**
 * @brief Checks if a grammar node type represents a foldable syntax block.
 * @param[in] type AST node type string.
 * @return True if the node type is foldable.
 */
bool IsSyntaxFoldingNode(std::string_view type)
{
    static constexpr std::array<std::string_view, 24> k_foldingTypes = {
        "accessor",
        "argument_list",
        "case_clause",
        "class_declaration",
        "do_while_statement",
        "enum_declaration",
        "for_statement",
        "foreach_statement",
        "func_declaration",
        "funcdef_declaration",
        "if_statement",
        "initializer_list",
        "interface_declaration",
        "interface_method",
        "lambda_expression",
        "mixin_declaration",
        "namespace_declaration",
        "parameter_list",
        "statement_block",
        "switch_statement",
        "try_statement",
        "typed_initializer_list",
        "virtual_property",
        "while_statement",
    };
    return std::binary_search(k_foldingTypes.begin(), k_foldingTypes.end(), type);
}

/**
 * @brief Attempts to extract a syntax folding range from an AST node.
 * @param[in] node AST node.
 * @param[out] outFr Resulting folding range if applicable.
 * @return True if a multi-line syntax folding range was extracted.
 */
bool TryExtractSyntaxFoldingRange(TSNode node, lsp::FoldingRange& outFr)
{
    const char* type = ts_node_type(node);
    if (!type || !IsSyntaxFoldingNode(type))
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
bool TryExtractCommentFoldingRange(TSNode node, std::string_view sourceCode, lsp::FoldingRange& outFr)
{
    const char* type = ts_node_type(node);
    if (!type || std::string_view(type) != "comment")
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
void CollectAstFoldingRanges(TSNode rootNode, std::string_view sourceCode, std::vector<lsp::FoldingRange>& outRanges)
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
 * @brief Collects folding ranges for contiguous single-line comments (// ...).
 * @param[in] lines Splitted document lines.
 * @param[in,out] outRanges Output accumulator for folding ranges.
 */
void CollectSingleLineCommentRanges(const std::vector<std::string_view>& lines,
                                    std::vector<lsp::FoldingRange>& outRanges)
{
    int commentStart = -1;
    int commentEnd = -1;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
    {
        const std::string_view trimmed = TrimLeading(lines[i]);
        if (trimmed.starts_with("//"))
        {
            if (commentStart == -1)
            {
                commentStart = i;
            }
            commentEnd = i;
        }
        else
        {
            if (commentStart != -1 && commentEnd > commentStart)
            {
                lsp::FoldingRange fr;
                fr.startLine = static_cast<uint32_t>(commentStart);
                fr.endLine = static_cast<uint32_t>(commentEnd);
                fr.kind = lsp::FoldingRangeKind::Comment;
                outRanges.push_back(fr);
            }
            commentStart = -1;
            commentEnd = -1;
        }
    }
    if (commentStart != -1 && commentEnd > commentStart)
    {
        lsp::FoldingRange fr;
        fr.startLine = static_cast<uint32_t>(commentStart);
        fr.endLine = static_cast<uint32_t>(commentEnd);
        fr.kind = lsp::FoldingRangeKind::Comment;
        outRanges.push_back(fr);
    }
}

/**
 * @brief Collects folding ranges for contiguous imports and #include directives.
 * @param[in] lines Splitted document lines.
 * @param[in,out] outRanges Output accumulator for folding ranges.
 */
void CollectImportAndIncludeRanges(const std::vector<std::string_view>& lines,
                                   std::vector<lsp::FoldingRange>& outRanges)
{
    int importStart = -1;
    int importEnd = -1;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
    {
        const std::string_view trimmed = TrimLeading(lines[i]);
        if (trimmed.starts_with("import ") || trimmed.starts_with("#include"))
        {
            if (importStart == -1)
            {
                importStart = i;
            }
            importEnd = i;
        }
        else
        {
            if (importStart != -1 && importEnd > importStart)
            {
                lsp::FoldingRange fr;
                fr.startLine = static_cast<uint32_t>(importStart);
                fr.endLine = static_cast<uint32_t>(importEnd);
                fr.kind = lsp::FoldingRangeKind::Imports;
                outRanges.push_back(fr);
            }
            importStart = -1;
            importEnd = -1;
        }
    }
    if (importStart != -1 && importEnd > importStart)
    {
        lsp::FoldingRange fr;
        fr.startLine = static_cast<uint32_t>(importStart);
        fr.endLine = static_cast<uint32_t>(importEnd);
        fr.kind = lsp::FoldingRangeKind::Imports;
        outRanges.push_back(fr);
    }
}

/**
 * @brief Collects folding ranges for preprocessor directives (#region and #if).
 * @param[in] lines Splitted document lines.
 * @param[in,out] outRanges Output accumulator for folding ranges.
 */
void CollectPreprocessorFoldingRanges(const std::vector<std::string_view>& lines,
                                      std::vector<lsp::FoldingRange>& outRanges)
{
    std::vector<uint32_t> regionStack;
    std::vector<uint32_t> ifStack;

    for (uint32_t i = 0; i < lines.size(); ++i)
    {
        const std::string_view trimmed = TrimLeading(lines[i]);
        if (trimmed.starts_with("#region"))
        {
            regionStack.push_back(i);
        }
        else if (trimmed.starts_with("#endregion"))
        {
            if (!regionStack.empty())
            {
                const uint32_t start = regionStack.back();
                regionStack.pop_back();
                if (start < i)
                {
                    lsp::FoldingRange fr;
                    fr.startLine = start;
                    fr.endLine = i;
                    fr.kind = lsp::FoldingRangeKind::Region;
                    outRanges.push_back(fr);
                }
            }
        }
        else if (trimmed.starts_with("#if") || trimmed.starts_with("#ifdef") || trimmed.starts_with("#ifndef"))
        {
            ifStack.push_back(i);
        }
        else if (trimmed.starts_with("#endif"))
        {
            if (!ifStack.empty())
            {
                const uint32_t start = ifStack.back();
                ifStack.pop_back();
                if (start < i)
                {
                    lsp::FoldingRange fr;
                    fr.startLine = start;
                    fr.endLine = i;
                    fr.kind = std::nullopt;
                    outRanges.push_back(fr);
                }
            }
        }
    }
}

/**
 * @brief Deduplicates and orders raw folding ranges by start line ascending and end line descending.
 * @param[in] rawRanges Unordered list of candidate folding ranges.
 * @return Deduplicated and sorted folding ranges.
 */
FoldingRangeResult DeduplicateAndSortFoldingRanges(const std::vector<lsp::FoldingRange>& rawRanges)
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
} // namespace

std::optional<FoldingRangeResult> GetFoldingRanges(const FoldingRangeRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::FoldingRange> rawRanges;
    const TSNode rootNode = ts_tree_root_node(request.tree);
    CollectAstFoldingRanges(rootNode, request.sourceCode, rawRanges);

    const auto lines = SplitLines(request.sourceCode);
    CollectSingleLineCommentRanges(lines, rawRanges);
    CollectImportAndIncludeRanges(lines, rawRanges);
    CollectPreprocessorFoldingRanges(lines, rawRanges);

    return DeduplicateAndSortFoldingRanges(rawRanges);
}
} // namespace angel_lsp::features
