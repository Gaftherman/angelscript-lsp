#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "parser/queries/BuiltQueries.h"
#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::features
{
    namespace
    {
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
            Type_Decorator = 22
        };

        enum TokenModifierBit : uint32_t
        {
            Mod_Declaration = 1 << 0,
            Mod_Definition = 1 << 1,
            Mod_Readonly = 1 << 2,
            Mod_Static = 1 << 3,
            Mod_Deprecated = 1 << 4,
            Mod_Abstract = 1 << 5,
            Mod_Async = 1 << 6,
            Mod_Modification = 1 << 7,
            Mod_Documentation = 1 << 8,
            Mod_DefaultLibrary = 1 << 9
        };

        struct RawToken
        {
            uint32_t line = 0;
            uint32_t startChar = 0;
            uint32_t length = 0;
            uint32_t tokenType = 0;
            uint32_t tokenModifiers = 0;
            int priority = 0;
        };

        std::vector<std::string_view> SplitLinesView(std::string_view str)
        {
            std::vector<std::string_view> lines;
            size_t start = 0;
            for (size_t i = 0; i < str.size(); ++i)
            {
                if (str[i] == '\n')
                {
                    size_t len = i - start;
                    if (len > 0 && str[i - 1] == '\r')
                    {
                        len--;
                    }
                    lines.emplace_back(str.data() + start, len);
                    start = i + 1;
                }
            }
            if (start < str.size())
            {
                size_t len = str.size() - start;
                if (len > 0 && str.back() == '\r')
                {
                    len--;
                }
                lines.emplace_back(str.data() + start, len);
            }
            return lines;
        }
    }

    const lsp::SemanticTokensLegend &GetSemanticTokensLegend()
    {
        static const lsp::SemanticTokensLegend legend = {
            /* tokenTypes */ {
                "namespace",
                "type",
                "class",
                "enum",
                "interface",
                "struct",
                "typeParameter",
                "parameter",
                "variable",
                "property",
                "enumMember",
                "event",
                "function",
                "method",
                "macro",
                "keyword",
                "modifier",
                "comment",
                "string",
                "number",
                "regexp",
                "operator",
                "decorator"
            },
            /* tokenModifiers */ {
                "declaration",
                "definition",
                "readonly",
                "static",
                "deprecated",
                "abstract",
                "async",
                "modification",
                "documentation",
                "defaultLibrary"
            }
        };
        return legend;
    }

    lsp::SemanticTokens GetSemanticTokens(const SemanticTokensRequest &request)
    {
        if (!request.tree || request.sourceCode.empty())
        {
            return lsp::SemanticTokens{};
        }

        const TSLanguage *lang = tree_sitter_angelscript();
        uint32_t errorOffset = 0;
        TSQueryError errorType = TSQueryErrorNone;
        TSQuery *query = ts_query_new(lang, parser::queries::HIGHLIGHTS_QUERY,
                                     static_cast<uint32_t>(strlen(parser::queries::HIGHLIGHTS_QUERY)),
                                     &errorOffset, &errorType);
        if (!query)
        {
            return lsp::SemanticTokens{};
        }

        TSQueryCursor *cursor = ts_query_cursor_new();
        ts_query_cursor_exec(cursor, query, ts_tree_root_node(request.tree));

        std::vector<RawToken> rawTokens;
        auto sourceLines = SplitLinesView(request.sourceCode);

        TSQueryMatch match;
        uint32_t captureIndex = 0;

        while (ts_query_cursor_next_capture(cursor, &match, &captureIndex))
        {
            TSNode node = match.captures[captureIndex].node;
            uint32_t captureId = match.captures[captureIndex].index;

            uint32_t nameLen = 0;
            const char *captureNamePtr = ts_query_capture_name_for_id(query, captureId, &nameLen);
            std::string_view captureName(captureNamePtr, nameLen);

            uint32_t tokenType = Type_Variable;
            uint32_t tokenMod = 0;
            int priority = 1;
            bool valid = true;

            if (captureName == "comment")
            {
                tokenType = Type_Comment;
                priority = 10;
            }
            else if (captureName == "keyword.directive")
            {
                tokenType = Type_Macro;
                priority = 8;
            }
            else if (captureName == "string")
            {
                tokenType = Type_String;
                priority = 10;
            }
            else if (captureName == "number")
            {
                tokenType = Type_Number;
                priority = 9;
            }
            else if (captureName == "type.builtin")
            {
                tokenType = Type_Keyword;
                tokenMod = 0;
                priority = 7;
            }
            else if (captureName == "type")
            {
                tokenType = Type_Type;
                priority = 6;
            }
            else if (captureName == "constant")
            {
                tokenType = Type_EnumMember;
                priority = 5;
            }
            else if (captureName == "constant.builtin")
            {
                tokenType = Type_Keyword;
                tokenMod = Mod_Readonly;
                priority = 8;
            }
            else if (captureName == "function")
            {
                tokenType = Type_Function;
                tokenMod = Mod_Declaration;
                priority = 6;
            }
            else if (captureName == "function.call")
            {
                tokenType = Type_Function;
                priority = 5;
            }
            else if (captureName == "function.method.call")
            {
                tokenType = Type_Method;
                priority = 5;
            }
            else if (captureName == "property")
            {
                tokenType = Type_Property;
                priority = 5;
            }
            else if (captureName == "variable")
            {
                tokenType = Type_Variable;
                priority = 3;
            }
            else if (captureName == "variable.parameter")
            {
                tokenType = Type_Parameter;
                priority = 4;
            }
            else if (captureName == "module")
            {
                tokenType = Type_Namespace;
                priority = 6;
            }
            else if (captureName == "keyword" || captureName == "keyword.control")
            {
                tokenType = Type_Keyword;
                priority = 8;
            }
            else if (captureName == "keyword.modifier")
            {
                tokenType = Type_Modifier;
                priority = 8;
            }
            else if (captureName == "boolean")
            {
                tokenType = Type_Keyword;
                priority = 8;
            }
            else if (captureName == "operator" || captureName == "punctuation.special")
            {
                tokenType = Type_Operator;
                priority = 4;
            }
            else
            {
                valid = false;
            }

            if (!valid)
            {
                continue;
            }

            TSPoint startPoint = ts_node_start_point(node);
            TSPoint endPoint = ts_node_end_point(node);

            if (startPoint.row == endPoint.row)
            {
                if (endPoint.column > startPoint.column)
                {
                    rawTokens.push_back(RawToken{
                        startPoint.row,
                        startPoint.column,
                        endPoint.column - startPoint.column,
                        tokenType,
                        tokenMod,
                        priority
                    });
                }
            }
            else
            {
                // Multi-line token: split line by line
                for (uint32_t r = startPoint.row; r <= endPoint.row; ++r)
                {
                    if (r >= sourceLines.size())
                    {
                        break;
                    }
                    uint32_t lineLen = static_cast<uint32_t>(sourceLines[r].size());
                    uint32_t sc = (r == startPoint.row) ? startPoint.column : 0;
                    uint32_t ec = (r == endPoint.row) ? endPoint.column : lineLen;

                    if (ec > sc)
                    {
                        rawTokens.push_back(RawToken{
                            r,
                            sc,
                            ec - sc,
                            tokenType,
                            tokenMod,
                            priority
                        });
                    }
                }
            }
        }

        ts_query_cursor_delete(cursor);
        ts_query_delete(query);

        if (rawTokens.empty())
        {
            return lsp::SemanticTokens{};
        }

        // Sort tokens: ascending line, ascending startChar, descending priority
        std::sort(rawTokens.begin(), rawTokens.end(), [](const RawToken &a, const RawToken &b)
        {
            if (a.line != b.line) return a.line < b.line;
            if (a.startChar != b.startChar) return a.startChar < b.startChar;
            return a.priority > b.priority;
        });

        // Deduplicate and filter overlapping tokens on the same line
        std::vector<RawToken> filteredTokens;
        filteredTokens.reserve(rawTokens.size());

        uint32_t currentLine = UINT32_MAX;
        uint32_t lastEndChar = 0;

        for (const auto &tok : rawTokens)
        {
            if (tok.length == 0)
            {
                continue;
            }

            if (tok.line != currentLine)
            {
                currentLine = tok.line;
                lastEndChar = 0;
            }

            if (tok.startChar >= lastEndChar)
            {
                filteredTokens.push_back(tok);
                lastEndChar = tok.startChar + tok.length;
            }
        }

        // Delta Encode 5-tuple
        std::vector<lsp::uint> data;
        data.reserve(filteredTokens.size() * 5);

        uint32_t prevLine = 0;
        uint32_t prevChar = 0;

        for (const auto &tok : filteredTokens)
        {
            uint32_t deltaLine = tok.line - prevLine;
            uint32_t deltaChar = (deltaLine == 0) ? (tok.startChar - prevChar) : tok.startChar;

            data.push_back(deltaLine);
            data.push_back(deltaChar);
            data.push_back(tok.length);
            data.push_back(tok.tokenType);
            data.push_back(tok.tokenModifiers);

            prevLine = tok.line;
            prevChar = tok.startChar;
        }

        return lsp::SemanticTokens{ std::move(data) };
    }
}
