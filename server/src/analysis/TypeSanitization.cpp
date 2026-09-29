#include "analysis/TypeSanitization.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>

namespace angel_lsp::analysis
{
void TrimTypeWhitespace(std::string_view& typeName)
{
    while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
    {
        typeName.remove_prefix(1);
    }
    while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t'))
    {
        typeName.remove_suffix(1);
    }
}

void StripLeadingConst(std::string_view& typeName)
{
    if (typeName.starts_with("const "))
    {
        typeName.remove_prefix(6);
    }
}

void StripTrailingDecorations(std::string& result)
{
    bool modified = true;
    while (modified)
    {
        modified = false;
        while (!result.empty() &&
               (result.back() == '@' || result.back() == '&' || result.back() == ' ' || result.back() == '\t'))
        {
            result.pop_back();
            modified = true;
        }
        if (result.ends_with(" const"))
        {
            result.resize(result.size() - 6);
            modified = true;
        }
        else if (result.ends_with("[]"))
        {
            result.resize(result.size() - 2);
            modified = true;
        }
        else if (!result.empty() && result.back() == ']')
        {
            const size_t bracket = result.rfind('[');
            if (bracket != std::string::npos)
            {
                result = result.substr(0, bracket);
                modified = true;
            }
        }
    }
}

std::string CleanBaseType(std::string_view typeName, std::string_view arrayTypeName)
{
    TrimTypeWhitespace(typeName);
    StripLeadingConst(typeName);
    TrimTypeWhitespace(typeName);

    std::string result(typeName);
    StripTrailingDecorations(result);

    if (result.starts_with("array<") && result.ends_with(">"))
    {
        const std::string inner = result.substr(6, result.size() - 7);
        return CleanBaseType(inner, arrayTypeName);
    }
    if (!arrayTypeName.empty())
    {
        const std::string prefix = std::string(arrayTypeName) + "<";
        if (result.starts_with(prefix) && result.ends_with(">"))
        {
            const std::string inner = result.substr(prefix.size(), result.size() - prefix.size() - 1);
            return CleanBaseType(inner, arrayTypeName);
        }
    }

    return result;
}

std::string CanonicalizeArrayType(std::string_view typeName, std::string_view arrayTypeName)
{
    std::string s(typeName);

    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '@' || s.back() == '&'))
        s.pop_back();

    if (s.starts_with("const "))
    {
        s = s.substr(6);
    }

    const std::string_view effectiveArrayName = arrayTypeName.empty() ? "array" : arrayTypeName;

    while (s.ends_with("[]"))
    {
        const std::string element = CanonicalizeArrayType(s.substr(0, s.size() - 2), effectiveArrayName);
        s = std::string(effectiveArrayName) + "<" + element + ">";
    }

    return s;
}

std::string MemberOwnerType(std::string_view typeName, std::string_view arrayTypeName)
{
    const std::string canonical = CanonicalizeArrayType(typeName, arrayTypeName);

    if (canonical.ends_with(">"))
    {
        const size_t open = canonical.find('<');
        if (open != std::string::npos && open > 0)
        {
            std::string container = canonical.substr(0, open);
            while (!container.empty() && (container.back() == ' ' || container.back() == '\t'))
            {
                container.pop_back();
            }
            if (!container.empty())
            {
                return container;
            }
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
    return name.find("::") != std::string_view::npos;
}

std::vector<std::string> SplitScopeSegments(std::string_view name)
{
    std::vector<std::string> segments;
    size_t start = 0;
    while (start < name.size())
    {
        const size_t pos = name.find("::", start);
        if (pos == std::string_view::npos)
        {
            segments.emplace_back(name.substr(start));
            break;
        }
        segments.emplace_back(name.substr(start, pos - start));
        start = pos + 2;
    }
    return segments;
}

} // namespace angel_lsp::analysis
