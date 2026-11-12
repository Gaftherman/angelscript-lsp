#include "analysis/SemanticAnalyzer.h"

#include "analysis/checkers/AccessChecker.h"
#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/CallChecker.h"
#include "analysis/checkers/ConstChecker.h"
#include "analysis/checkers/ControlFlowChecker.h"
#include "analysis/checkers/DefiniteAssignmentChecker.h"
#include "analysis/checkers/InitializerListChecker.h"
#include "analysis/checkers/IsolationChecker.h"
#include "analysis/checkers/LValueChecker.h"
#include "analysis/checkers/NamespaceChecker.h"
#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/checkers/UndefinedIdentifierChecker.h"
#include "analysis/checkers/null_safety/NullSafetyChecker.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/mediator/SemanticMediator.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/DirectiveRules.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/rules/TypeRules.h"
#include "analysis/rules/UnusedSymbolChecker.h"
#include "analysis/rules/VariableRules.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "parser/GrammarNames.h"
#include "spdlog/fmt/fmt.h"
#include "utils/LspLogger.h"
#include "utils/Timer.h"

namespace angel_lsp::analysis
{
SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger* logger) : m_logger(logger)
{
}

void SemanticAnalyzer::LogSymbolDump(const SemanticAnalysisRequest& request) const
{
    if (m_logger && m_logger->IsDebugEnabled())
    {
        m_logger->LogDebug(fmt::format("=== [SYMBOL COLLECTOR OUTPUT] Document: {} ===", request.fileUri));
        request.symbolTable.ForEachSymbolInFile(
            request.fileUri,
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
            {
                for (const auto& sym : symbols)
                {
                    if (sym.fileUri == request.fileUri)
                    {
                        m_logger->LogDebug(
                            fmt::format("  -> Symbol: [{}] Name: \"{}\" | Container: \"{}\" | Range: L{}:C{}-L{}:C{}",
                                        SymbolTypeToString(sym.type), sym.name, sym.containerName, sym.startLine + 1,
                                        sym.startCharacter + 1, sym.endLine + 1, sym.endCharacter + 1));
                    }
                }
            });
    }
}

double SemanticAnalyzer::RunScopeRules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx) const
{
    if (!request.scopeRoot)
    {
        return 0.0;
    }
    const SemanticMediator mediator(request.symbolTable, request.scopeRoot.get(), request.nodeIndex, request.sourceCode,
                                    request.fileUri, request.exprCache);
    const AstCheckContext ast{request.sourceCode, request.scopeRoot.get(), request.nodeIndex, &mediator};
    RunChecker<UndefinedIdentifierChecker>(ast, ctx);

    utils::HighResTimer unusedTimer;
    rules::CheckUnusedSymbols(request, ctx);
    const double unusedMs = unusedTimer.ElapsedMs();

    rules::CheckNullAssignedToNonHandleInScope(request.scopeRoot.get(), ctx);
    rules::CheckLocalVariableDeclarations(request.scopeRoot.get(), ctx);
    return unusedMs;
}

void SemanticAnalyzer::RunStatementAndControlFlowRules(const SemanticAnalysisRequest& request,
                                                       const NodeIndex* indexPtr, DiagnosticContext& ctx) const
{
    const ControlFlowCheckRequest flowRequest{request.sourceCode, indexPtr ? indexPtr : request.nodeIndex};
    CheckControlFlow(flowRequest, ctx);
    if (indexPtr)
    {
        rules::ValidateStandaloneLambda(*indexPtr, ctx);
    }
}

void SemanticAnalyzer::RunExpressionRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                          DiagnosticContext& ctx) const
{
    const SemanticMediator mediator(request.symbolTable, request.scopeRoot.get(), indexPtr, request.sourceCode,
                                    request.fileUri, request.exprCache);
    const AstCheckContext ast{request.sourceCode, request.scopeRoot.get(), indexPtr, &mediator};
    RunCheckers<AccessChecker, ConstChecker, LValueChecker, CallChecker, DefiniteAssignmentChecker, NullSafetyChecker>(
        ast, ctx);
    if (indexPtr)
    {
        rules::CheckEngineDialectRules(*indexPtr, ctx);
    }
}

void SemanticAnalyzer::RunTypeAndStructureRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                                DiagnosticContext& ctx) const
{
    const SemanticMediator mediator(request.symbolTable, request.scopeRoot.get(), indexPtr, request.sourceCode,
                                    request.fileUri, request.exprCache);
    const AstCheckContext ast{request.sourceCode, request.scopeRoot.get(), indexPtr, &mediator};
    RunCheckers<IsolationChecker, InitializerListChecker>(ast, ctx);
    CheckNamespacesAndScopes(NamespaceCheckRequest{request.sourceCode, indexPtr ? indexPtr : request.nodeIndex}, ctx);
}

/// Supplies extracted sites for semantic type conversion checks.
static void RunTypeConversions(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                               DiagnosticContext& ctx)
{
    const NodeIndex* index = indexPtr ? indexPtr : request.nodeIndex;
    if (index)
    {
        CheckTypeConversions({*index, request.sourceCode, request.scopeRoot.get(), request.mutableScopeRoot}, ctx);
    }
}

