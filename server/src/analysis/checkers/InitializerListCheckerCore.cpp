#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
static std::string Trimmed(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

/**
 * @brief Strips the decorations that do not change which values a type accepts.
 *        `const`, `&` and `@` all leave the underlying type - and therefore its list pattern -
 */
std::string StripDecorations(std::string_view raw)
{
    std::string_view sv = raw;
    for (;;)
    {
        std::string_view prev = sv;
        sv = StripLeadingConst(sv);
        sv = StripTrailingDecorationsView(sv);
        if (sv == prev)
        {
            break;
        }
    }

    std::string compact;
    compact.reserve(sv.size());
    for (char c : sv)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
            compact += c;
    }
    return compact;
}

TemplateSpelling ReadTemplateSpelling(const std::string& type)
{
    TemplateSpelling spelling;

    const size_t open = type.find('<');
    if (open == std::string::npos || !type.ends_with('>'))
    {
        spelling.name = type;
        return spelling;
    }

    spelling.name = type.substr(0, open);

    const std::string inner = type.substr(open + 1, type.size() - open - 2);
    int depth = 0;
    std::string current;
    for (char c : inner)
    {
        if (c == '<')
        {
            ++depth;
        }
        else if (c == '>')
        {
            --depth;
        }
        else if (c == ',' && depth == 0)
        {
            spelling.arguments.push_back(Trimmed(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty())
    {
        spelling.arguments.push_back(Trimmed(current));
    }
    return spelling;
}

/**
 * @brief Builds a list of candidate container keys for a symbol name lookup.
 * @param[in] sym          Target symbol.
 * @param[in] fallbackName Fallback type name.
 * @return Ordered list of unique candidate container keys.
 */
std::vector<std::string> CandidateContainerKeys(const Symbol& sym, std::string_view fallbackName)
{
    std::vector<std::string> containers;
    if (!sym.qualifiedName.empty())
    {
        containers.push_back(sym.qualifiedName);
    }
    if (!sym.name.empty() && sym.name != sym.qualifiedName)
    {
        containers.push_back(sym.name);
    }
    if (!fallbackName.empty() && fallbackName != sym.name && fallbackName != sym.qualifiedName)
    {
        containers.emplace_back(fallbackName);
    }
    return containers;
}

/**
 * @brief How many values a list writes, counting the ones that were left out.
 */
uint32_t ListValueCount(TSNode listNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    uint32_t separators = 0;
    uint32_t written = 0;
    parser::ForEachChildNode(listNode,
                             [&](TSNode child)
                             {
                                 const TSSymbol sym = ts_node_symbol(child);
                                 if (sym == gs.tokComma)
                                 {
                                     ++separators;
                                 }
                                 else if (ts_node_is_named(child) && sym != gs.symComment)
                                 {
                                     ++written;
                                 }
                             });

    if (separators > 0)
    {
        return separators + 1;
    }
    return written > 0 ? 1 : 0;
}

void CheckElementValue(TSNode element, const std::string& wanted, DiagnosticContext& ctx,
                       const ElementContext& elements)
{
    if (wanted.empty() || wanted == "?")
    {
        return;
    }

    const std::string actual = ResolveExpressionType(element, ExpressionTypeContext(elements.scopeRoot, ctx));
    if (actual.empty())
    {
        return;
    }

    if (!CanConvertImplicitly(actual, wanted, ctx))
    {
        const TSPoint start = ts_node_start_point(element);
        const TSPoint end = ts_node_end_point(element);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-no-implicit-conversion",
                        StripDecorations(actual), StripDecorations(wanted));
    }
}
} // namespace angel_lsp::analysis
