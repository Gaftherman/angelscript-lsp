#include "analysis/TypeSanitization.h"
#include "analysis/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>

namespace angel_lsp::analysis
{

void StripTrailingDecorations(std::string& result)
{
    std::string_view view = StripTrailingDecorationsView(result, true);
    result.resize(view.size());
}

std::string_view CleanBaseTypeView(std::string_view typeName, std::string_view arrayTypeName) noexcept
{
    TrimTypeWhitespace(typeName);
    typeName = StripLeadingConst(typeName);
    TrimTypeWhitespace(typeName);

    typeName = StripTrailingDecorationsView(typeName, true);

    if (typeName.starts_with("array<") && typeName.ends_with('>'))
    {
        const std::string_view inner = typeName.substr(6, typeName.size() - 7);
        return CleanBaseTypeView(inner, arrayTypeName);
    }
    if (!arrayTypeName.empty())
    {
        if (typeName.starts_with(arrayTypeName) && typeName.size() > arrayTypeName.size() + 1 &&
            typeName[arrayTypeName.size()] == '<' && typeName.ends_with('>'))
        {
            const size_t prefixLen = arrayTypeName.size() + 1;
            const std::string_view inner = typeName.substr(prefixLen, typeName.size() - prefixLen - 1);
            return CleanBaseTypeView(inner, arrayTypeName);
        }
    }

    return typeName;
}

std::string CleanBaseType(std::string_view typeName, std::string_view arrayTypeName)
{
    return std::string(CleanBaseTypeView(typeName, arrayTypeName));
}

/**
 * @brief Canonicalize array type.
 * @param[in] typeName Parameter typeName.
 * @param[in] arrayTypeName Parameter arrayTypeName.
 * @return Result of the operation.
 * @note [REFACTOR-CANDIDATE] Manually re-evaluates AST for isHandle; candidate for direct Pass 1 flag read via
 * TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isHandle / LocalDefinition::isHandleType. Manually re-evaluates
 * AST for isConst; candidate for direct Pass 1 flag read via TypeExtraction::ExtractTypeInfo /
 * Symbol::modifiers.isConst.
 */
std::string CanonicalizeArrayType(std::string_view typeName, std::string_view arrayTypeName)
{
    std::string_view sv = StripLeadingConst(typeName);
    sv = StripTrailingDecorationsView(sv, false);
    TrimTypeWhitespace(sv);
    std::string s(sv);

    if (arrayTypeName.empty())
    {
        return s;
    }

    while (s.ends_with("[]"))
    {
        const std::string element = CanonicalizeArrayType(s.substr(0, s.size() - 2), arrayTypeName);
        s = std::string(arrayTypeName) + "<" + element + ">";
    }

    return s;
}

std::string MemberOwnerType(std::string_view typeName, std::string_view arrayTypeName)
{
    const std::string canonical = CanonicalizeArrayType(typeName, arrayTypeName);
    const auto decomp = DecomposeTemplateType(canonical);
    if (decomp.isTemplate)
    {
        std::string_view container = decomp.containerName;
        TrimTypeWhitespace(container);
        if (!container.empty())
        {
            return std::string(container);
        }
    }

    return CleanBaseType(canonical);
}

std::string CanonicalizeType(std::string_view typeName)
{
    std::string clean = CleanExpressionType(typeName);
    if (clean == "int32")
    {
        return "int";
    }
    if (clean == "uint32")
    {
        return "uint";
    }
    if (clean == "short")
    {
        return "int16";
    }
    if (clean == "ushort")
    {
        return "uint16";
    }
    return clean;
}

std::vector<std::string> SplitTemplateArguments(std::string_view inner)
{
    std::vector<std::string> arguments;
    int depth = 0;
    size_t start = 0;

    const auto push = [&](std::string_view piece)
    {
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t'))
            piece.remove_prefix(1);
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t'))
            piece.remove_suffix(1);
        arguments.emplace_back(piece);
    };

    for (size_t i = 0; i < inner.size(); ++i)
    {
        if (inner[i] == '<')
            ++depth;
        else if (inner[i] == '>')
            --depth;
        else if (inner[i] == ',' && depth == 0)
        {
            push(inner.substr(start, i - start));
            start = i + 1;
        }
    }

    if (start <= inner.size())
        push(inner.substr(start));

    return arguments;
}

bool HasScopeQualifier(std::string_view name) noexcept
{
    return utils::ContainsScopeSeparator(name);
}

std::vector<std::string> SplitScopeSegments(std::string_view name)
{
    std::vector<std::string> segments;
    std::string_view cur = name;
    while (!cur.empty())
    {
        const auto [first, rest] = utils::SplitFirstScopeSegment(cur);
        segments.emplace_back(first);
        cur = rest;
    }
    return segments;
}

} // namespace angel_lsp::analysis
