#include "analysis/resolution/ExpressionTypeLiterals.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

std::string ResolveFloatingPointLiteral(std::string_view text)
{
    if (text.find('f') != std::string_view::npos || text.find('F') != std::string_view::npos)
    {
        return "float";
    }
    if (text.find('.') != std::string_view::npos || text.find('e') != std::string_view::npos ||
        text.find('E') != std::string_view::npos)
    {
        return "double";
    }
    return "";
}

std::string ResolveIntegerLiteralSuffix(std::string_view text)
{
    const bool hasU = text.find('u') != std::string_view::npos || text.find('U') != std::string_view::npos;
    const bool hasL = text.find('l') != std::string_view::npos || text.find('L') != std::string_view::npos;
    if (hasU && hasL)
    {
        return "uint64";
    }
    if (hasL || text.ends_with("i64"))
    {
        return "int64";
    }
    if (hasU)
    {
        return "uint";
    }
    return "int";
}

std::string ResolveNumberLiteral(std::string_view text)
{
    const bool isRadixPrefixed =
        text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X' || text[1] == 'b' || text[1] == 'B');

    if (!isRadixPrefixed)
    {
        if (auto fp = ResolveFloatingPointLiteral(text); !fp.empty())
        {
            return fp;
        }
    }
    return ResolveIntegerLiteralSuffix(text);
}

} // namespace angel_lsp::analysis
