#include "analysis/index/SyntaxErrorPinpointing.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <cctype>

namespace angel_lsp::analysis
{

void SkipWhitespaceAndComments(std::string_view text, size_t& i)
{
    while (i < text.size())
    {
        if (std::isspace(static_cast<unsigned char>(text[i])))
        {
            ++i;
            continue;
        }
        if (text.substr(i).starts_with("//"))
        {
            const size_t nl = text.find('\n', i + 2);
            i = (nl == std::string_view::npos) ? text.size() : (nl + 1);
            continue;
        }
        if (text.substr(i).starts_with("/*"))
        {
            const size_t end = text.find("*/", i + 2);
            i = (end == std::string_view::npos) ? text.size() : (end + 2);
            continue;
        }
        break;
    }
}

bool IsIdentChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void SkipStringLiteral(std::string_view text, size_t& idx)
{
    if (idx >= text.size())
    {
        return;
    }
    if (text.substr(idx).starts_with("\"\"\""))
    {
        const size_t end = text.find("\"\"\"", idx + 3);
        idx = (end == std::string_view::npos) ? text.size() : (end + 3);
        return;
    }
    const char quote = text[idx++];
    while (idx < text.size() && text[idx] != '\n')
    {
        if (text[idx] == '\\')
        {
            idx += 2;
            continue;
        }
        if (text[idx++] == quote)
        {
            break;
        }
    }
}

TSPoint ComputeLocalPoint(std::string_view sourceCode, TSNode baseNode, size_t targetOffset)
{
    TSPoint pt = ts_node_start_point(baseNode);
    const size_t limit = std::min(targetOffset, sourceCode.size());
    for (size_t i = ts_node_start_byte(baseNode); i < limit; ++i)
    {
        if (sourceCode[i] == '\n')
        {
            ++pt.row;
            pt.column = 0;
        }
        else
        {
            ++pt.column;
        }
    }
    return pt;
}

SourceRange MakeSingleTokenRange(std::string_view sourceCode, TSNode baseNode, size_t tokStart, size_t tokEnd)
{
    const TSPoint pt = ComputeLocalPoint(sourceCode, baseNode, tokStart);
    const uint32_t len = (tokEnd > tokStart) ? static_cast<uint32_t>(tokEnd - tokStart) : 1u;
    return SourceRange{pt.row, pt.column, pt.row, pt.column + len};
}

TSNode FindChildOrParentSymbol(TSNode node, TSSymbol targetSymbol)
{
    if (ts_node_is_null(node) || targetSymbol == 0)
    {
        return TSNode{};
    }
    if (ts_node_symbol(node) == targetSymbol)
    {
        return node;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    TSNode match{};
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_null(child) && ts_node_symbol(child) == targetSymbol)
            {
                match = child;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return match;
}

static bool StartsWithKeyword(std::string_view sourceCode, size_t pos, std::string_view kw) noexcept
{
    if (pos >= sourceCode.size())
    {
        return false;
    }
    const std::string_view rem = sourceCode.substr(pos);
    return rem.starts_with(kw) && (rem.size() == kw.size() || !IsIdentChar(rem[kw.size()]));
}

static TSNode LocateControlHeaderNode(TSNode node, const parser::GrammarSymbols& gs, size_t& kwLen)
{
    auto matchKw = [&](TSNode target) -> TSNode
    {
        if (TSNode n = FindChildOrParentSymbol(target, gs.tokIf); !ts_node_is_null(n))
        {
            kwLen = 2;
            return n;
        }
        if (TSNode n = FindChildOrParentSymbol(target, gs.tokWhile); !ts_node_is_null(n))
        {
            kwLen = 5;
            return n;
        }
        return TSNode{};
    };
    if (TSNode n = matchKw(node); !ts_node_is_null(n))
    {
        return n;
    }
    const TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }
    const TSSymbol ps = ts_node_symbol(parent);
    if (ps == gs.symIfStatement || ps == gs.symWhileStatement)
    {
        kwLen = (ps == gs.symIfStatement) ? 2 : 5;
        return parent;
    }
    return matchKw(parent);
}

static std::optional<PinpointedSyntaxError> TryPinpointMissingRparen(TSNode node, std::string_view sourceCode,
                                                                     const angel_lsp::i18n::I18n* i18n,
                                                                     const parser::GrammarSymbols& gs)
{
    size_t kwLen = 0;
    const TSNode kwNode = ts_node_is_missing(node) ? TSNode{} : LocateControlHeaderNode(node, gs, kwLen);
    if (ts_node_is_null(kwNode))
        return std::nullopt;

    size_t pos = ts_node_start_byte(kwNode) + kwLen;
    SkipWhitespaceAndComments(sourceCode, pos);
    if (pos >= sourceCode.size() || sourceCode[pos] != '(')
        return std::nullopt;

    ++pos;
    for (int parenDepth = 1; pos < sourceCode.size();)
    {
        SkipWhitespaceAndComments(sourceCode, pos);
        if (pos >= sourceCode.size())
            break;
        const char ch = sourceCode[pos];
        if (ch == '"' || ch == '\'')
        {
            SkipStringLiteral(sourceCode, pos);
            continue;
        }
        if (ch == '(' || ch == '[')
            ++parenDepth;
        else if ((ch == ')' || ch == ']') && --parenDepth == 0)
            return std::nullopt;
        else if (parenDepth == 1 && ch == ';')
            return std::nullopt;
        else if (parenDepth == 1 && ch == '{')
        {
            return PinpointedSyntaxError{
                MakeSingleTokenRange(sourceCode, kwNode, pos, pos + 1),
                i18n::FormatMessage(i18n, "note-expected-rparen-before-brace", "Expected ')' before '{'"),
                ts_node_start_byte(node),
                std::max(ts_node_end_byte(node), static_cast<uint32_t>(pos + 1)),
            };
        }
        ++pos;
    }
    return std::nullopt;
}

static std::optional<PinpointedSyntaxError> TryPinpointMissingCatch(TSNode node, std::string_view sourceCode,
                                                                    const angel_lsp::i18n::I18n* i18n,
                                                                    const parser::GrammarSymbols& gs)
{
    TSNode tryStartNode = FindChildOrParentSymbol(node, gs.tokTry);
    if (ts_node_is_null(tryStartNode))
    {
        const TSNode parent = parser::GetParent(node);
        if (!ts_node_is_null(parent) && (ts_node_symbol(parent) == gs.symTryStatement || ts_node_is_error(parent)))
        {
            tryStartNode =
                (ts_node_symbol(parent) == gs.symTryStatement) ? parent : FindChildOrParentSymbol(parent, gs.tokTry);
        }
    }
    if (ts_node_is_null(tryStartNode))
    {
        return std::nullopt;
    }

    size_t pos = ts_node_start_byte(tryStartNode);
    if (!StartsWithKeyword(sourceCode, pos, "try"))
    {
        return std::nullopt;
    }
    pos += 3;
    SkipWhitespaceAndComments(sourceCode, pos);
    if (pos >= sourceCode.size() || sourceCode[pos] != '{')
    {
        return std::nullopt;
    }

    int braceDepth = 1;
    size_t tryCloseBrace = pos++;
    while (pos < sourceCode.size() && braceDepth > 0)
    {
        SkipWhitespaceAndComments(sourceCode, pos);
        if (pos >= sourceCode.size())
        {
            break;
        }
        const char ch = sourceCode[pos];
        if (ch == '"' || ch == '\'')
        {
            SkipStringLiteral(sourceCode, pos);
            continue;
        }
        if (ch == '{')
        {
            ++braceDepth;
        }
        else if (ch == '}' && --braceDepth == 0)
        {
            tryCloseBrace = pos;
        }
        ++pos;
    }
    if (braceDepth != 0)
    {
        return std::nullopt;
    }

    SkipWhitespaceAndComments(sourceCode, pos);
    if (StartsWithKeyword(sourceCode, pos, "catch"))
    {
        return std::nullopt;
    }

    const size_t tokStart = (pos < sourceCode.size()) ? pos : tryCloseBrace;
    size_t tokEnd = tokStart + 1;
    if (pos < sourceCode.size() && IsIdentChar(sourceCode[tokStart]))
    {
        for (tokEnd = tokStart; tokEnd < sourceCode.size() && IsIdentChar(sourceCode[tokEnd]); ++tokEnd)
        {
        }
    }

    return PinpointedSyntaxError{
        MakeSingleTokenRange(sourceCode, tryStartNode, tokStart, tokEnd),
        i18n::FormatMessage(i18n, "note-expected-catch-after-try", "Expected 'catch' after 'try' block"),
        ts_node_start_byte(node),
        std::max(ts_node_end_byte(node), static_cast<uint32_t>(tokEnd)),
    };
}

std::optional<PinpointedSyntaxError> TryPinpointSyntaxError(TSNode node, std::string_view sourceCode,
                                                            const angel_lsp::i18n::I18n* i18n)
{
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (auto rparen = TryPinpointMissingRparen(node, sourceCode, i18n, gs))
    {
        return rparen;
    }
    if (auto colon = TryPinpointMissingTernaryColon(node, sourceCode, i18n))
    {
        return colon;
    }
    return TryPinpointMissingCatch(node, sourceCode, i18n, gs);
}

} // namespace angel_lsp::analysis
