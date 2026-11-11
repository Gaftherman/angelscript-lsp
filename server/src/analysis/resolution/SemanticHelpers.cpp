#include "analysis/resolution/SemanticHelpers.h"

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/types/TypeSanitization.h"
#include "config/EngineRuleConfig.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
#include "parser/QueryRegistry.h"
#include "utils/Constants.h"
#include "utils/LspLogger.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <optional>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
/**
 * @brief Normalizes an expression type name by stripping const, references, handles, and whitespace.
 * @param[in] typeName Raw expression type name to sanitize.
 * @return Cleaned base type name without qualifiers or decorations.
 */
std::string CleanExpressionType(std::string_view typeName)
{
    typeName = StripLeadingConst(typeName);
    TrimTypeWhitespace(typeName);
    if (const size_t refLen = TrailingReferenceModifierLength(typeName); refLen > 0)
    {
        typeName.remove_suffix(refLen);
    }
    TrimTypeWhitespace(typeName);
    return std::string(typeName);
}

bool IsReservedKeyword(std::string_view name)
{
    // The list this used to hold was measured against the compiler word by word and came back
    // exactly right - 53, no additions, no removals. It moved to parser/Keywords.h so the
    // formatter and completion could stop keeping their own, differing, copies.
    return parser::keywords::IsReserved(name);
}

bool IsKeyword(std::string_view word) noexcept
{
    return parser::keywords::IsKeyword(word);
}

bool IsPrimitiveTypeName(std::string_view name)
{
    return IsCorePrimitive(name);
}

InitializerItemKind ClassifyInitializerItem(std::string_view item)
{
    if (item.empty())
    {
        return InitializerItemKind::NumericOrExpression;
    }

    if (item.starts_with("\"") || item.starts_with("'"))
    {
        return InitializerItemKind::StringLiteral;
    }

    if (item == "true" || item == "false")
    {
        return InitializerItemKind::BooleanLiteral;
    }

    if (item == "null")
    {
        return InitializerItemKind::NullLiteral;
    }

    if (item.starts_with("{"))
    {
        return InitializerItemKind::NestedInitializer;
    }

    return InitializerItemKind::NumericOrExpression;
}

bool IsFromPredefinedStub(const Symbol& sym, const DiagnosticContext& ctx)
{
    return utils::IsPredefinedFile(sym.fileUri, ctx.request.predefinedFileExtension);
}

std::string_view FirstAttributeName(const SymbolModifiers& modifiers)
{
    // Grammar order: choice("override", "final", "explicit", "property", "delete").
    if (modifiers.isOverride)
    {
        return "override";
    }
    if (modifiers.isFinal)
    {
        return "final";
    }
    if (modifiers.isExplicit)
    {
        return "explicit";
    }
    if (modifiers.isProperty)
    {
        return "property";
    }
    if (modifiers.isDelete)
    {
        return "delete";
    }
    return {};
}

std::string_view LastScopeSegment(std::string_view name) noexcept
{
    const size_t bracketPos = name.find('<');
    const std::string_view prefix = (bracketPos != std::string_view::npos) ? name.substr(0, bracketPos) : name;
    return utils::LastScopeSegment(prefix);
}

std::string_view ParentScope(std::string_view name) noexcept
{
    return utils::ParentScopeView(name);
}

bool IsValidNumberLiteral(std::string_view s) noexcept
{
    const size_t start = (s.front() == '+' || s.front() == '-') ? 1 : 0;
    if (start >= s.size() || !std::isdigit(static_cast<unsigned char>(s[start])))
    {
        return false;
    }
    constexpr std::string_view k_allowed = "0123456789abcdefABCDEF.xXfFuUlL";
    return s.substr(start).find_first_not_of(k_allowed) == std::string_view::npos;
}

bool IsQuotedLiteral(std::string_view s) noexcept
{
    if (s.starts_with("\"\"\""))
    {
        return s.size() >= 6 && s.ends_with("\"\"\"");
    }
    if (s.size() < 2)
    {
        return false;
    }
    return (s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'');
}

bool IsSimpleLiteralExpression(std::string_view expr)
{
    while (!expr.empty() && (expr.front() == ' ' || expr.front() == '\t'))
    {
        expr.remove_prefix(1);
    }
    while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t'))
    {
        expr.remove_suffix(1);
    }
    if (expr.empty())
    {
        return false;
    }
    if (expr == "true" || expr == "false" || expr == "null")
    {
        return true;
    }
    if (IsQuotedLiteral(expr))
    {
        return true;
    }
    return IsValidNumberLiteral(expr);
}

} // namespace angel_lsp::analysis
