#include "analysis/resolution/ExpressionTypeTernary.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

static std::string ResolveTernaryHierarchy(const TernaryBranchTypes& types, const SymbolTable& symbolTable)
{
    auto h1 = GetInheritedTypeHierarchy(types.c1, symbolTable);
    for (const auto& b : h1)
    {
        if (CanonicalizeType(CleanBaseType(b)) == types.c2)
        {
            return std::string(types.t2);
        }
    }
    auto h2 = GetInheritedTypeHierarchy(types.c2, symbolTable);
    for (const auto& b : h2)
    {
        if (CanonicalizeType(CleanBaseType(b)) == types.c1)
        {
            return std::string(types.t1);
        }
    }
    if (HasConversionMethod(types.c1, types.c2, symbolTable))
    {
        return std::string(types.t2);
    }
    if (HasConversionMethod(types.c2, types.c1, symbolTable))
    {
        return std::string(types.t1);
    }
    return "";
}

static std::string ResolveTernaryNumericPromotion(std::string_view c1, std::string_view c2)
{
    if (c1 == parser::primitives::Double || c2 == parser::primitives::Double)
        return std::string(parser::primitives::Double);
    if (c1 == parser::primitives::Float || c2 == parser::primitives::Float)
        return std::string(parser::primitives::Float);
    if (c1 == "int64" || c2 == "int64")
        return "int64";
    if (c1 == "uint64" || c2 == "uint64")
        return "uint64";
    if (c1 == parser::primitives::UInt || c2 == parser::primitives::UInt)
        return std::string(parser::primitives::UInt);
    return std::string(parser::primitives::Int);
}

static std::string ResolveTernaryEnumPromotion(std::string_view c1, std::string_view c2, const SymbolTable& symbolTable)
{
    if (ResolvesToEnum(c1, symbolTable) && IsIntegerPrimitive(c2))
        return std::string(c2);
    if (ResolvesToEnum(c2, symbolTable) && IsIntegerPrimitive(c1))
        return std::string(c1);
    return "";
}

static std::string ResolveTernaryNullBranch(std::string_view t1, std::string_view t2, std::string_view c1,
                                            std::string_view c2)
{
    if (parser::primitives::IsNullType(c1) && IsHandleTypeView(t2))
    {
        return std::string(t2);
    }
    if (parser::primitives::IsNullType(c2) && IsHandleTypeView(t1))
    {
        return std::string(t1);
    }
    if (parser::primitives::IsNullType(c1) && parser::primitives::IsNullType(c2))
    {
        return std::string(parser::keywords::Null);
    }
    return {};
}

std::string ResolveTernaryBranchTypes(std::string_view t1, std::string_view t2, const SymbolTable& symbolTable)
{
    std::string clean1 = CleanExpressionType(t1);
    std::string clean2 = CleanExpressionType(t2);
    if (!clean1.empty() && clean1 == clean2)
    {
        return clean1;
    }

    std::string c1 = CanonicalizeType(CleanBaseType(t1));
    std::string c2 = CanonicalizeType(CleanBaseType(t2));
    if (!c1.empty() && c1 == c2)
    {
        return (IsHandleTypeView(t1) && IsHandleTypeView(t2)) ? c1 + "@" : c1;
    }
    if (auto nullRes = ResolveTernaryNullBranch(clean1, clean2, c1, c2); !nullRes.empty())
    {
        return nullRes;
    }
    if (IsNumericPrimitive(c1) && IsNumericPrimitive(c2))
    {
        return ResolveTernaryNumericPromotion(c1, c2);
    }
    if (auto enumRes = ResolveTernaryEnumPromotion(c1, c2, symbolTable); !enumRes.empty())
    {
        return enumRes;
    }
    return ResolveTernaryHierarchy(TernaryBranchTypes{t1, t2, c1, c2}, symbolTable);
}

} // namespace angel_lsp::analysis
