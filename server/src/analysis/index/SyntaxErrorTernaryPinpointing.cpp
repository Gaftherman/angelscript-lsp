#include "analysis/index/SyntaxErrorPinpointing.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>

namespace angel_lsp::analysis
{

static bool IsOpChar(char ch) noexcept
{
    return ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || ch == '=' || ch == '!' || ch == '~' ||
           ch == '<' || ch == '>' || ch == '&' || ch == '|' || ch == '^';
}

static TSNode LocateTernaryQuestionNode(TSNode node, const parser::GrammarSymbols& gs)
{
    TSNode qmarkNode = FindChildOrParentSymbol(node, gs.tokQmark);
    if (!ts_node_is_null(qmarkNode))
    {
        return qmarkNode;
    }
    const TSNode parent = parser::GetParent(node);
    if (!ts_node_is_null(parent) && (ts_node_symbol(parent) == gs.symTernaryExpression || ts_node_is_error(parent)))
    {
        return FindChildOrParentSymbol(parent, gs.tokQmark);
    }
    return TSNode{};
}

std::optional<PinpointedSyntaxError> TryPinpointMissingTernaryColon(TSNode node, std::string_view sourceCode,
                                                                    const angel_lsp::i18n::I18n* i18n)
{
    if (ts_node_is_null(node) || ts_node_is_missing(node))
    {
        return std::nullopt;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode qmarkNode = LocateTernaryQuestionNode(node, gs);
    if (ts_node_is_null(qmarkNode))
    {
        return std::nullopt;
    }

    auto emitAt = [&](size_t tokStart, size_t tokEnd)
    {
        return PinpointedSyntaxError{
            MakeSingleTokenRange(sourceCode, qmarkNode, tokStart, tokEnd),
            i18n::FormatMessage(i18n, "note-expected-colon-in-ternary", "Expected ':' in ternary expression"),
            ts_node_start_byte(node),
            std::max(ts_node_end_byte(node), static_cast<uint32_t>(tokEnd)),
        };
    };

    TernaryScanState st;
    for (size_t pos = ts_node_end_byte(qmarkNode); pos < sourceCode.size();)
    {
        SkipWhitespaceAndComments(sourceCode, pos);
        if (pos >= sourceCode.size())
        {
            break;
        }
        const size_t tokStart = pos;
        const char ch = sourceCode[pos];
        const bool atTop = (st.depth == 0 && st.nestedTernary == 0);

        if (ch == '"' || ch == '\'')
        {
            SkipStringLiteral(sourceCode, pos);
            if (atTop && !st.expectingOperand)
            {
                return emitAt(tokStart, tokStart + 1);
            }
            st.expectingOperand = false;
        }
        else if (ch == '(' || ch == '[' || ch == '{')
        {
            if (ch == '{' && atTop && !st.expectingOperand)
            {
                return emitAt(tokStart, tokStart + 1);
            }
            ++st.depth;
            ++pos;
            st.expectingOperand = true;
        }
        else if (ch == ')' || ch == ']' || ch == '}')
        {
            if (st.depth == 0)
            {
                return (st.nestedTernary == 0 && !st.expectingOperand)
                           ? std::optional<PinpointedSyntaxError>(emitAt(tokStart, tokStart + 1))
                           : std::nullopt;
            }
            --st.depth;
            ++pos;
            st.expectingOperand = false;
        }
        else if (st.depth == 0 && ch == '?')
        {
            ++st.nestedTernary;
            ++pos;
            st.expectingOperand = true;
        }
        else if (st.depth == 0 && ch == ':')
        {
            if (pos + 1 < sourceCode.size() && sourceCode[pos + 1] == ':')
            {
                pos += 2;
                st.expectingOperand = true;
            }
            else if (st.nestedTernary-- > 0)
            {
                ++pos;
                st.expectingOperand = true;
            }
            else
            {
                return std::nullopt;
            }
        }
        else if (atTop && (ch == ';' || ch == ','))
        {
            return st.expectingOperand ? std::nullopt
                                       : std::optional<PinpointedSyntaxError>(emitAt(tokStart, tokStart + 1));
        }
        else if (IsIdentChar(ch))
        {
            while (pos < sourceCode.size() && IsIdentChar(sourceCode[pos]))
            {
                ++pos;
            }
            const std::string_view word = sourceCode.substr(tokStart, pos - tokStart);
            if (word == "and" || word == "or" || word == "xor" || word == "is" || word == "not")
            {
                st.expectingOperand = true;
            }
            else if (atTop && !st.expectingOperand)
            {
                return emitAt(tokStart, pos);
            }
            else
            {
                st.expectingOperand = false;
            }
        }
        else if (ch == '.' || ch == '@')
        {
            ++pos;
            if (ch == '@' && pos < sourceCode.size() && sourceCode[pos] == '=')
            {
                ++pos;
                st.expectingOperand = true;
            }
            else
            {
                st.expectingOperand = (ch == '.');
            }
        }
        else if ((ch == '+' || ch == '-') && pos + 1 < sourceCode.size() && sourceCode[pos + 1] == ch)
        {
            pos += 2;
        }
        else if (IsOpChar(ch))
        {
            if (atTop && !st.expectingOperand &&
                (ch == '~' || (ch == '!' && (pos + 1 >= sourceCode.size() || sourceCode[pos + 1] != '='))))
            {
                return emitAt(tokStart, tokStart + 1);
            }
            while (pos < sourceCode.size() && IsOpChar(sourceCode[pos]))
            {
                ++pos;
            }
            st.expectingOperand = true;
        }
        else
        {
            ++pos;
        }
    }
    return std::nullopt;
}

} // namespace angel_lsp::analysis
