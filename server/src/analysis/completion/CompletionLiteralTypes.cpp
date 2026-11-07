#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <cctype>
#include <string>
#include <string_view>

namespace angel_lsp::analysis::completion
{

bool IsNumericLiteralDot(std::string_view prefix, size_t dotPos)
{
    if (dotPos == 0 || !std::isdigit(static_cast<unsigned char>(prefix[dotPos - 1])))
    {
        return false;
    }
    size_t d = dotPos - 1;
    while (d > 0 && std::isdigit(static_cast<unsigned char>(prefix[d - 1])))
    {
        --d;
    }
    return d == 0 || (!std::isalpha(static_cast<unsigned char>(prefix[d - 1])) && prefix[d - 1] != '_');
}

static bool IsNumericStart(std::string_view expr)
{
    if (expr.empty())
    {
        return false;
    }
    if (std::isdigit(static_cast<unsigned char>(expr.front())))
    {
        return true;
    }
    return (expr.front() == '-' || expr.front() == '+') && expr.size() > 1 &&
           std::isdigit(static_cast<unsigned char>(expr[1]));
}

static std::string_view DeduceNumericLiteralType(std::string_view expr)
{
    if (expr.ends_with('f') || expr.ends_with('F'))
    {
        return "float";
    }
    if (expr.find_first_of(".eE") != std::string_view::npos)
    {
        return "double";
    }
    if (expr.ends_with('u') || expr.ends_with('U'))
    {
        return "uint";
    }
    return "int";
}

std::string DeduceLiteralOrNamedType(std::string_view expr, const CompletionQuery& request,
                                     const analysis::Scope* scope)
{
    if (expr.empty())
    {
        return "";
    }
    if (parser::IsQuotedString(expr))
    {
        return std::string(request.symbolTable.GetStringType());
    }
    if (expr == parser::keywords::True || expr == parser::keywords::False)
    {
        return "bool";
    }
    if (IsNumericStart(expr))
    {
        return std::string(DeduceNumericLiteralType(expr));
    }
    return ResolveNamedReceiverType(expr, request, scope);
}

} // namespace angel_lsp::analysis::completion
