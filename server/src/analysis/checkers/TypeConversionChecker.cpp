#include "analysis/checkers/TypeConversionChecker.h"

#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ConversionSiteFacts.h"
#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"

#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>

namespace angel_lsp::analysis
{

void ProcessCallConstructSite(const CallConstructSiteFact& site, const TypeConversionCheckRequest& request,
                              DiagnosticContext& ctx)
{
    const auto calleeName = CleanBaseType(site.calleeText);

    std::optional<Symbol> calleeFuncdef;
    if (site.soleArgumentIsLambda)
    {
        calleeFuncdef = FindFuncdef(calleeName, ctx.request.symbolTable);
    }

    const NonInstantiableKind calleeNonInst = ClassifyNonInstantiable(calleeName, ctx.request.symbolTable);
    if (calleeNonInst == NonInstantiableKind::Abstract)
    {
        ctx.EmitAtNodeRange(site.range, "as-err-abstract-instantiated", {calleeName, calleeName});
    }
    else if (calleeNonInst == NonInstantiableKind::Mixin)
    {
        ctx.EmitAtNodeRange(site.range, "as-err-mixin-not-a-type", calleeName);
    }
    else if (calleeFuncdef)
    {
        CheckFuncdefAssignment(site.range, calleeFuncdef->GetFuncdef(), site.soleArgumentFuncdef, ctx);
    }
    else
    {
        const DeclaredType target = ReadDeclaredType(site.calleeText, ctx);
        if (target.usable && !target.qualifiers.isHandle && IsSameType(calleeName, target.baseName))
        {
            const Scope* scope = ResolveNodeScope(site.range, request);
            CheckConstruction(site.argument, target.baseName, scope, ctx);
        }
    }
}

/**
 * @brief Reads the byte span of a node, which keys the indexed site lookups.
 * @param[in] node Syntax node dispatched by the driver.
 * @return Start and end byte of the node.
 */
static IndexedByteRange SpanOf(TSNode node)
{
    return IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

static bool ProcessStatementNode(TSSymbol sym, TSNode node, const TypeConversionCheckRequest& request,
                                 DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symExpressionStatement)
    {
        if (const auto* site = request.conversionSites->FindExpressionStatement(SpanOf(node)))
        {
            ProcessExpressionStatementSite(*site, request, ctx);
        }
        return true;
    }
    if (sym == gs.symForeachStatement)
    {
        if (const auto* site = request.conversionSites->FindForeach(SpanOf(node)))
        {
            ProcessForeachSite(*site, request, ctx);
        }
        return true;
    }
    if (sym == gs.symVariableDeclaration)
    {
        if (const auto* site = request.declarationSites->FindVariableDeclaration(SpanOf(node)))
        {
            ProcessVariableDeclarationSite(*site, request, ctx);
        }
        return true;
    }
    if (sym == gs.symReturnStatement)
    {
        if (const auto* site = request.returnSites->Find(SpanOf(node)))
        {
            ProcessReturnSite(*site, request, ctx);
        }
        return true;
    }
    if (sym == gs.symFuncDeclaration)
    {
        if (const auto* site = request.declarationSites->FindConstructorDelegation(SpanOf(node)))
        {
            CheckConstructorDelegation(*site, *request.declarationSites, ctx);
        }
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
        if (const auto* site = request.assignmentSites->Find(SpanOf(node)))
        {
            ProcessAssignmentSite(*site, request, ctx);
        }
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
        if (const auto* site = request.declarationSites->FindCallConstruct(SpanOf(node)))
        {
            ProcessCallConstructSite(*site, request, ctx);
        }
    }
}

static void ProcessNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symIfStatement || sym == gs.symWhileStatement || sym == gs.symDoWhileStatement ||
        sym == gs.symForStatement || sym == gs.symTernaryExpression)
    {
        if (const auto* site = request.conversionSites->FindCondition(SpanOf(node)))
        {
            ProcessConditionSite(*site, request, ctx);
        }
        if (sym == gs.symTernaryExpression)
        {
            if (const auto* ternary = request.conversionSites->FindTernary(SpanOf(node)))
            {
                ProcessTernarySite(*ternary, request, ctx);
            }
        }
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
    if (request.nodeIndex.Empty() || request.sourceCode.empty())
    {
        return;
    }

    std::optional<BinaryExpressionFacts> localFacts;
    if (!request.binaryFacts)
    {
        localFacts.emplace(BuildBinaryExpressionFacts(request.nodeIndex, request.sourceCode));
    }
    std::optional<ConversionSiteFacts> localSites;
    if (!request.conversionSites)
    {
        localSites.emplace(BuildConversionSiteFacts(request.nodeIndex, request.sourceCode));
    }
    std::optional<ReturnSiteFacts> localReturns;
    if (!request.returnSites)
    {
        localReturns.emplace(BuildReturnSiteFacts(request.nodeIndex, request.sourceCode));
    }
    std::optional<AssignmentSiteFacts> localAssignments;
    if (!request.assignmentSites)
    {
        localAssignments.emplace(BuildAssignmentSiteFacts(request.nodeIndex, request.sourceCode));
    }
    std::optional<DeclarationSiteFacts> localDeclarations;
    if (!request.declarationSites)
    {
        localDeclarations.emplace(BuildDeclarationSiteFacts(request.nodeIndex, request.sourceCode));
    }
    std::optional<ExpressionSiteFacts> localExpressions;
    if (!request.expressionSites)
    {
        localExpressions.emplace(BuildExpressionSiteFacts(request.nodeIndex, request.sourceCode));
    }
    TypeConversionCheckRequest withFacts = request;
    withFacts.binaryFacts = request.binaryFacts ? request.binaryFacts : &*localFacts;
    withFacts.conversionSites = request.conversionSites ? request.conversionSites : &*localSites;
    withFacts.returnSites = request.returnSites ? request.returnSites : &*localReturns;
    withFacts.assignmentSites = request.assignmentSites ? request.assignmentSites : &*localAssignments;
    withFacts.declarationSites = request.declarationSites ? request.declarationSites : &*localDeclarations;
    withFacts.expressionSites = request.expressionSites ? request.expressionSites : &*localExpressions;

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