SemanticAnalyzer::TimedAstBreakdown SemanticAnalyzer::RunTimedAstRules(const SemanticAnalysisRequest& request,
                                                                       const NodeIndex* indexPtr,
                                                                       DiagnosticContext& ctx) const
{
    TimedAstBreakdown breakdown;
    const NodeIndex* index = indexPtr ? indexPtr : request.nodeIndex;
    if (index && !request.sourceCode.empty())
    {
        utils::HighResTimer stmtTimer;
        RunStatementAndControlFlowRules(request, index, ctx);
        breakdown.stmtMs = stmtTimer.ElapsedMs();

        if (request.enableTypeConversionChecks)
        {
            utils::HighResTimer typeConvTimer;
            RunTypeConversions(request, indexPtr, ctx);
            breakdown.typeConvMs = typeConvTimer.ElapsedMs();
        }

        utils::HighResTimer typeTimer;
        RunTypeAndStructureRules(request, indexPtr, ctx);
        breakdown.typeMs = typeTimer.ElapsedMs();

        utils::HighResTimer exprTimer;
        RunExpressionRules(request, indexPtr, ctx);
        breakdown.exprMs = exprTimer.ElapsedMs();
    }
    return breakdown;
}

static void DispatchDeclarationRule(const Symbol& sym, DiagnosticContext& ctx,
                                    rules::MixinFileCache* mixinCache = nullptr)
{
    switch (sym.type)
    {
    case SymbolType::Class:
        rules::ValidateClass(sym, ctx, mixinCache);
        break;
    case SymbolType::Interface:
        rules::ValidateClass(sym, ctx, mixinCache);
        rules::ValidateInterfaceMembers(sym, ctx);
        break;
    case SymbolType::Typedef:
        rules::ValidateTypedef(sym, ctx);
        break;
    case SymbolType::Function:
        rules::ValidateFunction(sym, ctx);
        rules::ValidateOperator(sym, ctx);
        break;
    case SymbolType::Funcdef:
        rules::ValidateFuncdef(sym, ctx);
        rules::ValidateParameters(sym, sym.GetFuncdef().parameters, true, ctx);
        break;
    case SymbolType::Enum:
        rules::ValidateEnum(sym, ctx);
        break;
    case SymbolType::Variable:
    case SymbolType::Property:
        rules::ValidateVariable(sym, ctx);
        break;
    default:
        break;
    }
}

void SemanticAnalyzer::CheckDeclarationRules(const SymbolTable& symbolTable, DiagnosticContext& ctx) const
{
    rules::MixinFileCache localMixinCache;
    rules::MixinFileCache* effectiveMixinCache = ctx.request.mixinCache ? ctx.request.mixinCache : &localMixinCache;
    symbolTable.ForEachSymbolInFile(
        ctx.request.fileUri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            rules::ValidateDuplicates(symbols, ctx);
            for (const auto& sym : symbols)
            {
                if (sym.fileUri == ctx.request.fileUri)
                {
                    DispatchDeclarationRule(sym, ctx, effectiveMixinCache);
                }
            }
        });
}

static void StoreCallBindings(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (!request.mutableScopeRoot)
    {
        return;
    }
    auto& enclosing = request.mutableScopeRoot->enclosingContext;
    if (!ctx.callBindings && (!enclosing || !enclosing->callBindings))
    {
        return;
    }
    auto updated =
        enclosing ? std::make_shared<ScopeEnclosingContext>(*enclosing) : std::make_shared<ScopeEnclosingContext>();
    updated->callBindings = std::move(ctx.callBindings);
    enclosing = std::move(updated);
}

std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest& request) const
{
    std::vector<Diagnostic> diagnostics;
    LogSymbolDump(request);

    ExpressionTypeCache localCache;
    const bool ownsCache = (request.exprCache == nullptr);
    if (ownsCache)
    {
        request.exprCache = &localCache;
    }

    AnalysisContextGuards guards(request);
    const NodeIndex* indexPtr = request.nodeIndex;

    {
        DiagnosticContext ctx{request, diagnostics, m_logger};
        rules::CheckNullAssignedToNonHandle(request.symbolTable, ctx);
        CheckDeclarationRules(request.symbolTable, ctx);

        utils::HighResTimer scopeTimer;
        const double unusedMs = RunScopeRules(request, ctx);
        const double scopeMs = scopeTimer.ElapsedMs();

        auto breakdown = RunTimedAstRules(request, indexPtr, ctx);
        breakdown.unusedMs = unusedMs;
        rules::CheckDirectivesAndModules(request, ctx);
        rules::CheckMetadataSupport(request, indexPtr, ctx);
        StoreCallBindings(request, ctx);

        if (m_logger && m_logger->IsEnabled(utils::LogLevel::Info))
        {
            m_logger->LogInfo(
                fmt::format("[Checkers Breakdown] File: {} | ScopeRules: {:.2f} ms, StmtFlow: {:.2f} ms, "
                            "ExprRules: {:.2f} ms, TypeRules: {:.2f} ms, TypeConv: {:.2f} ms, Unused: {:.2f} ms",
                            request.fileUri, scopeMs, breakdown.stmtMs, breakdown.exprMs, breakdown.typeMs,
                            breakdown.typeConvMs, breakdown.unusedMs));
        }
    }

    rules::FilterSuppressedDiagnostics(diagnostics, request, indexPtr);
    if (ownsCache)
    {
        request.exprCache = nullptr;
    }
    return diagnostics;
}

} // namespace angel_lsp::analysis
