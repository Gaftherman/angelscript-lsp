#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

static bool IsMatchingLocalDefinition(const Scope* scope, std::string_view name, TSPoint nameStart)
{
    if (scope == nullptr)
    {
        return false;
    }
    const LocalDefinition* def = ResolveInScope(scope, name);
    return def != nullptr && def->name == name && def->startLine == nameStart.row &&
           def->startCharacter == nameStart.column;
}

static bool IsCyclicAutoDependency(TSNode valueNode, TSNode nameNode, const Scope* scope,
                                   const TypeConversionCheckRequest& request)
{
    const auto varName = parser::GetNodeTextView(nameNode, request.sourceCode);
    if (varName.empty())
    {
        return false;
    }

    const uint32_t end = ts_node_end_byte(valueNode);
    if (end > request.sourceCode.size())
    {
        return false;
    }
    for (const auto& candidate : request.nodeIndex.InitializerIdentifiers(ts_node_start_byte(valueNode), end))
    {
        const auto& [name, scoped] = candidate;
        if (name.endByte > end || name.endByte - name.startByte != varName.size() ||
            request.sourceCode.substr(name.startByte, varName.size()) != varName)
        {
            continue;
        }
        const auto scopedText = request.sourceCode.substr(scoped.startByte, scoped.endByte - scoped.startByte);
        if (!utils::ContainsScopeSeparator(scopedText))
        {
            return !scope || IsMatchingLocalDefinition(scope, varName, ts_node_start_point(nameNode));
        }
    }
    return false;
}

void ProcessAutoDeclarator(const SyntaxOperands& declarator, const TypeConversionCheckRequest& request,
                           DiagnosticContext& ctx)
{
    TSNode nameNode = declarator.Field(parser::GetGrammarSymbols().fieldName);
    const std::string varName = parser::NodeText(nameNode, request.sourceCode);
    TSNode valueNode = declarator.DeclaratorValue(request.sourceCode);

    if (ts_node_is_null(valueNode))
    {
        ctx.EmitAtNode(declarator.Node(), "as-err-auto-requires-initializer");
        return;
    }

    const Scope* scope = ResolveNodeScope(declarator.Node(), request);
    if (IsCyclicAutoDependency(valueNode, nameNode, scope, request))
    {
        ctx.EmitAtNode(valueNode, "as-err-cyclic-auto-dependency", varName);
        return;
    }

    if (const auto dataTypeName = IsBareDataType(valueNode, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(valueNode, ctx, *dataTypeName);
        return;
    }

    const std::string rhsType = ResolveExpressionType(valueNode, ExpressionTypeContext(scope, ctx));

    if (rhsType == "void")
    {
        ctx.EmitAtNode(valueNode, "as-err-cannot-infer-void");
    }
    else if (rhsType == "null")
    {
        ctx.EmitAtNode(valueNode, "as-err-cannot-infer-null");
    }
    else if (!rhsType.empty() && request.mutableScopeRoot && scope)
    {
        const LocalDefinition* def = ResolveInScope(scope, varName);
        if (def && CleanBaseType(def->typeName) == "auto")
        {
            std::string finalType = rhsType;
            if ((def->isHandleType || IsHandleTypeView(def->typeName)) && !IsHandleTypeView(finalType))
            {
                finalType += '@';
            }
            const_cast<LocalDefinition*>(def)->typeName = finalType;
        }
    }
}

void ProcessAutoVariableDeclarators(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    SyntaxOperands(node, ctx.request.nodeIndex)
        .ForEachNamedOperands(
            [&](const SyntaxOperands& child)
            {
                if (child.Symbol() == gs.symVariableDeclarator)
                {
                    ProcessAutoDeclarator(child, request, ctx);
                }
                return true;
            });
}

} // namespace angel_lsp::analysis
