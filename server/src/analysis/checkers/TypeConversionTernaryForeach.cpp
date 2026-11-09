#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
#include <variant>

namespace angel_lsp::analysis
{

static bool IsIncompleteOrIgnoredBranchType(std::string_view type)
{
    return type.empty() || type == "auto";
}

static bool AreTernaryBranchesIncompatible(std::string_view clean1, std::string_view clean2, DiagnosticContext& ctx)
{
    if ((clean1 == "null" && IsHandleTypeView(clean2)) || (clean2 == "null" && IsHandleTypeView(clean1)))
    {
        return false;
    }
    if (IsStringType(clean1, ctx) != IsStringType(clean2, ctx))
    {
        return true;
    }
    if (ResolvesToEnum(clean1, ctx.request.symbolTable) && ResolvesToEnum(clean2, ctx.request.symbolTable) &&
        clean1 != clean2)
    {
        return true;
    }
    return !IsConvertible(clean1, clean2, ctx) && !IsConvertible(clean2, clean1, ctx);
}

void ProcessTernarySite(const TernarySiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const ConversionExpressionFact& consequence = site.consequence;
    const ConversionExpressionFact& alternative = site.alternative;

    const Scope* scope = ResolveNodeScope(site.range, request);
    if (const auto dt1 = IsBareDataType(consequence.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(consequence, ctx, *dt1);
        return;
    }
    if (const auto dt2 = IsBareDataType(alternative.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(alternative, ctx, *dt2);
        return;
    }

    const auto t1 = ResolveExpressionType(consequence.bytes, ExpressionTypeContext(scope, ctx));
    const auto t2 = ResolveExpressionType(alternative.bytes, ExpressionTypeContext(scope, ctx));

    const auto clean1 = CanonicalizeType(CleanExpressionType(t1));
    const auto clean2 = CanonicalizeType(CleanExpressionType(t2));

    if (IsFunctionReference(consequence.bytes, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtRange(consequence.range, "as-err-no-implicit-conversion", {TrimString(consequence.text), clean2});
        return;
    }
    if (IsFunctionReference(alternative.bytes, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtRange(alternative.range, "as-err-no-implicit-conversion", {TrimString(alternative.text), clean1});
        return;
    }

    if (IsIncompleteOrIgnoredBranchType(clean1) || IsIncompleteOrIgnoredBranchType(clean2))
    {
        return;
    }

    if (AreTernaryBranchesIncompatible(clean1, clean2, ctx))
    {
        ctx.EmitAtRange(alternative.range, "as-err-no-implicit-conversion", {clean2, clean1});
    }
}

void ProcessExpressionStatementSite(const ExpressionStatementSiteFact& site, const TypeConversionCheckRequest& request,
                                    DiagnosticContext& ctx)
{
    const ConversionExpressionFact& expr = site.expression;

    const Scope* scope = ResolveNodeScope(site.statementRange, request);
    if (const auto dataTypeName = IsBareDataType(expr.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(expr, ctx, *dataTypeName);
        return;
    }

    if (IsFunctionReference(expr.bytes, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtRange(expr.range, diagnostics::codes::IllegalOperation);
        return;
    }

    const std::string_view text = TrimString(expr.text);
    if (!text.empty())
    {
        if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(text))
        {
            bool onlyNs = true;
            for (const auto& s : *syms)
            {
                if (s.type != SymbolType::Namespace)
                {
                    onlyNs = false;
                    break;
                }
            }
            if (onlyNs)
            {
                ctx.EmitAtRange(expr.range, "as-err-undefined-identifier", text);
            }
        }
    }
}

/**
 * @brief Resolves the type a foreach variable receives from the container's `opForValueN` method.
 * @param[in] containerType Resolved type of the iterated expression.
 * @param[in] variableIndex Ordinal of the loop variable, which selects the method suffix.
 * @param[in] ctx Diagnostic context carrying the symbol table.
 * @return Cleaned value type, or an empty string when no inherited `opForValueN` returns a value.
 */
static auto ForeachValueType(std::string_view containerType, uint32_t variableIndex, const DiagnosticContext& ctx)
{
    auto cleaned = CleanExpressionType(containerType);
    if (cleaned.empty())
    {
        return cleaned;
    }

    const std::string_view cleanedView{cleaned};
    const auto bare = LastScopeSegment(cleanedView.substr(0, cleanedView.find('<')));

    const TemplateBinding binding = BindTemplateArguments(cleaned, ctx.request.symbolTable);
    const auto method = "opForValue" + std::to_string(variableIndex);

    for (const auto& candidate : GetInheritedTypeHierarchy(bare, ctx.request.symbolTable))
    {
        const auto overloads = ctx.request.symbolTable.FindMemberSymbolPtr(candidate, method);
        if (!overloads)
        {
            continue;
        }
        for (const Symbol& overload : *overloads)
        {
            if (overload.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(overload.signature))
            {
                continue;
            }

            auto returnType = std::get<FunctionSignature>(overload.signature).returnType;
            if (returnType.empty() || returnType == "void")
            {
                continue;
            }
            if (binding.usable)
            {
                for (size_t i = 0; i < binding.parameters.size(); ++i)
                {
                    returnType = SubstituteTypeParam(returnType, binding.parameters[i], binding.arguments[i]);
                }
            }
            return CleanExpressionType(returnType);
        }
    }

    cleaned.clear();
    return cleaned;
}

/**
 * @brief Writes the inferred value type back into each `auto` foreach variable.
 * @param[in] site Foreach site whose variable slice is visited.
 * @param[in] containerType Resolved type of the iterated expression.
 * @param[in] request Analysis request carrying the scope tree and variable facts.
 * @param[in,out] ctx Diagnostic context.
 */
static void InferForeachVariables(const ForeachSiteFact& site, std::string_view containerType,
                                  const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    for (uint32_t i = 0; i < site.variableCount; ++i)
    {
        const ForeachVariableFact& variable = request.conversionSites->foreachVariables[site.variableBegin + i];
        const auto valueType = ForeachValueType(containerType, variable.ordinal, ctx);
        if (variable.name.empty() || valueType.empty())
        {
            continue;
        }

        const SourceRange& at = variable.nameRange;
        const Scope* bodyScope = FindEnclosingScope(request.scopeRoot, at.startLine, at.startCharacter);
        const LocalDefinition* def = ResolveInScope(
            bodyScope ? bodyScope : FindInnermostScope(request.scopeRoot, at.startLine, at.startCharacter),
            variable.name);

        if (def && CleanBaseType(def->typeName) == "auto")
        {
            const_cast<LocalDefinition*>(def)->typeName = valueType;
        }
    }
}

void ProcessForeachSite(const ForeachSiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(site.statementRange, request);
    const auto containerType = ResolveExpressionType(site.collection.bytes, ExpressionTypeContext(scope, ctx));

    const auto containerBase = CleanExpressionType(containerType);
    if (IsCorePrimitive(containerBase) && containerBase != "auto" && containerBase != "void")
    {
        ctx.EmitAtRange(site.collection.range, "as-err-invalid-foreach-container", containerBase);
    }

    if (!containerType.empty() && request.mutableScopeRoot)
    {
        InferForeachVariables(site, containerType, request, ctx);
    }
}

} // namespace angel_lsp::analysis
