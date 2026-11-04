#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

static bool IsIncompleteOrIgnoredBranchType(std::string_view type)
{
    return type.empty() || type == "auto";
}

static bool AreTernaryBranchesIncompatible(const std::string& clean1, const std::string& clean2, DiagnosticContext& ctx)
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

void ProcessTernaryNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(node, ctx.request.nodeIndex);
    TSNode consequence = syntax.Field(parser::GetGrammarSymbols().fieldConsequence);
    TSNode alternative = syntax.Field(parser::GetGrammarSymbols().fieldAlternative);
    if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    if (const auto dt1 = IsBareDataType(consequence, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(consequence, ctx, *dt1);
        return;
    }
    if (const auto dt2 = IsBareDataType(alternative, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(alternative, ctx, *dt2);
        return;
    }

    const std::string t1 = ResolveExpressionType(consequence, ExpressionTypeContext(scope, ctx));
    const std::string t2 = ResolveExpressionType(alternative, ExpressionTypeContext(scope, ctx));

    const std::string clean1 = CanonicalizeType(CleanExpressionType(t1));
    const std::string clean2 = CanonicalizeType(CleanExpressionType(t2));

    if (IsFunctionReference(consequence, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtNode(consequence, "as-err-no-implicit-conversion",
                       {TrimString(parser::NodeText(consequence, request.sourceCode)), clean2});
        return;
    }
    if (IsFunctionReference(alternative, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtNode(alternative, "as-err-no-implicit-conversion",
                       {TrimString(parser::NodeText(alternative, request.sourceCode)), clean1});
        return;
    }

    if (IsIncompleteOrIgnoredBranchType(clean1) || IsIncompleteOrIgnoredBranchType(clean2))
    {
        return;
    }

    if (AreTernaryBranchesIncompatible(clean1, clean2, ctx))
    {
        ctx.EmitAtNode(alternative, "as-err-no-implicit-conversion", {clean2, clean1});
    }
}

void ProcessExpressionStatementNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode expr = SyntaxOperands(node, ctx.request.nodeIndex).FirstNamed();
    if (ts_node_is_null(expr))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    if (const auto dataTypeName = IsBareDataType(expr, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(expr, ctx, *dataTypeName);
        return;
    }

    if (IsFunctionReference(expr, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtNode(expr, diagnostics::codes::IllegalOperation);
        return;
    }

    const std::string text = TrimString(parser::NodeText(expr, request.sourceCode));
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
                ctx.EmitAtNode(expr, "as-err-undefined-identifier", text);
            }
        }
    }
}

static std::string ForeachValueType(const std::string& containerType, uint32_t variableIndex,
                                    const DiagnosticContext& ctx)
{
    const std::string cleaned = CleanExpressionType(containerType);
    if (cleaned.empty())
    {
        return {};
    }

    const size_t open = cleaned.find('<');
    const std::string bare{LastScopeSegment((open == std::string::npos) ? cleaned : cleaned.substr(0, open))};

    const TemplateBinding binding = BindTemplateArguments(cleaned, ctx.request.symbolTable);
    const std::string method = "opForValue" + std::to_string(variableIndex);

    for (const std::string& candidate : GetInheritedTypeHierarchy(bare, ctx.request.symbolTable))
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

            std::string returnType = std::get<FunctionSignature>(overload.signature).returnType;
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

    return {};
}

static void InferForeachVariables(TSNode node, const std::string& containerType,
                                  const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    uint32_t variableIndex = 0;
    SyntaxOperands(node, ctx.request.nodeIndex)
        .ForEachNamedOperands(
            [&](const SyntaxOperands& child)
            {
                if (child.Symbol() != parser::GetGrammarSymbols().symForeachVariable)
                {
                    return true;
                }

                TSNode nameNode = child.Field(parser::GetGrammarSymbols().fieldName);
                const std::string valueType = ForeachValueType(containerType, variableIndex, ctx);
                ++variableIndex;

                if (ts_node_is_null(nameNode) || valueType.empty())
                {
                    return true;
                }

                const TSPoint namePoint = ts_node_start_point(nameNode);
                const Scope* bodyScope = FindEnclosingScope(request.scopeRoot, namePoint.row, namePoint.column);
                const LocalDefinition* def = ResolveInScope(
                    bodyScope ? bodyScope : FindInnermostScope(request.scopeRoot, namePoint.row, namePoint.column),
                    parser::NodeText(nameNode, request.sourceCode));

                if (def && CleanBaseType(def->typeName) == "auto")
                {
                    const_cast<LocalDefinition*>(def)->typeName = valueType;
                }
                return true;
            });
}

void ProcessForeachNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode collection = SyntaxOperands(node, ctx.request.nodeIndex).Field(parser::GetGrammarSymbols().fieldCollection);
    if (ts_node_is_null(collection))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    const std::string containerType = ResolveExpressionType(collection, ExpressionTypeContext(scope, ctx));

    const std::string containerBase = CleanExpressionType(containerType);
    if (IsCorePrimitive(containerBase) && containerBase != "auto" && containerBase != "void")
    {
        ctx.EmitAtNode(collection, "as-err-invalid-foreach-container", containerBase);
    }

    if (!containerType.empty() && request.mutableScopeRoot)
    {
        InferForeachVariables(node, containerType, request, ctx);
    }
}

} // namespace angel_lsp::analysis
