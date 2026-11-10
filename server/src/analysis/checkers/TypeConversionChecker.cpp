#include "analysis/checkers/TypeConversionChecker.h"

#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ConversionSiteFacts.h"
#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ReturnSiteFacts.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/LspLogger.h"

#include <array>
#include <cstdint>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string_view>

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
        ctx.EmitAtRange(site.range, "as-err-abstract-instantiated", {calleeName, calleeName});
    }
    else if (calleeNonInst == NonInstantiableKind::Mixin)
    {
        ctx.EmitAtRange(site.range, "as-err-mixin-not-a-type", calleeName);
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

    std::array<TypeConversionSiteCursor, 14> cursors = {{
        {[](size_t i, const auto& req) { return req.conversionSites->conditions[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessConditionSite(req.conversionSites->conditions[i], req, dctx); },
         withFacts.conversionSites->conditions.size(), 0},
        {[](size_t i, const auto& req) { return req.conversionSites->ternaries[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessTernarySite(req.conversionSites->ternaries[i], req, dctx); },
         withFacts.conversionSites->ternaries.size(), 0},
        {[](size_t i, const auto& req) { return req.conversionSites->expressionStatements[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessExpressionStatementSite(req.conversionSites->expressionStatements[i], req, dctx); },
         withFacts.conversionSites->expressionStatements.size(), 0},
        {[](size_t i, const auto& req) { return req.conversionSites->foreachLoops[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessForeachSite(req.conversionSites->foreachLoops[i], req, dctx); },
         withFacts.conversionSites->foreachLoops.size(), 0},
        {[](size_t i, const auto& req) { return req.declarationSites->variableDeclarations[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessVariableDeclarationSite(req.declarationSites->variableDeclarations[i], req, dctx); },
         withFacts.declarationSites->variableDeclarations.size(), 0},
        {[](size_t i, const auto& req) { return req.returnSites->returns[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx) { ProcessReturnSite(req.returnSites->returns[i], req, dctx); },
         withFacts.returnSites->returns.size(), 0},
        {[](size_t i, const auto& req) { return req.declarationSites->constructorDelegations[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { CheckConstructorDelegation(req.declarationSites->constructorDelegations[i], *req.declarationSites, dctx); },
         withFacts.declarationSites->constructorDelegations.size(), 0},
        {[](size_t i, const auto& req) { return req.assignmentSites->assignments[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessAssignmentSite(req.assignmentSites->assignments[i], req, dctx); },
         withFacts.assignmentSites->assignments.size(), 0},
        {[](size_t i, const auto& req) { return req.declarationSites->callConstructs[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessCallConstructSite(req.declarationSites->callConstructs[i], req, dctx); },
         withFacts.declarationSites->callConstructs.size(), 0},
        {[](size_t i, const auto& req) { return req.expressionSites->binaryExpressions[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessBinarySite(req.expressionSites->binaryExpressions[i], req, dctx); },
         withFacts.expressionSites->binaryExpressions.size(), 0},
        {[](size_t i, const auto& req) { return req.expressionSites->unaryPostfixExpressions[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessUnaryOrPostfixSite(req.expressionSites->unaryPostfixExpressions[i], req, dctx); },
         withFacts.expressionSites->unaryPostfixExpressions.size(), 0},
        {[](size_t i, const auto& req) { return req.expressionSites->memberExpressions[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessMemberExpressionSite(req.expressionSites->memberExpressions[i], req, dctx); },
         withFacts.expressionSites->memberExpressions.size(), 0},
        {[](size_t i, const auto& req) { return req.expressionSites->indexExpressions[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessIndexExpressionSite(req.expressionSites->indexExpressions[i], req, dctx); },
         withFacts.expressionSites->indexExpressions.size(), 0},
        {[](size_t i, const auto& req) { return req.expressionSites->castExpressions[i].bytes.startByte; },
         [](size_t i, const auto& req, auto& dctx)
         { ProcessCastSite(req.expressionSites->castExpressions[i], req, dctx); },
         withFacts.expressionSites->castExpressions.size(), 0},
    }};

    while (true)
    {
        size_t best = 0;
        uint32_t minByte = UINT32_MAX;
        for (size_t c = 0; c < cursors.size(); ++c)
        {
            const uint32_t b = cursors[c].currentByte(withFacts);
            if (b < minByte)
            {
                minByte = b;
                best = c;
            }
        }
        if (minByte == UINT32_MAX)
        {
            break;
        }
        cursors[best].dispatch(cursors[best].index, withFacts, ctx);
        cursors[best].index++;
    }

    if (ctx.logger && ctx.logger->IsTraceEnabled())
    {
        ctx.logger->LogTrace(
            fmt::format("[TypeConversionChecker] Finished CheckTypeConversions for URI: {}", ctx.request.fileUri));
    }
}

} // namespace angel_lsp::analysis
