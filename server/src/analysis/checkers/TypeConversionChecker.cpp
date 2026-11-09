#include "analysis/checkers/TypeConversionChecker.h"

#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"

#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>

namespace angel_lsp::analysis
{

void ProcessCallOrConstructNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(node, ctx.request.nodeIndex);
    TSNode callee = syntax.Callee();
    const std::string calleeName = CleanBaseType(parser::NodeText(callee, request.sourceCode));

    TSNode argsNode = syntax.Field(parser::GetGrammarSymbols().fieldArguments);
    const TSNode soleArgument = SyntaxOperands(argsNode, ctx.request.nodeIndex).OnlyNamed();
    const bool soleArgumentIsLambda = (!ts_node_is_null(soleArgument) &&
                                       ts_node_symbol(soleArgument) == parser::GetGrammarSymbols().symLambdaExpression);

    std::optional<Symbol> calleeFuncdef;
    if (soleArgumentIsLambda)
    {
        calleeFuncdef = FindFuncdef(calleeName, ctx.request.symbolTable);
    }

    const NonInstantiableKind calleeNonInst = ClassifyNonInstantiable(calleeName, ctx.request.symbolTable);
    if (calleeNonInst == NonInstantiableKind::Abstract)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-abstract-instantiated", {calleeName, calleeName});
    }
    else if (calleeNonInst == NonInstantiableKind::Mixin)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-mixin-not-a-type", calleeName);
    }
    else if (calleeFuncdef)
    {
        CheckFuncdefAssignment(node, calleeFuncdef->GetFuncdef(), soleArgument, ctx);
    }
    else
    {
        const DeclaredType target = ReadDeclaredType(callee, ctx);
        if (target.usable && !target.qualifiers.isHandle && IsSameType(calleeName, target.baseName))
        {
            const Scope* scope = ResolveNodeScope(node, request);
            CheckConstruction(argsNode, target.baseName, scope, ctx);
        }
    }
}

static bool ProcessStatementNode(TSSymbol sym, TSNode node, const TypeConversionCheckRequest& request,
                                 DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symExpressionStatement)
    {
        ProcessExpressionStatementNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symForeachStatement)
    {
        ProcessForeachNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symVariableDeclaration)
    {
        ProcessVariableDeclarationNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symReturnStatement)
    {
        ProcessReturnStatementNode(node, request, ctx);
        return true;
    }
    if (sym == gs.symFuncDeclaration)
    {
        CheckConstructorDelegation(node, request.nodeIndex, ctx);
        return true;
    }
    return false;
}

static void ProcessExpressionNode(TSSymbol sym, TSNode node, const TypeConversionCheckRequest& request,
                                  DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symAssignmentExpression)
    {
        ProcessAssignmentNode(node, request, ctx);
    }
    else if (sym == gs.symBinaryExpression)
    {
        ProcessBinaryNode(node, request, ctx);
    }
    else if (sym == gs.symUnaryExpression || sym == gs.symPostfixExpression)
    {
        ProcessUnaryOrPostfixNode(node, request, ctx);
    }
    else if (sym == gs.symMemberExpression)
    {
        ProcessMemberExpressionNode(node, request, ctx);
    }
    else if (sym == gs.symIndexExpression)
    {
        ProcessIndexExpressionNode(node, request, ctx);
    }
    else if (sym == gs.symCastExpression)
    {
        ProcessCastNode(node, request, ctx);
    }
    else if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression)
    {
        ProcessCallOrConstructNode(node, request, ctx);
    }
}

static void ProcessNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symIfStatement || sym == gs.symWhileStatement || sym == gs.symDoWhileStatement ||
        sym == gs.symForStatement)
    {
        ProcessConditionNode(node, request, ctx);
        return;
    }
    if (sym == gs.symTernaryExpression)
    {
        ProcessConditionNode(node, request, ctx);
        ProcessTernaryNode(node, request, ctx);
        return;
    }

    if (!ProcessStatementNode(sym, node, request, ctx))
    {
        ProcessExpressionNode(sym, node, request, ctx);
    }
}

/**
 * @brief Evaluates indexed conversion sites and reports unsupported conversions.
 * @param[in] request Tree, source text and scope tree of the document under analysis.
 * @param[in,out] ctx Diagnostic sink and evaluation context.
 */
void CheckTypeConversions(const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.nodeIndex.Root()) || request.sourceCode.empty())
    {
        return;
    }

    std::optional<BinaryExpressionFacts> localFacts;
    if (!request.binaryFacts)
    {
        localFacts.emplace(BuildBinaryExpressionFacts(request.nodeIndex, request.sourceCode));
    }
    TypeConversionCheckRequest withFacts = request;
    withFacts.binaryFacts = request.binaryFacts ? request.binaryFacts : &*localFacts;

    if (ctx.logger && ctx.logger->IsDebugEnabled())
    {
        ctx.logger->LogDebug(
            fmt::format("[TypeConversionChecker] Running CheckTypeConversions for URI: {}", ctx.request.fileUri));
    }

    const auto& gs = parser::GetGrammarSymbols();
    withFacts.nodeIndex.ForEachNodeOrdered(gs.conversionTargetSymbols,
                                           [&](TSNode node) { ProcessNode(node, withFacts, ctx); });

    if (ctx.logger && ctx.logger->IsTraceEnabled())
    {
        ctx.logger->LogTrace(
            fmt::format("[TypeConversionChecker] Finished CheckTypeConversions for URI: {}", ctx.request.fileUri));
    }
}

} // namespace angel_lsp::analysis
