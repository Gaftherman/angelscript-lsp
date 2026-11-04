#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::analysis
{

bool CanTriviallyConvert(const std::string& from, const std::string& to, const DiagnosticContext& ctx)
{
    if (IsPoisonOrEmptyType(from, to) || IsSameType(from, to))
    {
        return true;
    }

    if ((from == "null" || CanonicalizeType(from) == "null") && IsHandleTypeView(to))
    {
        return true;
    }

    if (IsVariableType(to) || IsVariableType(from))
    {
        return true;
    }

    if (from == "auto" || to == "auto")
    {
        return true;
    }

    if (ctx.request.IsRegisteredSymbol(from) || ctx.request.IsRegisteredSymbol(to))
    {
        return true;
    }

    return CanonicalizeType(from) == CanonicalizeType(to);
}

bool CanConvertBuiltins(const std::string& from, const std::string& to, const DiagnosticContext& ctx)
{
    if (from == to)
    {
        return true;
    }
    const bool fromNum = IsNumericPrimitive(from);
    const bool toNum = IsNumericPrimitive(to);
    if (fromNum && toNum)
    {
        return true;
    }
    if ((from == "bool" && toNum) || (fromNum && to == "bool"))
    {
        return false;
    }

    if (IsStringType(to, ctx))
    {
        return true;
    }

    return false;
}

bool CanConvertEnumTarget(const std::string& from, const std::string& to, const SymbolTable& table,
                          const DiagnosticContext& ctx)
{
    const bool fromBuiltIn = IsBuiltInValueType(from, ctx);
    const TypeDeclarationInfo fromDecl = FindTypeDeclaration(from, table);

    if (!fromBuiltIn && !fromDecl.found && !ResolvesToEnum(from, table))
    {
        return true;
    }
    if (!fromBuiltIn && DeclaresConversionTo(from, to, table, true))
    {
        return true;
    }
    return false;
}

bool CanConvertClasses(const std::string& from, const std::string& to, const DiagnosticContext& ctx,
                       ConversionState& state)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (state.depth >= TypeConversionChecker::MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    const TypeDeclarationInfo toDecl = FindTypeDeclaration(to, table);
    if (toDecl.isTemplate)
    {
        return true;
    }

    TemplateBinding binding;
    if (to.find('<') != std::string::npos)
    {
        binding = BindTemplateArguments(to, table);
    }

    bool convertible = false;
    const auto acceptsFrom = [&](const Symbol& sym)
    {
        if (sym.GetFunction().modifiers.isExplicit || sym.GetFunction().modifiers.isDelete)
        {
            return false;
        }
        const auto& parameters = sym.GetFunction().parameters;
        if (AcceptsSingleArgument(parameters))
        {
            ConversionState nextState{state.depth + 1, state.visitedEdges};
            std::string paramType = SingleArgumentType(parameters);
            if (binding.usable)
            {
                paramType = SubstituteTemplateParameters(paramType, binding);
            }
            if (TypeConversionEvaluator::IsConvertible(from, paramType, ctx, nextState))
            {
                convertible = true;
                return true;
            }
        }
        return false;
    };

    ForEachConstructor(to, table, acceptsFrom);
    if (!convertible)
    {
        ForEachMethod(to, "opAssign", table, acceptsFrom);
    }
    return convertible;
}

bool IsUnresolvedOrExternalType(const std::string& typeName, bool isBuiltIn, const SymbolTable& table)
{
    if (!isBuiltIn && !FindTypeDeclaration(typeName, table).found && !ResolvesToEnum(typeName, table))
    {
        return true;
    }
    return ResolvesToNonClassDeclaration(typeName, table);
}

bool CanConvertUserTypes(const ConversionTypePair& types, const DiagnosticContext& ctx, ConversionState& state)
{
    const bool fromBuiltIn = IsBuiltInValueType(types.from, ctx);
    const bool toBuiltIn = IsBuiltInValueType(types.to, ctx);
    const SymbolTable& table = ctx.request.symbolTable;

    if (!fromBuiltIn && !toBuiltIn && IsDerivedFrom(types.from, types.to, table))
    {
        return true;
    }
    if (!fromBuiltIn && DeclaresConversionTo(types.from, types.to, table, true))
    {
        return true;
    }
    return CanConvertClasses(types.from, types.to, ctx, state);
}

bool TypeConversionEvaluator::IsConvertible(const std::string& from, const std::string& to,
                                            const DiagnosticContext& ctx, ConversionState& state)
{
    if (IsPoisonOrEmptyType(from, to))
    {
        return true;
    }

    if (state.depth >= TypeConversionChecker::MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    if (CanTriviallyConvert(from, to, ctx))
    {
        return true;
    }

    std::string edgeKey = from + "->" + to;
    if (!state.visitedEdges.insert(edgeKey).second)
    {
        return false;
    }

    const SymbolTable& table = ctx.request.symbolTable;

    EdgeGuard guard{state.visitedEdges, std::move(edgeKey)};

    const bool fromBuiltIn = IsBuiltInValueType(from, ctx);
    const bool toBuiltIn = IsBuiltInValueType(to, ctx);
    if (fromBuiltIn && toBuiltIn)
    {
        return CanConvertBuiltins(from, to, ctx);
    }

    if (ResolvesToEnum(from, table) && parser::primitives::IsNumeric(CanonicalizeType(to)))
    {
        return true;
    }

    if (ResolvesToEnum(to, table))
    {
        return CanConvertEnumTarget(from, to, table, ctx);
    }

    if (IsSameType(from, to))
    {
        return true;
    }

    if (IsUnresolvedOrExternalType(from, fromBuiltIn, table) || IsUnresolvedOrExternalType(to, toBuiltIn, table))
    {
        return true;
    }

    if (CanConvertUserTypes({from, to}, ctx, state))
    {
        return true;
    }

    if (AreIncompatibleTemplateTypes(from, to))
    {
        return false;
    }

    return false;
}

bool IsConvertible(const std::string& from, const std::string& to, const DiagnosticContext& ctx, size_t depth)
{
    ConversionState state;
    state.depth = depth;
    return TypeConversionEvaluator::IsConvertible(from, to, ctx, state);
}

bool CanConvertImplicitly(const std::string& fromType, const std::string& toType, const DiagnosticContext& ctx)
{
    return IsConvertible(CleanBaseType(fromType), CleanBaseType(toType), ctx);
}

namespace TypeConversionChecker
{
bool evaluateUserConversions(const TypeInfo& source, const TypeInfo& target, size_t depth,
                             ankerl::unordered_dense::set<std::string>& visitedEdges)
{
    if (depth >= MAX_CONVERSION_DEPTH)
    {
        return false;
    }
    if (source == target)
    {
        return true;
    }
    const std::string edgeKey = source.name + "->" + target.name;
    return visitedEdges.contains(edgeKey);
}

bool canConvertImplicitly(const TypeInfo& source, const TypeInfo& target, size_t depth,
                          ankerl::unordered_dense::set<std::string>& visitedEdges)
{
    if (depth >= MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    if (source == target)
    {
        return true;
    }

    std::string edgeKey = source.name + "->" + target.name;
    if (!visitedEdges.insert(edgeKey).second)
    {
        return false;
    }

    bool convertible = evaluateUserConversions(source, target, depth + 1, visitedEdges);
    visitedEdges.erase(edgeKey);
    return convertible;
}
} // namespace TypeConversionChecker

} // namespace angel_lsp::analysis
