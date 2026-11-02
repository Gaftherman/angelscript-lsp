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
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/DirectiveRules.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/rules/TypeRules.h"
#include "analysis/rules/UnusedSymbolChecker.h"
#include "analysis/rules/VariableRules.h"
#include "parser/ASTUtils.h"
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

static TSNode SafeGetRootNode(const SemanticAnalysisRequest& request)
{
    return request.tree ? ts_tree_root_node(request.tree) : TSNode{};
}

double SemanticAnalyzer::RunScopeRules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx) const
{
    if (!request.scopeRoot)
    {
        return 0.0;
    }
    const AstCheckContext ast{SafeGetRootNode(request), request.sourceCode, request.scopeRoot.get(), request.nodeIndex};
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
    const ControlFlowCheckRequest flowRequest{SafeGetRootNode(request), request.sourceCode};
    CheckControlFlow(flowRequest, ctx);
    if (indexPtr)
    {
        rules::ValidateStandaloneLambda(*indexPtr, ctx);
    }
}

void SemanticAnalyzer::RunExpressionRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                          DiagnosticContext& ctx) const
{
    const AstCheckContext ast{SafeGetRootNode(request), request.sourceCode, request.scopeRoot.get(), indexPtr};
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
    const AstCheckContext ast{SafeGetRootNode(request), request.sourceCode, request.scopeRoot.get(), indexPtr};
    RunCheckers<IsolationChecker, InitializerListChecker>(ast, ctx);
    CheckNamespacesAndScopes(NamespaceCheckRequest{SafeGetRootNode(request), request.sourceCode, indexPtr}, ctx);
}

SemanticAnalyzer::TimedAstBreakdown SemanticAnalyzer::RunTimedAstRules(const SemanticAnalysisRequest& request,
                                                                       const NodeIndex* indexPtr,
                                                                       DiagnosticContext& ctx) const
{
    TimedAstBreakdown breakdown;
    if (request.tree && !request.sourceCode.empty())
    {
        utils::HighResTimer stmtTimer;
        RunStatementAndControlFlowRules(request, indexPtr, ctx);
        breakdown.stmtMs = stmtTimer.ElapsedMs();

        if (request.enableTypeConversionChecks)
        {
            utils::HighResTimer typeConvTimer;
            const TypeConversionCheckRequest conversionRequest{SafeGetRootNode(request), request.sourceCode,
                                                               request.scopeRoot.get(), request.mutableScopeRoot,
                                                               indexPtr};
            CheckTypeConversions(conversionRequest, ctx);
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

std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest& request) const
{
    std::vector<Diagnostic> diagnostics;
    LogSymbolDump(request);

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
    return diagnostics;
}

} // namespace angel_lsp::analysis
