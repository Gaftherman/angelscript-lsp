#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "parser/queries/BuiltQueries.h"
#include <algorithm>
#include <cstring>
#include <ankerl/unordered_dense.h>
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
            Type_Decorator = 22,

            /**
             * @brief The `<` and `>` that delimit a template argument list.
             *
             * Not a standard LSP token type; contributed by the client (see package.json's
             * `semanticTokenTypes` / `semanticTokenScopes`). It exists because TextMate cannot tell
             * these apart from the shift operators: `angelscript.tmLanguage.json`'s operator rule
             * matches `>>` unconditionally, so the closing brackets of `array<array<int>>` were
             * scoped `keyword.operator` - as one two-character shift, rather than two separate
             * closers. The grammar has no way to know better; this pass does, because it is looking
             * at a parse tree where those characters belong to a `template_type_list`.
             *
             * Emitting the token is the whole fix. It previously `continue`d here, which left
             * nothing for the client to override the TextMate scope with.
             */
            Type_TemplatePunctuation = 23
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

        /**
         * @brief Packs a position into one key, so reference kinds can be looked up by where they are.
         */
        constexpr uint64_t PositionKey(uint32_t line, uint32_t character)
        {
            return (static_cast<uint64_t>(line) << 32) | character;
        }

        /**
         * @brief Resolves every identifier reference in the scope tree to what it declares.
         *
         * Same walk SemanticAnalyzer uses for its unused-variable pass: each reference is already
         * stored in the scope that contains it, so ResolveInScope answers directly and no scope
         * lookup by position is needed. Member accesses are skipped - resolving "obj.field" needs
         * the type of "obj", which is a different question from the one asked here.
         */
        void CollectReferenceKinds(const analysis::Scope *scope,
                                   ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind> &out)
        {
            for (const auto &ref : scope->references)
            {
                if (ref.isMemberAccess)
                {
                    continue;
                }

                if (const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, ref.name))
                {
                    out[PositionKey(ref.startLine, ref.startCharacter)] = def->kind;
                }
            }

            for (const auto &child : scope->children)
            {
                CollectReferenceKinds(child.get(), out);
            }
        }

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

        [[nodiscard]] inline bool IsTemplatePunctuationNode(TSNode node) noexcept
        {
            TSNode parent = ts_node_parent(node);
            if (ts_node_is_null(parent))
            {
                return false;
            }

            std::string_view parentType = ts_node_type(parent);
            if (parentType == "type_arguments" ||
                parentType == "template_type" ||
                parentType == "template_type_list" ||
                parentType == "cast_expression" ||
                parentType == "template_parameter_list")
            {
                return true;
            }

            TSNode grandParent = ts_node_parent(parent);
            if (!ts_node_is_null(grandParent))
            {
                std::string_view grandParentType = ts_node_type(grandParent);
                if (grandParentType == "type_arguments" ||
                    grandParentType == "template_type" ||
                    grandParentType == "template_type_list" ||
                    grandParentType == "cast_expression" ||
                    grandParentType == "template_parameter_list")
                {
                    if (parentType != "binary_expression" &&
                        parentType != "assignment_expression" &&
                        parentType != "unary_expression" &&
                        parentType != "postfix_expression")
                    {
                        return true;
                    }
                }
            }

            return false;
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
                "decorator",
                "templatePunctuation"
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

        static TSQuery *s_highlightsQuery = []() -> TSQuery *
        {
            const TSLanguage *lang = tree_sitter_angelscript();
            uint32_t errorOffset = 0;
            TSQueryError errorType = TSQueryErrorNone;
            return ts_query_new(lang, parser::queries::HIGHLIGHTS_QUERY,
                                static_cast<uint32_t>(strlen(parser::queries::HIGHLIGHTS_QUERY)),
                                &errorOffset, &errorType);
        }();

        TSQuery *query = s_highlightsQuery;
        if (!query)
        {
            return lsp::SemanticTokens{};
        }

        // Resolved once for the whole document rather than per identifier: the map is keyed by
        // position, and every reference is looked up in the scope that already holds it.
        ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind> referenceKinds;
        if (request.scopeRoot)
        {
            CollectReferenceKinds(request.scopeRoot.get(), referenceKinds);
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
            else if (captureName == "template.list")
            {
                tokenType = Type_TemplatePunctuation;
                priority = 4;
            }
            else if (captureName == "operator" || captureName == "punctuation.special" || captureName == "punctuation.bracket")
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

            // The query can only say "this is an identifier being used". What it is a use OF comes
            // from the scope tree: a parameter reads as a parameter, a class field as a property.
            if (tokenType == Type_Variable)
            {
                const auto resolved = referenceKinds.find(PositionKey(startPoint.row, startPoint.column));
                if (resolved != referenceKinds.end())
                {
                    switch (resolved->second)
                    {
                        case analysis::LocalDefinitionKind::Parameter: tokenType = Type_Parameter; break;
                        case analysis::LocalDefinitionKind::Field: tokenType = Type_Property; break;
                        case analysis::LocalDefinitionKind::Function: tokenType = Type_Function; break;
                        case analysis::LocalDefinitionKind::Method: tokenType = Type_Method; break;
                        case analysis::LocalDefinitionKind::Type: tokenType = Type_Type; break;
                        case analysis::LocalDefinitionKind::Constant: tokenType = Type_EnumMember; break;
                        default: break;
                    }
                }
            }

            // A template argument list is captured whole, but only its two brackets are tokens: the
            // types inside already have their own, better ones. Emitting the span would paint
            // `array<int>` a single colour and lose the `int`.
            if (tokenType == Type_TemplatePunctuation)
            {
                rawTokens.push_back(RawToken{ startPoint.row, startPoint.column, 1, tokenType, tokenMod, priority });
                if (endPoint.column > 0)
                {
                    rawTokens.push_back(RawToken{ endPoint.row, endPoint.column - 1, 1, tokenType, tokenMod, priority });
                }
                continue;
            }

            if (startPoint.row == endPoint.row)
            {
                if (endPoint.column > startPoint.column)
                {
                    // A `<` or `>` the query reached as an operator, in a position where it is not
                    // one. Rare - the grammar usually resolves this through its external scanner -
                    // but a mis-parse should not repaint a bracket as arithmetic.
                    if (tokenType == Type_Operator && IsTemplatePunctuationNode(node))
                    {
                        tokenType = Type_TemplatePunctuation;
                    }

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

        // Narrowed after de-duplication rather than before, so a ranged request and a full request
        // resolve every overlap identically. Filtering the raw tokens first would let a token that
        // loses an overlap in the full document win it in a range that excludes its competitor.
        if (request.range.has_value())
        {
            const lsp::Range &range = *request.range;
            std::erase_if(filteredTokens, [&range](const RawToken &tok)
            {
                if (tok.line < range.start.line || tok.line > range.end.line)
                {
                    return true;
                }
                if (tok.line == range.start.line && tok.startChar + tok.length <= range.start.character)
                {
                    return true;
                }
                if (tok.line == range.end.line && tok.startChar >= range.end.character)
                {
                    return true;
                }
                return false;
            });
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

    std::vector<lsp::SemanticTokensEdit> ComputeSemanticTokensDelta(const std::vector<lsp::uint> &previous,
                                                                     const std::vector<lsp::uint> &current)
    {
        if (previous == current)
        {
            return {};
        }

        // Longest common prefix, then longest common suffix over what is left. One splice between
        // the two describes the change, which is what an edit confined to a few lines actually is.
        size_t prefix = 0;
        const size_t shortest = std::min(previous.size(), current.size());
        while (prefix < shortest && previous[prefix] == current[prefix])
        {
            ++prefix;
        }

        size_t suffix = 0;
        while (suffix < shortest - prefix &&
               previous[previous.size() - 1 - suffix] == current[current.size() - 1 - suffix])
        {
            ++suffix;
        }

        lsp::SemanticTokensEdit edit;
        edit.start = static_cast<lsp::uint>(prefix);
        edit.deleteCount = static_cast<lsp::uint>(previous.size() - prefix - suffix);

        if (current.size() - prefix - suffix > 0)
        {
            edit.data = lsp::Array<lsp::uint>(current.begin() + static_cast<std::ptrdiff_t>(prefix),
                                              current.end() - static_cast<std::ptrdiff_t>(suffix));
        }

        return { std::move(edit) };
    }
}
