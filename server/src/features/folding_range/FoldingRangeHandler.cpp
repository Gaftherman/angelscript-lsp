#include "features/folding_range/FoldingRangeHandler.h"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Strips leading whitespace from a string view.
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
         * @brief Recursively traverses the AST and collects syntax-based folding ranges.
         */
        void CollectAstFoldingRanges(TSNode node, const std::string &sourceCode, std::vector<lsp::FoldingRange> &outRanges)
        {
            if (ts_node_is_null(node))
            {
                return;
            }

            std::string_view type = ts_node_type(node);
            TSPoint startPt = ts_node_start_point(node);
            TSPoint endPt = ts_node_end_point(node);

            if (startPt.row < endPt.row)
            {
                if (type == "class_declaration" || type == "mixin_declaration" ||
                    type == "interface_declaration" || type == "namespace_declaration" ||
                    type == "enum_declaration" || type == "func_declaration" ||
                    type == "interface_method" || type == "funcdef_declaration" ||
                    type == "lambda_expression" || type == "virtual_property" ||
                    type == "accessor" || type == "statement_block" ||
                    type == "switch_statement" || type == "case_clause" ||
                    type == "try_statement" || type == "if_statement" ||
                    type == "for_statement" || type == "foreach_statement" ||
                    type == "while_statement" || type == "do_while_statement" ||
                    type == "argument_list" || type == "parameter_list" ||
                    type == "initializer_list" || type == "typed_initializer_list")
                {
                    lsp::FoldingRange fr;
                    fr.startLine = startPt.row;
                    fr.endLine = endPt.row;
                    fr.startCharacter = startPt.column;
                    fr.endCharacter = endPt.column;
                    fr.kind = std::nullopt;
                    outRanges.push_back(fr);
                }
                else if (type == "comment")
                {
                    uint32_t startByte = ts_node_start_byte(node);
                    if (startByte + 2 <= sourceCode.size() && sourceCode[startByte] == '/' && sourceCode[startByte + 1] == '*')
                    {
                        lsp::FoldingRange fr;
                        fr.startLine = startPt.row;
                        fr.endLine = endPt.row;
                        fr.startCharacter = startPt.column;
                        fr.endCharacter = endPt.column;
                        fr.kind = lsp::FoldingRangeKind::Comment;
                        outRanges.push_back(fr);
                    }
                }
            }

            uint32_t childCount = ts_node_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                CollectAstFoldingRanges(ts_node_child(node, i), sourceCode, outRanges);
            }
        }
    }

    std::optional<FoldingRangeResult> GetFoldingRanges(const FoldingRangeRequest &request)
    {
        if (!request.tree || request.sourceCode.empty())
        {
            return std::nullopt;
        }

        std::vector<lsp::FoldingRange> rawRanges;

        // 1. AST-based syntax and block comment ranges
        TSNode rootNode = ts_tree_root_node(request.tree);
        CollectAstFoldingRanges(rootNode, request.sourceCode, rawRanges);

        // 2. Line-based processing: contiguous single-line comments, #region, #if, and imports
        auto lines = SplitLines(request.sourceCode);

        // Contiguous single-line comments (// ...)
        int commentStart = -1;
        int commentEnd = -1;

        for (int i = 0; i < static_cast<int>(lines.size()); ++i)
        {
            std::string_view trimmed = TrimLeading(lines[i]);
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
                    rawRanges.push_back(fr);
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
            rawRanges.push_back(fr);
        }

        // Contiguous imports / includes
        int importStart = -1;
        int importEnd = -1;

        for (int i = 0; i < static_cast<int>(lines.size()); ++i)
        {
            std::string_view trimmed = TrimLeading(lines[i]);
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
                    rawRanges.push_back(fr);
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
            rawRanges.push_back(fr);
        }

        // Preprocessor directives: #region ... #endregion and #if ... #endif
        std::vector<uint32_t> regionStack;
        std::vector<uint32_t> ifStack;

        for (uint32_t i = 0; i < lines.size(); ++i)
        {
            std::string_view trimmed = TrimLeading(lines[i]);
            if (trimmed.starts_with("#region"))
            {
                regionStack.push_back(i);
            }
            else if (trimmed.starts_with("#endregion"))
            {
                if (!regionStack.empty())
                {
                    uint32_t start = regionStack.back();
                    regionStack.pop_back();
                    if (start < i)
                    {
                        lsp::FoldingRange fr;
                        fr.startLine = start;
                        fr.endLine = i;
                        fr.kind = lsp::FoldingRangeKind::Region;
                        rawRanges.push_back(fr);
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
                    uint32_t start = ifStack.back();
                    ifStack.pop_back();
                    if (start < i)
                    {
                        lsp::FoldingRange fr;
                        fr.startLine = start;
                        fr.endLine = i;
                        fr.kind = std::nullopt;
                        rawRanges.push_back(fr);
                    }
                }
            }
        }

        // 3. De-duplication and Sorting
        // Map from (startLine, endLine) to folding range (preferring explicit kind over nullopt)
        std::map<std::pair<uint32_t, uint32_t>, lsp::FoldingRange> uniqueMap;

        for (const auto &fr : rawRanges)
        {
            if (fr.startLine >= fr.endLine)
            {
                continue;
            }

            auto key = std::make_pair(fr.startLine, fr.endLine);
            auto it = uniqueMap.find(key);
            if (it == uniqueMap.end())
            {
                uniqueMap[key] = fr;
            }
            else
            {
                // If the new one has a kind and existing doesn't, override
                if (fr.kind.has_value() && !it->second.kind.has_value())
                {
                    it->second = fr;
                }
            }
        }

        FoldingRangeResult result;
        result.reserve(uniqueMap.size());
        for (auto &[k, fr] : uniqueMap)
        {
            result.push_back(fr);
        }

        std::sort(result.begin(), result.end(),
                  [](const lsp::FoldingRange &a, const lsp::FoldingRange &b)
                  {
                      if (a.startLine != b.startLine)
                      {
                          return a.startLine < b.startLine;
                      }
                      return a.endLine > b.endLine;
                  });

        return result;
    }
}
