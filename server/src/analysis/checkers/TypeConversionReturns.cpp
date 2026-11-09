#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <cctype>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

static void CheckLambdaReturnStatement(TSNode expr, TSNode lambdaNode, const Scope* scope, DiagnosticContext& ctx)
{
    const auto target = FuncdefTargetOfLambda(lambdaNode, ctx.request.symbolTable, ctx.request.sourceCode);
    if (!target)
    {
        return;
    }

    const std::string expected = CleanBaseType(target->GetFuncdef().returnType);
    if (expected == "void")
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-void-return-value");
        return;
    }

    if (!expected.empty())
    {
        if (const auto dataTypeName = IsBareDataType(expr, ExpressionTypeContext(scope, ctx)))
        {
            EmitExpressionIsDataType(expr, ctx, *dataTypeName);
            return;
        }
        if (IsFunctionReference(expr, ExpressionTypeContext(scope, ctx)))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-no-implicit-conversion",
                                {TrimString(std::string(parser::NodeText(expr, ctx.request.sourceCode))), expected});
            return;
        }

        const std::string actual = CleanBaseType(ResolveExpressionType(expr, ExpressionTypeContext(scope, ctx)));
        CheckFloatTruncation(expr, {actual, expected}, scope, ctx);
        if (!actual.empty() && actual != expected && !IsConvertible(actual, expected, ctx))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-no-implicit-conversion", {actual, expected});
        }
    }
}

static bool IsFunctionReturnRef(TSNode funcNode, TSNode retTypeNode, const DiagnosticContext& ctx)
{
    const std::string_view rawRetText = parser::NodeText(retTypeNode, ctx.request.sourceCode);
    if (IsReferenceTypeView(rawRetText))
    {
        return true;
    }
    const auto* index = ctx.request.nodeIndex;
    const auto* fact = index ? index->FindOperandSyntax(funcNode) : nullptr;
    return fact && fact->hasReturnReferenceToken;
}

static void CheckReturnReference(TSNode expr, TSNode funcNode, const Scope* scope, DiagnosticContext& ctx)
{
    if (!scope)
    {
        return;
    }

    std::string exprText(parser::NodeText(expr, ctx.request.sourceCode));
    while (!exprText.empty() && isspace(static_cast<unsigned char>(exprText.front())))
    {
        exprText.erase(exprText.begin());
    }
    while (!exprText.empty() && isspace(static_cast<unsigned char>(exprText.back())))
    {
        exprText.pop_back();
    }

    const LocalDefinition* def = ResolveInScopeAt(scope, exprText, ts_node_start_byte(expr));
    if (!def)
    {
        return;
    }

    const TSPoint funcStart = ts_node_start_point(funcNode);
    const TSPoint funcEnd = ts_node_end_point(funcNode);
    const bool isInsideFunction = (def->startLine > funcStart.row && def->startLine < funcEnd.row) ||
                                  (def->startLine == funcStart.row && def->startCharacter >= funcStart.column);
    if (!isInsideFunction)
    {
        return;
    }

    if (def->kind == LocalDefinitionKind::Parameter)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-cannot-return-param-ref", exprText);
    }
    else if (def->kind == LocalDefinitionKind::Variable)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-cannot-return-local-ref", exprText);
    }
}

static void CheckFuncDeclarationReturnStatement(TSNode expr, TSNode funcNode, const Scope* scope,
                                                DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(funcNode, ctx.request.nodeIndex);
    TSNode retTypeNode = syntax.Field(parser::GetGrammarSymbols().fieldReturnType);
    if (ts_node_is_null(retTypeNode))
    {
        retTypeNode = syntax.Field(parser::GetGrammarSymbols().fieldType);
    }
    if (ts_node_is_null(retTypeNode))
    {
        return;
    }

    if (IsFunctionReturnRef(funcNode, retTypeNode, ctx))
    {
        CheckReturnReference(expr, funcNode, scope, ctx);
    }

    const std::string rawRetText(parser::NodeText(retTypeNode, ctx.request.sourceCode));
    const std::string expected = CleanBaseType(rawRetText);
    if (expected == "void")
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-void-return-value");
        return;
    }

    if (!expected.empty())
    {
        if (const auto dataTypeName = IsBareDataType(expr, ExpressionTypeContext(scope, ctx)))
        {
            EmitExpressionIsDataType(expr, ctx, *dataTypeName);
            return;
        }
        if (IsFunctionReference(expr, ExpressionTypeContext(scope, ctx)))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-no-implicit-conversion",
                                {TrimString(std::string(parser::NodeText(expr, ctx.request.sourceCode))), expected});
            return;
        }

        const std::string rawActual = ResolveExpressionType(expr, ExpressionTypeContext(scope, ctx));
        if (ExpressionTypeCache::IsPoisonType(rawActual))
        {
            return;
        }
        const std::string actual = CleanBaseType(rawActual);
        CheckFloatTruncation(expr, {actual, expected}, scope, ctx);
        if (!actual.empty() && actual != expected && !IsConvertible(actual, expected, ctx))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(expr), "as-err-no-implicit-conversion", {actual, expected});
        }
    }
}

void ProcessReturnStatementNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    TSNode expr = SyntaxOperands(node, ctx.request.nodeIndex).FieldOrNamed(parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(expr))
    {
        return;
    }
    const Scope* scope = ResolveNodeScope(node, request);
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode parent = request.nodeIndex.EnclosingDeclaration(node);
    if (!ts_node_is_null(parent))
    {
        const TSSymbol pSym = ts_node_symbol(parent);
        if (pSym == gs.symLambdaExpression)
        {
            CheckLambdaReturnStatement(expr, parent, scope, ctx);
        }
        else if (pSym == gs.symFuncDeclaration)
        {
            CheckFuncDeclarationReturnStatement(expr, parent, scope, ctx);
        }
    }
}

} // namespace angel_lsp::analysis
