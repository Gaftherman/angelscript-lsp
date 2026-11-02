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

static void CheckModuleContext(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (request.moduleContext.has_value() && !request.moduleContext->name.empty() &&
        !request.moduleContext->alsoClaimedBy.empty())
    {
        std::string others;
        for (const auto& name : request.moduleContext->alsoClaimedBy)
        {
            if (!others.empty())
            {
                others += ", ";
            }
            others += "'" + name + "'";
        }
        ctx.EmitAtRange({0, 0, 0, 0}, "as-hint-file-in-several-modules", {request.moduleContext->name, others},
                        DiagnosticSeverity::Hint);
    }
}

static void CheckUnsupportedDirective(const utils::UnsupportedDirective& directive,
                                      const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (directive.problem == utils::DirectiveProblem::Unrecognised)
    {
        ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                        "as-err-unknown-directive", directive.name, DiagnosticSeverity::Error);
        return;
    }
    if (directive.problem == utils::DirectiveProblem::IncludeNotQuoted)
    {
        ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                        "as-err-include-not-quoted", DiagnosticSeverity::Error);
        return;
    }
    if (directive.problem == utils::DirectiveProblem::SpaceAfterHash)
    {
        ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                        "as-err-directive-space-after-hash", {directive.name, directive.name},
                        DiagnosticSeverity::Error);
        return;
    }

    ctx.EmitAtRange({directive.line, directive.startColumn, directive.line, directive.endColumn},
                    "as-warn-unsupported-directive", directive.name,
                    directive.name == "pragma" ? request.pragmaSeverity : DiagnosticSeverity::Warning);
}

void SemanticAnalyzer::CheckDirectivesAndModules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx) const
{
    CheckModuleContext(request, ctx);
    for (const auto& directive : request.unsupportedDirectives)
    {
        CheckUnsupportedDirective(directive, request, ctx);
    }
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

static void CheckMetadataSupport(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                 DiagnosticContext& ctx)
{
    if (!request.enableMetadata && indexPtr)
    {
        const auto metaNodes = indexPtr->Nodes(parser::GetGrammarSymbols().symMetadata);
        for (TSNode metaNode : metaNodes)
        {
            const TSPoint start = ts_node_start_point(metaNode);
            const TSPoint end = ts_node_end_point(metaNode);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::MetadataDisabled,
                            DiagnosticSeverity::Warning);
        }
    }
}

static void FilterSuppressedDiagnostics(std::vector<Diagnostic>& diagnostics, const SemanticAnalysisRequest& request,
                                        const NodeIndex* indexPtr)
{
    if (!request.excludedLineRanges.empty())
    {
        std::erase_if(diagnostics, [&request](const Diagnostic& d)
                      { return utils::IsLineExcluded(request.excludedLineRanges, d.range.start.line); });
    }

    if (request.enableCommentSuppressions && !request.sourceCode.empty())
    {
        const auto suppressions = ParseDiagnosticSuppressions(request.sourceCode, indexPtr);
        if (!suppressions.Empty())
        {
            std::erase_if(diagnostics,
                          [&](const Diagnostic& d) { return suppressions.IsSuppressed(d.code, d.range.start.line); });
        }
    }

    if (request.tree && ts_node_has_error(ts_tree_root_node(request.tree)) && !request.sourceCode.empty())
    {
        const auto braceBalance = parser::ComputeBraceBalance(request.sourceCode);
        if (braceBalance.HasMissingOpenBrace() && braceBalance.firstUnmatchedCloseLine >= 0)
        {
            const int cutoffLine = braceBalance.firstUnmatchedCloseLine;
            std::erase_if(diagnostics, [cutoffLine](const Diagnostic& d)
                          { return static_cast<int>(d.range.start.line) > cutoffLine && d.code != "as-syntax-error"; });
        }
    }
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
        CheckDirectivesAndModules(request, ctx);
        CheckMetadataSupport(request, indexPtr, ctx);

        if (m_logger && m_logger->IsEnabled(utils::LogLevel::Info))
        {
            m_logger->LogInfo(
                fmt::format("[Checkers Breakdown] File: {} | ScopeRules: {:.2f} ms, StmtFlow: {:.2f} ms, "
                            "ExprRules: {:.2f} ms, TypeRules: {:.2f} ms, TypeConv: {:.2f} ms, Unused: {:.2f} ms",
                            request.fileUri, scopeMs, breakdown.stmtMs, breakdown.exprMs, breakdown.typeMs,
                            breakdown.typeConvMs, breakdown.unusedMs));
        }
    }

    FilterSuppressedDiagnostics(diagnostics, request, indexPtr);
    return diagnostics;
}

} // namespace angel_lsp::analysis
