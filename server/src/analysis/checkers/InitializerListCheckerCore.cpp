#include "analysis/checkers/InitializerListCheckerCommon.h"
#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Strips leading and trailing whitespace characters from a string view.
 * @param[in] text Input string view.
 * @return Trimmed string view.
 */
static std::string_view TrimmedView(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::string StripDecorations(std::string_view raw)
{
    std::string_view sv = raw;
    for (;;)
    {
        const std::string_view prev = sv;
        sv = StripLeadingConst(sv);
        sv = StripTrailingDecorationsView(sv);
        if (sv == prev)
        {
            break;
        }
    }

    std::string compact;
    compact.reserve(sv.size());
    for (const char c : sv)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            compact += c;
        }
    }
    return compact;
}

TemplateSpelling ReadTemplateSpelling(std::string_view type)
{
    TemplateSpelling spelling;

    const size_t open = type.find('<');
    if (open == std::string_view::npos || !type.ends_with('>'))
    {
        spelling.name = std::string(type);
        return spelling;
    }

    spelling.name = std::string(type.substr(0, open));

    const std::string_view inner = type.substr(open + 1, type.size() - open - 2);
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < inner.size(); ++i)
    {
        const char c = inner[i];
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
            const std::string_view arg = TrimmedView(inner.substr(start, i - start));
            spelling.arguments.emplace_back(arg);
            start = i + 1;
        }
    }
    if (start < inner.size())
    {
        const std::string_view arg = TrimmedView(inner.substr(start));
        if (!arg.empty())
        {
            spelling.arguments.emplace_back(arg);
        }
    }
    return spelling;
}

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

void CheckElementValue(const InitializerListElementFact& element, std::string_view wanted, DiagnosticContext& ctx,
                       const ElementContext& elements)
{
    if (wanted.empty() || wanted == "?")
    {
        return;
    }

    const std::string actual = ResolveExpressionType(element.bytes, ExpressionTypeContext(elements.scopeRoot, ctx));
    if (actual.empty())
    {
        return;
    }

    if (!CanConvertImplicitly(actual, wanted, ctx))
    {
        ctx.EmitAtRange(element.range, "as-err-no-implicit-conversion", StripDecorations(actual),
                        StripDecorations(wanted));
    }
}

} // namespace angel_lsp::analysis
