#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <string_view>
#include <utility>

namespace angel_lsp::analysis
{

bool CanTriviallyConvert(std::string_view from, std::string_view to, const DiagnosticContext& ctx)
{
    if (HasEmptyType(from, to) || IsSameType(from, to))
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

bool CanConvertBuiltins(std::string_view from, std::string_view to, const DiagnosticContext& ctx,
                        bool allowUnstubbedString)
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

    if (allowUnstubbedString && IsStringType(to, ctx) && !FindTypeDeclaration(to, ctx.request.symbolTable).found)
    {
        return IsNumericPrimitive(from) || from == "bool";
    }

    return false;
}

bool IsUnresolvedOrExternalType(std::string_view typeName, bool isBuiltIn, const SymbolTable& table)
{
    if (isBuiltIn || typeName == "null" || ResolvesToEnum(typeName, table) || FindFuncdef(typeName, table).has_value())
    {
        return false;
    }
    // Check the container name directly for template instantiations: FindTypeDeclaration strips `<...>`
    // internally and reports true, while CleanBaseTypeView would reduce `array<T>` to `T` and fail visibility.
    const auto decomp = DecomposeTemplateType(typeName);
    if (decomp.isTemplate)
    {
        return !FindTypeDeclaration(decomp.containerName, table).found ||
               !HierarchyIsFullyVisible(decomp.containerName, table);
    }
    if (FindTypeDeclaration(typeName, table).found)
    {
        return !HierarchyIsFullyVisible(CleanBaseTypeView(typeName), table);
    }
    return true;
}

bool CanConvertEnumTarget(std::string_view from, std::string_view to, const SymbolTable& table,
                          const DiagnosticContext& ctx)
{
    const bool fromBuiltIn = IsBuiltInValueType(from, ctx);

    if (IsUnresolvedOrExternalType(from, fromBuiltIn, table))
    {
        return true;
    }
    if (!fromBuiltIn && DeclaresConversionTo(from, to, table, true))
    {
        return true;
    }
    return false;
}

bool CanConvertClasses(std::string_view from, std::string_view to, const DiagnosticContext& ctx, ConversionState& state)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (state.depth >= TypeConversionChecker::MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    const TypeDeclarationInfo toDecl = FindTypeDeclaration(to, table);
    // A bare template name has no arguments to bind, so nothing can be judged; an instantiation is
    // checked through its bound constructors and opAssign overloads.
    if (toDecl.isTemplate && to.find('<') == std::string_view::npos)
    {
        return true;
    }

    TemplateBinding binding;
    if (to.find('<') != std::string_view::npos)
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
            std::string paramType = parameters[0].typeName;
            if (binding.usable)
            {
                paramType = SubstituteTemplateParameters(paramType, binding);
            }
            paramType = NormalizeType(paramType, ctx.request.GetEffectiveArrayTypeName());
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

bool CanConvertUserTypes(std::string_view from, std::string_view to, const DiagnosticContext& ctx,
                         ConversionState& state)
{
    const bool fromBuiltIn = IsBuiltInValueType(from, ctx);
    const bool toBuiltIn = IsBuiltInValueType(to, ctx);
    const SymbolTable& table = ctx.request.symbolTable;

    const bool fromTemplate = from.find('<') != std::string_view::npos;
    const bool toTemplate = to.find('<') != std::string_view::npos;
    if (!fromBuiltIn && !toBuiltIn && !fromTemplate && !toTemplate && IsDerivedFrom(from, to, table))
    {
        return true;
    }
    if (!fromBuiltIn && DeclaresConversionTo(from, to, table, true))
    {
        return true;
    }
    return CanConvertClasses(from, to, ctx, state);
}

bool TypeConversionEvaluator::IsConvertible(std::string_view from, std::string_view to, const DiagnosticContext& ctx,
                                            ConversionState& state)
{
    if (HasEmptyType(from, to))
    {
        return true;
    }
    if (from == "void" || to == "void")
    {
        return false;
    }

    if (state.depth >= TypeConversionChecker::MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    if (CanTriviallyConvert(from, to, ctx))
    {
        return true;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view arrayType = ctx.request.GetEffectiveArrayTypeName();
    const std::string unwrappedFrom = UnwrapTypedef(NormalizeType(from, arrayType), table);
    const std::string unwrappedTo = UnwrapTypedef(NormalizeType(to, arrayType), table);
    const std::string_view uFrom = unwrappedFrom;
    const std::string_view uTo = unwrappedTo;
    if (uFrom == "void" || uTo == "void")
    {
        return false;
    }
    if (CanTriviallyConvert(uFrom, uTo, ctx))
    {
        return true;
    }

    if (uFrom == "null" || CanonicalizeType(uFrom) == "null")
    {
        return !IsPrimitiveTypeName(uTo) && !ResolvesToEnum(uTo, table);
    }

    auto edgeKey = MakeConversionEdgeKey(uFrom, uTo);
    if (!state.visitedEdges.insert(edgeKey).second)
    {
        return false;
    }

    EdgeGuard guard{state.visitedEdges, std::move(edgeKey)};

    const bool fromBuiltIn = IsBuiltInValueType(uFrom, ctx);
    const bool toBuiltIn = IsBuiltInValueType(uTo, ctx);
    if (fromBuiltIn && toBuiltIn)
    {
        return CanConvertBuiltins(uFrom, uTo, ctx, state.depth == 0);
    }

    if (ResolvesToEnum(uFrom, table) && IsNumericPrimitive(CanonicalizeType(uTo)))
    {
        return true;
    }

    if (ResolvesToEnum(uTo, table))
    {
        return CanConvertEnumTarget(uFrom, uTo, table, ctx);
    }

    if (const auto fromFd = FindFuncdef(uFrom, table))
    {
        if (const auto toFd = FindFuncdef(uTo, table))
        {
            return AreFuncdefSignaturesCompatible(fromFd->GetFuncdef(), toFd->GetFuncdef(), table);
        }
    }

    if (IsSameType(uFrom, uTo))
    {
        return true;
    }

    if (uFrom.find('<') != std::string_view::npos && uTo.find('<') != std::string_view::npos &&
        AreIncompatibleTemplateTypes(uFrom, uTo))
    {
        return false;
    }

    if (IsUnresolvedOrExternalType(uFrom, fromBuiltIn, table) || IsUnresolvedOrExternalType(uTo, toBuiltIn, table))
    {
        return true;
    }

    if (CanConvertUserTypes(uFrom, uTo, ctx, state))
    {
        return true;
    }

    if (AreIncompatibleTemplateTypes(uFrom, uTo))
    {
        return false;
    }

    return false;
}

bool IsConvertible(std::string_view from, std::string_view to, const DiagnosticContext& ctx, size_t depth)
{
    ConversionState state;
    state.depth = depth;
    return TypeConversionEvaluator::IsConvertible(from, to, ctx, state);
}

bool CanConvertImplicitly(std::string_view fromType, std::string_view toType, const DiagnosticContext& ctx)
{
    return IsConvertible(CleanBaseType(fromType), CleanBaseType(toType), ctx);
}

} // namespace angel_lsp::analysis
