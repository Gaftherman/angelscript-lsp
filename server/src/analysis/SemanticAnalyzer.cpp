#include "analysis/SemanticAnalyzer.h"
#include "analysis/AccessChecker.h"
#include "analysis/CallChecker.h"
#include "analysis/ConstChecker.h"
#include "analysis/ControlFlowChecker.h"
#include "analysis/DefiniteAssignmentChecker.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/DiagnosticSuppression.h"
#include "analysis/InitializerListChecker.h"
#include "analysis/IsolationChecker.h"
#include "analysis/LValueChecker.h"
#include "analysis/NamespaceChecker.h"
#include "analysis/NodeIndex.h"
#include "analysis/NullSafetyChecker.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeConversionChecker.h"
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
#include "utils/ScopedNameUtils.h"
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
    CheckUndefinedIdentifiers(request.scopeRoot.get(), request.GetRuleIndex().globalNames, ctx);

    utils::HighResTimer unusedTimer;
    rules::CheckUnusedSymbols(request, ctx);
    const double unusedMs = unusedTimer.ElapsedMs();

    CheckNullAssignedToNonHandleInScope(request.scopeRoot.get(), ctx);
    CheckLocalVariableDeclarations(request.scopeRoot.get(), ctx);
    return unusedMs;
}

void SemanticAnalyzer::RunStatementAndControlFlowRules(const SemanticAnalysisRequest& request,
                                                       const NodeIndex* indexPtr, DiagnosticContext& ctx) const
{
    const ControlFlowCheckRequest flowRequest{ts_tree_root_node(request.tree), request.sourceCode};
    CheckControlFlow(flowRequest, ctx);
    if (indexPtr)
    {
        rules::ValidateStandaloneLambda(*indexPtr, ctx);
    }
}

void SemanticAnalyzer::RunExpressionRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                          DiagnosticContext& ctx) const
{
    const AccessCheckRequest accessRequest{ts_tree_root_node(request.tree), request.sourceCode, request.scopeRoot.get(),
                                           indexPtr};
    CheckMemberAccess(accessRequest, ctx);

    const ConstCheckRequest constRequest{ts_tree_root_node(request.tree), request.sourceCode, request.scopeRoot.get(),
                                         indexPtr};
    CheckConstCorrectness(constRequest, ctx);

    const LValueCheckRequest lvalueRequest{ts_tree_root_node(request.tree), request.sourceCode, request.scopeRoot.get(),
                                           indexPtr};
    CheckLValues(lvalueRequest, ctx);

    const CallCheckRequest callRequest{ts_tree_root_node(request.tree), request.sourceCode, request.scopeRoot.get(),
                                       indexPtr};
    CheckCallArguments(callRequest, ctx);

    const DefiniteAssignmentCheckRequest assignRequest{ts_tree_root_node(request.tree), request.sourceCode,
                                                       request.scopeRoot.get(), indexPtr};
    CheckDefiniteAssignment(assignRequest, ctx);

    if (ctx.request.diagnostics && ctx.request.diagnostics->reportPossibleNullDereference)
    {
        const NullSafetyCheckRequest nullSafetyRequest{ts_tree_root_node(request.tree), request.sourceCode,
                                                       request.scopeRoot.get(), indexPtr};
        CheckNullSafety(nullSafetyRequest, ctx);
    }
    if (indexPtr)
    {
        CheckEngineDialectRules(*indexPtr, ctx);
    }
}

void SemanticAnalyzer::RunTypeAndStructureRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                                DiagnosticContext& ctx) const
{

    const IsolationCheckRequest isolationRequest{ts_tree_root_node(request.tree), request.sourceCode,
                                                 request.scopeRoot.get(), indexPtr};
    CheckSharedIsolation(isolationRequest, ctx);

    CheckNamespacesAndScopes(NamespaceCheckRequest{ts_tree_root_node(request.tree), request.sourceCode, indexPtr}, ctx);

    CheckInitializerLists(InitializerListCheckRequest{ts_tree_root_node(request.tree), request.sourceCode,
                                                      request.scopeRoot.get(), indexPtr},
                          ctx);
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
            const TypeConversionCheckRequest conversionRequest{ts_tree_root_node(request.tree), request.sourceCode,
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
        const auto braceBalance = ComputeBraceBalance(request.sourceCode);
        if (braceBalance.HasMissingOpenBrace() && braceBalance.firstUnmatchedCloseLine >= 0)
        {
            const int cutoffLine = braceBalance.firstUnmatchedCloseLine;
            std::erase_if(diagnostics, [cutoffLine](const Diagnostic& d)
                          { return static_cast<int>(d.range.start.line) > cutoffLine && d.code != "as-syntax-error"; });
        }
    }
}

struct AnalysisContextGuards
{
    const SemanticAnalysisRequest& req;
    ExpressionTypeCache* prevCache;
    const NodeIndex* prevIndex;
    ExpressionTypeCache localCache;
    std::unique_ptr<NodeIndex> localNodeIndex;

    explicit AnalysisContextGuards(const SemanticAnalysisRequest& request)
        : req(request), prevCache(request.exprCache), prevIndex(request.nodeIndex)
    {
        if (!req.exprCache)
        {
            const size_t typeCapacity = req.sourceCode.size() > 10000 ? (req.sourceCode.size() / 32) : 512;
            localCache.types.reserve(typeCapacity);
            localCache.hierarchies.reserve(256);
            req.exprCache = &localCache;
        }
        if (!req.nodeIndex && req.tree)
        {
            localNodeIndex = std::make_unique<NodeIndex>(ts_tree_root_node(req.tree), nullptr, req.traversalBudget);
            req.nodeIndex = localNodeIndex.get();
        }
    }

    ~AnalysisContextGuards()
    {
        req.exprCache = prevCache;
        req.nodeIndex = prevIndex;
    }
};

std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest& request) const
{
    std::vector<Diagnostic> diagnostics;
    LogSymbolDump(request);

    AnalysisContextGuards guards(request);
    const NodeIndex* indexPtr = request.nodeIndex;

    {
        DiagnosticContext ctx{request, diagnostics, m_logger};
        CheckNullAssignedToNonHandle(request.symbolTable, ctx);
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

static void CheckDialectForeach(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (!ctx.request.SupportsForeach())
    {
        for (TSNode node : nodeIndex.Nodes(parser::GetGrammarSymbols().symForeachStatement))
        {
            const TSPoint start = ts_node_start_point(node);
            ctx.EmitAtRange({start.row, start.column, start.row, start.column + 7}, "as-err-foreach-unsupported",
                            DiagnosticSeverity::Error);
        }
    }
}

static void CheckDialectEmptyListElements(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (ctx.request.DisallowsEmptyListElements())
    {
        const auto& gs = parser::GetGrammarSymbols();
        for (TSNode node : nodeIndex.Nodes(gs.symInitializerList))
        {
            TSTreeCursor cursor = ts_tree_cursor_new(node);
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                TSSymbol prevSym = 0;
                TSPoint prevStart{};
                do
                {
                    TSNode ch = ts_tree_cursor_current_node(&cursor);
                    TSSymbol currSym = ts_node_symbol(ch);
                    if (prevSym == gs.tokComma && (currSym == gs.tokComma || currSym == gs.tokCloseBrace))
                    {
                        ctx.EmitAtRange({prevStart.row, prevStart.column, prevStart.row, prevStart.column + 1},
                                        "as-err-empty-list-element", DiagnosticSeverity::Error);
                    }
                    prevSym = currSym;
                    prevStart = ts_node_start_point(ch);
                } while (ts_tree_cursor_goto_next_sibling(&cursor));
            }
            ts_tree_cursor_delete(&cursor);
        }
    }
}

static void CheckCharLiteralDeclarator(TSNode declarator, const std::string& declared, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(declarator) != gs.symVariableDeclarator)
    {
        return;
    }

    const TSNode value = parser::GetChildByField(declarator, gs.fieldValue);
    if (ts_node_is_null(value) || ts_node_symbol(value) != gs.symStringLiteral)
    {
        return;
    }

    const uint32_t from = ts_node_start_byte(value);
    if (from < ctx.request.sourceCode.size() && ctx.request.sourceCode[from] == '\'')
    {
        const TSPoint start = ts_node_start_point(value);
        const TSPoint end = ts_node_end_point(value);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-character-literal-is-string", declared,
                        DiagnosticSeverity::Error);
    }
}

static void CheckDialectVariableDeclarationNode(TSNode node, DiagnosticContext& ctx)
{
    if (ctx.request.CharacterLiteralMode() != 0)
    {
        return;
    }

    const TSNode typeNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        return;
    }

    const std::string declared = CleanBaseType(GetNodeText(typeNode, ctx.request.sourceCode));
    if (!IsPrimitiveTypeName(declared) || declared == "auto" || declared == "void")
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            CheckCharLiteralDeclarator(ts_tree_cursor_current_node(&cursor), declared, ctx);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

static void CheckDialectCharacterLiterals(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (ctx.request.CharacterLiteralMode() != 0)
    {
        return;
    }

    for (TSNode node : nodeIndex.Nodes(parser::GetGrammarSymbols().symVariableDeclaration))
    {
        CheckDialectVariableDeclarationNode(node, ctx);
    }
}

static bool IsIntegerLiteral(TSNode candidate, std::string_view sourceCode)
{
    if (ts_node_is_null(candidate) || ts_node_symbol(candidate) != parser::GetGrammarSymbols().symNumberLiteral)
    {
        return false;
    }
    const std::string text = GetNodeText(candidate, sourceCode);
    return text.find_first_of(".eEfF") == std::string::npos;
}

static void CheckDialectIntegerDivisionNode(TSNode node, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode op = parser::GetChildByField(node, gs.fieldOperator);
    if (ts_node_is_null(op) || ts_node_symbol(op) != gs.tokSlash)
    {
        return;
    }

    const TSNode left = parser::GetChildByField(node, gs.fieldLeft);
    const TSNode right = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldRight);

    if (IsIntegerLiteral(left, ctx.request.sourceCode) && IsIntegerLiteral(right, ctx.request.sourceCode))
    {
        const TSPoint start = ts_node_start_point(node);
        const TSPoint end = ts_node_end_point(node);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-hint-integer-division",
                        DiagnosticSeverity::Hint);
    }
}

static void CheckDialectIntegerDivision(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (ctx.request.diagnostics && ctx.request.diagnostics->reportIntegerDivision &&
        !ctx.request.DisablesIntegerDivision())
    {
        for (TSNode node : nodeIndex.Nodes(parser::GetGrammarSymbols().symBinaryExpression))
        {
            CheckDialectIntegerDivisionNode(node, ctx);
        }
    }
}

static void CheckDialectNamedArguments(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (ctx.request.NamedArgumentSyntaxMode() != 2)
    {
        for (TSNode node : nodeIndex.Nodes(parser::GetGrammarSymbols().symArgumentList))
        {
            const uint32_t argCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < argCount; ++i)
            {
                const auto& gs = parser::GetGrammarSymbols();
                const TSNode argument = ts_node_named_child(node, i);
                if (ts_node_symbol(argument) != gs.symAssignmentExpression)
                {
                    continue;
                }

                const TSNode target = parser::GetChildByField(argument, gs.fieldLeft);
                if (ts_node_is_null(target))
                {
                    continue;
                }
                const TSSymbol targetSym = ts_node_symbol(target);
                if (targetSym != gs.symIdentifier && targetSym != gs.symScopedIdentifier)
                {
                    continue;
                }
                if (targetSym == gs.symScopedIdentifier && ts_node_named_child_count(target) != 1)
                {
                    continue;
                }

                const TSPoint start = ts_node_start_point(argument);
                const TSPoint end = ts_node_end_point(argument);
                const std::string name = GetNodeText(target, ctx.request.sourceCode);

                ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-named-argument-syntax", name,
                                ctx.request.NamedArgumentSyntaxMode() == 1 ? DiagnosticSeverity::Warning
                                                                           : DiagnosticSeverity::Error);
            }
        }
    }
}

static bool IsSymbolTableClass(const SymbolTable& table, const std::string& typeName)
{
    if (const auto symbols = table.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Class)
            {
                return true;
            }
        }
    }
    return false;
}

static void CheckDialectAssignmentExpressionNode(TSNode node, DiagnosticContext& ctx)
{
    const TSNode op = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldOperator);
    const TSNode target = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldLeft);

    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(op) || ts_node_symbol(op) != gs.tokAssign || ts_node_is_null(target))
    {
        return;
    }

    if (IsHandleAssignmentTarget(target))
    {
        return;
    }

    const Scope* scope = ctx.request.scopeRoot
                             ? FindInnermostScope(ctx.request.scopeRoot.get(), ts_node_start_point(target).row,
                                                  ts_node_start_point(target).column)
                             : nullptr;
    ExpressionTypeContext exprCtx(scope, ctx);
    const std::string targetType = CleanBaseType(ResolveExpressionType(target, exprCtx));

    if (IsSymbolTableClass(ctx.request.symbolTable, targetType))
    {
        const TSPoint start = ts_node_start_point(node);
        const TSPoint end = ts_node_end_point(node);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-value-assign-for-ref", targetType,
                        DiagnosticSeverity::Error);
    }
}

static void CheckDialectValueAssignForRef(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (ctx.request.DisallowsValueAssignForRef())
    {
        for (TSNode node : nodeIndex.Nodes(parser::GetGrammarSymbols().symAssignmentExpression))
        {
            CheckDialectAssignmentExpressionNode(node, ctx);
        }
    }
}

static void CheckDialectMultilineStrings(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (!ctx.request.AllowsMultilineStrings())
    {
        for (TSNode node : nodeIndex.Nodes(parser::GetGrammarSymbols().symStringLiteral))
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);

            if (end.row > start.row)
            {
                const uint32_t from = ts_node_start_byte(node);
                const bool isHeredoc =
                    from + 3 <= ctx.request.sourceCode.size() && ctx.request.sourceCode.compare(from, 3, "\"\"\"") == 0;

                if (!isHeredoc)
                {
                    ctx.EmitAtRange({start.row, start.column, start.row, start.column + 1}, "as-err-multiline-string",
                                    DiagnosticSeverity::Error);
                }
            }
        }
    }
}

void SemanticAnalyzer::CheckEngineDialectRules(const NodeIndex& nodeIndex, DiagnosticContext& ctx) const
{
    CheckDialectForeach(nodeIndex, ctx);
    CheckDialectEmptyListElements(nodeIndex, ctx);
    CheckDialectCharacterLiterals(nodeIndex, ctx);
    CheckDialectIntegerDivision(nodeIndex, ctx);
    CheckDialectNamedArguments(nodeIndex, ctx);
    CheckDialectValueAssignForRef(nodeIndex, ctx);
    CheckDialectMultilineStrings(nodeIndex, ctx);
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

namespace
{
using MixinRanges = std::vector<std::pair<uint32_t, uint32_t>>;

MixinRanges CollectMixinRanges(const SymbolTable& symbolTable, const std::string& fileUri)
{
    MixinRanges mixinRanges;
    symbolTable.ForEachSymbolInFile(
        fileUri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
                {
                    mixinRanges.push_back({sym.startLine, sym.endLine});
                }
            }
        });
    return mixinRanges;
}

bool ShouldIgnoreReference(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (ref.isMemberAccess || ref.isTypeSpecifier || ref.isNamedArgument)
        return true;

    if (ref.name == "this" || ref.name == "value")
        return true;

    // `super` names no symbol, by design. In `class D : B { D() { super(1); } }` it is the
    // base-constructor call and the code compiles; anywhere else - `super.F()`,
    // `super::F()` - the compiler agrees with this rule and reports it. So the shape is
    // tested, not the name. NamespaceChecker makes the same distinction for the error it
    // raises on the same line.
    if (ref.name == "super")
    {
        if (ctx.request.tree)
        {
            const TSPoint at{ref.startLine, ref.startCharacter};
            const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
            if (IsBaseConstructorCall(node, ctx.request.sourceCode))
                return true;
        }
        else
        {
            // No tree to ask. Staying silent is the policy when the analyzer cannot see
            // enough to be sure.
            return true;
        }
    }

    if (ref.name == "function" && ctx.request.tree)
    {
        const TSPoint at{ref.startLine, ref.startCharacter};
        const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
        TSNode parent = parser::GetParent(node);
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == parser::GetGrammarSymbols().symLambdaExpression)
            return true;
    }

    return false;
}

bool IsReferenceInMixin(const LocalReference& ref, const MixinRanges& mixinRanges)
{
    for (const auto& r : mixinRanges)
    {
        if (ref.startLine >= r.first && ref.endLine <= r.second)
            return true;
    }
    return false;
}

/**
 * @brief Checks if an identifier reference is part of a qualified scoped identifier (e.g. Enum::Member).
 * @param[in] ref Lexical reference to inspect.
 * @param[in] ctx Diagnostic context holding AST tree and source code.
 * @return True when the reference is preceded by a scope resolution operator.
 */
bool IsEnumReferenceQualified(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (!ctx.request.tree)
    {
        return false;
    }
    const TSPoint at{ref.startLine, ref.startCharacter};
    const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
    if (ts_node_is_null(node))
    {
        return false;
    }
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    if (ts_node_symbol(parent) == parser::GetGrammarSymbols().symScopedIdentifier)
    {
        const uint32_t pStart = ts_node_start_byte(parent);
        const uint32_t pEnd = ts_node_end_byte(parent);
        if (pStart < ctx.request.sourceCode.size() && pEnd <= ctx.request.sourceCode.size() && pStart < pEnd)
        {
            const std::string_view text = ctx.request.sourceCode.substr(pStart, pEnd - pStart);
            if (utils::ContainsScopeSeparator(text))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Validates that enum member access obeys asEP_REQUIRE_ENUM_SCOPE when enabled.
 * @param[in] ref Lexical reference to an enumerator.
 * @param[in] resolved Resolved local definition, if any.
 * @param[in,out] ctx Diagnostic context to emit into.
 * @return True when a diagnostic was emitted or reference should be skipped.
 */
bool CheckEnumScopeDiagnostic(const LocalReference& ref, const LocalDefinition* resolved, DiagnosticContext& ctx)
{
    // asEP_REQUIRE_ENUM_SCOPE, checked before the resolution is acted on, because an
    // enumerator DOES resolve from the scope tree - LOCALS_QUERY captures `enum_member` as
    // a definition - and continuing on that is what made the first attempt at this rule
    // never run.
    if (resolved != nullptr && resolved->kind != LocalDefinitionKind::Constant)
    {
        return false;
    }
    if (!ctx.request.GetRuleIndex().enumMemberNames.contains(ref.name))
    {
        return false;
    }

    const bool isOwnDeclaration =
        resolved != nullptr && resolved->startLine == ref.startLine && resolved->startCharacter == ref.startCharacter;
    if (isOwnDeclaration || IsEnumReferenceQualified(ref, ctx))
    {
        return false;
    }

    if (ctx.request.RequiresEnumScope())
    {
        ctx.EmitAtRange({ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter},
                        "as-err-enum-scope-required", ref.name);
        return true;
    }
    return true;
}

bool IsContainerAccessorProperty(const TSNode node, std::string_view refName, const DiagnosticContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const auto& index = ctx.request.GetRuleIndex();
    const bool reqKeyword = ctx.request.RequiresAccessorKeyword();
    auto containers = GetEnclosingContainers(node, ctx.request.sourceCode);
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string nsName = container.qualifiedName.empty() ? container.name : container.qualifiedName;
            const auto& members = index.NamespaceMembers(nsName);
            const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
            if (accSet.contains(refName))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            const auto& members = index.ClassMembers(typeName);
            const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
            if (accSet.contains(refName))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Checks if an identifier name is declared in an enclosing container or using-namespace.
 * @param[in] node AST descendant node for the reference.
 * @param[in] refName Identifier name to check.
 * @param[in] usingNamespaces Active using-namespace list for the document.
 * @param[in] ctx Diagnostic context.
 * @return True if refName is a declared member in the enclosing scope hierarchy.
 */
bool IsEnclosingScopeMember(TSNode node, const std::string& refName, const std::vector<std::string>& usingNamespaces,
                            const DiagnosticContext& ctx)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const auto& index = ctx.request.GetRuleIndex();
    const auto containers = GetEnclosingContainers(node, ctx.request.sourceCode);
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string nsName = container.qualifiedName.empty() ? container.name : container.qualifiedName;
            if (index.NamespaceMembers(nsName).allMemberNames.contains(refName))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            if (index.ClassMembers(typeName).allMemberNames.contains(refName))
            {
                return true;
            }
        }
    }

    for (const auto& uNs : usingNamespaces)
    {
        if (index.NamespaceMembers(uNs).allMemberNames.contains(refName))
        {
            return true;
        }
    }

    return false;
}

bool IsAccessorPropertyOrKeyword(const LocalReference& ref, const DiagnosticContext& ctx)
{
    if (IsReservedKeyword(ref.name))
    {
        return true;
    }

    const auto& index = ctx.request.GetRuleIndex();
    const auto& globalAccessors = ctx.request.RequiresAccessorKeyword() ? index.keywordGlobalAccessorPropertyNames
                                                                        : index.globalAccessorPropertyNames;
    if (globalAccessors.contains(ref.name))
    {
        return true;
    }

    if (ctx.request.tree)
    {
        const TSPoint at{ref.startLine, ref.startCharacter};
        const TSNode node = ts_node_descendant_for_point_range(ts_tree_root_node(ctx.request.tree), at, at);
        if (IsContainerAccessorProperty(node, ref.name, ctx))
        {
            return true;
        }
    }

    if (!FindGlobalPropertyAccessors(ref.name, ctx.request.symbolTable, ctx.request.RequiresAccessorKeyword()).empty())
    {
        return true;
    }

    return false;
}

struct UndefinedIdentifierContext
{
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames;
    const MixinRanges& mixinRanges;
    const std::vector<std::string>& usingNamespaces;
    DiagnosticContext& ctx;
};

static bool IsReferenceInSyntaxErrorOrMetadata(TSNode root, const LocalReference& ref, const NodeIndex* nodeIndex)
{
    const TSPoint atStart{ref.startLine, ref.startCharacter};
    const TSPoint atEnd{ref.endLine, ref.endCharacter};
    const TSNode node = ts_node_descendant_for_point_range(root, atStart, atEnd);
    if (ts_node_is_null(node))
    {
        return false;
    }
    if (nodeIndex && nodeIndex->NodeHasSyntaxError(node))
    {
        return true;
    }
    bool inErrorOrMeta = false;
    parser::ForEachAncestor(node,
                            [&](TSNode walk) -> bool
                            {
                                if (ts_node_is_error(walk) ||
                                    ts_node_symbol(walk) == parser::GetGrammarSymbols().symMetadata)
                                {
                                    inErrorOrMeta = true;
                                    return false;
                                }
                                return true;
                            });
    return inErrorOrMeta;
}

void CheckScopeReferences(const Scope* scope, const UndefinedIdentifierContext& uCtx)
{
    for (const auto& ref : scope->references)
    {
        if (ShouldIgnoreReference(ref, uCtx.ctx) || IsReferenceInMixin(ref, uCtx.mixinRanges))
            continue;

        const LocalDefinition* resolved = ResolveInScopeAt(scope, ref.name, ref.startByte);
        if (CheckEnumScopeDiagnostic(ref, resolved, uCtx.ctx))
            continue;

        if (resolved != nullptr || ResolveInScopeAt(scope, ref.name, ref.startByte, nullptr, false) != nullptr ||
            uCtx.knownGlobalNames.contains(ref.name))
            continue;

        if (IsAccessorPropertyOrKeyword(ref, uCtx.ctx))
            continue;

        if (uCtx.ctx.request.tree)
        {
            const TSNode root = ts_tree_root_node(uCtx.ctx.request.tree);
            if (IsReferenceInSyntaxErrorOrMetadata(root, ref, uCtx.ctx.request.nodeIndex))
                continue;
            const TSPoint at{ref.startLine, ref.startCharacter};
            const TSNode node = ts_node_descendant_for_point_range(root, at, at);
            if (IsEnclosingScopeMember(node, ref.name, uCtx.usingNamespaces, uCtx.ctx))
                continue;
        }

        uCtx.ctx.EmitAtRange({ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter},
                             "as-warn-undeclared-identifier", ref.name, DiagnosticSeverity::Error);
    }
}

void CheckUndefinedIdentifiersIterative(const Scope* rootScope, const UndefinedIdentifierContext& uCtx)
{
    if (!rootScope)
    {
        return;
    }

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        CheckScopeReferences(scope, uCtx);

        for (const auto& child : scope->children)
        {
            if (child)
            {
                worklist.push_back(child.get());
            }
        }
    }
}
} // namespace

void SemanticAnalyzer::CheckUndefinedIdentifiers(
    const Scope* scope,
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames,
    DiagnosticContext& ctx) const
{
    if (!scope)
    {
        return;
    }

    const MixinRanges mixinRanges = CollectMixinRanges(ctx.request.symbolTable, ctx.request.fileUri);
    const std::vector<std::string> usingNamespaces =
        ctx.request.nodeIndex
            ? CollectUsingNamespaces(*ctx.request.nodeIndex, ctx.request.sourceCode)
            : (ctx.request.tree ? CollectUsingNamespaces(ts_tree_root_node(ctx.request.tree), ctx.request.sourceCode)
                                : std::vector<std::string>{});
    const UndefinedIdentifierContext uCtx{knownGlobalNames, mixinRanges, usingNamespaces, ctx};
    CheckUndefinedIdentifiersIterative(scope, uCtx);
}

void SemanticAnalyzer::CollectUsedDefinitions(const Scope* rootScope,
                                              ankerl::unordered_dense::set<const LocalDefinition*>& used) const
{
    if (!rootScope)
    {
        return;
    }

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        for (const auto& ref : scope->references)
        {
            if (ref.isMemberAccess || ref.isNamedArgument)
                continue;

            const LocalDefinition* def = ResolveInScope(scope, ref.name);
            if (!def)
                continue;

            bool isSelfOccurrence = ref.startLine == def->startLine && ref.startCharacter == def->startCharacter &&
                                    ref.endLine == def->endLine && ref.endCharacter == def->endCharacter;
            if (isSelfOccurrence)
                continue;

            used.insert(def);
        }

        for (const auto& child : scope->children)
        {
            if (child)
            {
                worklist.push_back(child.get());
            }
        }
    }
}

void SemanticAnalyzer::CheckUnusedVariables(const Scope* rootScope,
                                            const ankerl::unordered_dense::set<const LocalDefinition*>& used,
                                            DiagnosticContext& ctx) const
{
    if (!rootScope)
    {
        return;
    }

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        bool isFunctionNested = false;
        for (const Scope* ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto& def : scope->definitions)
            {
                if (def.kind != LocalDefinitionKind::Variable)
                    continue;

                if (used.contains(&def))
                    continue;

                ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                                "as-warn-unused-variable", def.name, DiagnosticSeverity::Warning);
            }
        }

        for (const auto& child : scope->children)
        {
            if (child)
            {
                worklist.push_back(child.get());
            }
        }
    }
}

namespace
{
/**
 * @brief True only for the VM-intrinsic numeric/bool kinds, where null is unconditionally
 *        invalid under any AngelScript engine configuration - unlike a class/object type
 *        (TypeKind::Object/Unknown/String/...), which could be app-registered with a
 *        converting constructor or opAssign(int) that legitimately accepts null (e.g. this
 *        corpus's SvenCoop "EHandle eView = null;" - confirmed by corpus audit and by
 *        reading the real declaration: EHandle(pView) is a registered converting
 *        constructor, invisible to this analyzer since no predefined stub exists for it).
 */
bool IsUnconditionallyNonNullablePrimitive(TypeKind kind)
{
    switch (kind)
    {
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::Float:
    case TypeKind::Double:
    case TypeKind::Bool:
        return true;
    default:
        return false;
    }
}
} // namespace

void SemanticAnalyzer::CheckNullAssignedToNonHandle(const SymbolTable& symbolTable, DiagnosticContext& ctx) const
{
    symbolTable.ForEachSymbolInFile(
        ctx.request.fileUri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.fileUri != ctx.request.fileUri)
                    continue;

                if (!std::holds_alternative<VariableSignature>(sym.signature))
                    continue;

                const VariableSignature& varSig = sym.GetVariable();
                if (varSig.isVirtualProperty || !varSig.hasNullInitializer || varSig.modifiers.isHandle)
                    continue;

                if (!IsUnconditionallyNonNullablePrimitive(varSig.typeKind))
                    continue;

                ctx.Emit(sym, "as-err-null-non-handle", varSig.typeName, DiagnosticSeverity::Error);
            }
        });
}

void SemanticAnalyzer::CheckNullAssignedToNonHandleInScope(const Scope* rootScope, DiagnosticContext& ctx) const
{
    if (!rootScope)
    {
        return;
    }

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        bool isFunctionNested = false;
        for (const Scope* ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto& def : scope->definitions)
            {
                if (def.kind != LocalDefinitionKind::Variable)
                    continue;

                if (!def.hasNullInitializer || def.isHandleType)
                    continue;

                if (!IsUnconditionallyNonNullablePrimitive(def.typeKind))
                    continue;

                ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                                "as-err-null-non-handle", def.typeName, DiagnosticSeverity::Error);
            }
        }

        for (const auto& child : scope->children)
        {
            if (child)
            {
                worklist.push_back(child.get());
            }
        }
    }
}

namespace
{
SourceRange GetDefinitionTypeRange(const LocalDefinition& def)
{
    const bool hasExplicitType = (def.typeEndCharacter > def.typeStartCharacter || def.typeEndLine > def.typeStartLine);
    return SourceRange{
        hasExplicitType ? def.typeStartLine : def.startLine,
        hasExplicitType ? def.typeStartCharacter : def.startCharacter,
        hasExplicitType ? def.typeEndLine : def.endLine,
        hasExplicitType ? def.typeEndCharacter : def.endCharacter,
    };
}

void ValidateTemplateArguments(const LocalDefinition& def, const TemplateTypeInfo& tmplInfo,
                               std::string_view enclosingNs, DiagnosticContext& ctx)
{
    const SourceRange range = GetDefinitionTypeRange(def);
    if (!IsKnownType(tmplInfo.containerName, ctx, enclosingNs))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", tmplInfo.containerName, DiagnosticSeverity::Error);
    }
    for (size_t i = 0; i < tmplInfo.templateArgs.size(); ++i)
    {
        std::string cleanArg = CleanBaseType(tmplInfo.templateArgs[i]);
        if (!IsKnownType(cleanArg, ctx, enclosingNs))
        {
            uint32_t sLine = range.startLine;
            uint32_t sChar = range.startCharacter;
            uint32_t eLine = range.endLine;
            uint32_t eChar = range.endCharacter;

            if (i < def.templateArgPositions.size())
            {
                sLine = def.templateArgPositions[i].startLine;
                sChar = def.templateArgPositions[i].startCharacter;
                eLine = def.templateArgPositions[i].endLine;
                eChar = def.templateArgPositions[i].endCharacter;
            }

            ctx.EmitAtRange({sLine, sChar, eLine, eChar}, "as-err-unresolved-type", cleanArg,
                            DiagnosticSeverity::Error);
        }
    }
}

bool CheckMissingFuncdefHint(const std::string& base, const SourceRange& range, DiagnosticContext& ctx)
{
    if (ctx.request.diagnostics && ctx.request.diagnostics->reportMissingFuncdef && base != "auto" &&
        NamesAFunctionNotAType(base, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(range, "as-hint-funcdef-missing", base, DiagnosticSeverity::Hint);
        return true;
    }
    return false;
}

bool CheckIllegalHandleOnPrimitive(const LocalDefinition& def, const std::string& base, const SourceRange& range,
                                   DiagnosticContext& ctx)
{
    if (def.isHandleType && def.typeKind != TypeKind::Array && base != "auto" && IsPrimitiveTypeName(base))
    {
        ctx.EmitAtRange(range, "as-err-handle-on-primitive", base, DiagnosticSeverity::Error);
        return true;
    }
    return false;
}

std::string ResolveEnclosingNamespaceForDefinition(const LocalDefinition& def, const DiagnosticContext& ctx)
{
    if (!ctx.request.tree || ctx.request.sourceCode.empty())
    {
        return "";
    }
    const TSNode node = ts_node_descendant_for_point_range(
        ts_tree_root_node(ctx.request.tree), {def.startLine, def.startCharacter}, {def.endLine, def.endCharacter});
    if (ts_node_is_null(node))
    {
        return "";
    }
    for (const auto& container : GetEnclosingContainers(node, ctx.request.sourceCode))
    {
        if (container.kind == ContainerKind::Namespace)
        {
            return container.qualifiedName;
        }
    }
    return "";
}

void ValidateVariableType(const LocalDefinition& def, DiagnosticContext& ctx)
{
    const SourceRange range = GetDefinitionTypeRange(def);
    const std::string base = CleanBaseType(def.typeName);

    if (def.typeName == "void" || base == "void")
    {
        if (def.kind == LocalDefinitionKind::Parameter)
        {
            ctx.EmitAtRange(range, "as-err-void-parameter", {def.name, "<anonymous>"}, DiagnosticSeverity::Error);
        }
        else
        {
            ctx.EmitAtRange(range, "as-err-void-variable", def.name, DiagnosticSeverity::Error);
        }
        return;
    }
    if (IsMixinClass(base, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(range, "as-err-mixin-not-a-type", base, DiagnosticSeverity::Error);
        return;
    }
    if (CheckMissingFuncdefHint(base, range, ctx) || CheckIllegalHandleOnPrimitive(def, base, range, ctx))
    {
        return;
    }

    const std::string enclosingNs = ResolveEnclosingNamespaceForDefinition(def, ctx);
    const TemplateTypeInfo tmplInfo = ParseTemplateType(def.typeName);
    if (!tmplInfo.templateArgs.empty())
    {
        ValidateTemplateArguments(def, tmplInfo, enclosingNs, ctx);
    }
    else if (!base.empty() && base != "auto" && !IsReservedKeyword(base) && !IsKnownType(base, ctx, enclosingNs))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", base, DiagnosticSeverity::Error);
    }

    rules::CheckEngineLocalTypeSuggestions(def, ctx);
}
} // namespace

void SemanticAnalyzer::CheckLocalNames(const Scope* scope, DiagnosticContext& ctx) const
{
    // AngelScript rejects a name declared twice in one scope, and counts a function's
    // parameters as belonging to its body.
    ankerl::unordered_dense::set<std::string> declaredHere;

    if (scope->parent && scope->parent->isFunctionScope)
    {
        for (const auto& param : scope->parent->definitions)
        {
            if (param.kind == LocalDefinitionKind::Parameter)
                declaredHere.insert(param.name);
        }
    }

    for (const auto& def : scope->definitions)
    {
        if (def.kind != LocalDefinitionKind::Variable && def.kind != LocalDefinitionKind::Parameter)
            continue;

        if (!declaredHere.insert(def.name).second)
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            "as-err-duplicate-symbol", def.name);
        }

        if (IsReservedKeyword(def.name) && def.kind == LocalDefinitionKind::Variable &&
            IsKnownType(CleanBaseType(def.typeName), ctx))
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            "as-err-reserved-keyword-name", def.name);
        }
    }
}

void SemanticAnalyzer::CheckLocalTypes(const Scope* scope, DiagnosticContext& ctx) const
{
    for (const auto& def : scope->definitions)
    {
        if (def.kind == LocalDefinitionKind::Variable ||
            (scope->kind == ScopeKind::Closure && def.kind == LocalDefinitionKind::Parameter))
        {
            ValidateVariableType(def, ctx);
        }
    }
}

void SemanticAnalyzer::CheckLocalVariableDeclarations(const Scope* rootScope, DiagnosticContext& ctx) const
{
    if (!rootScope)
    {
        return;
    }

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        bool isFunctionNested = false;
        for (const Scope* ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            CheckLocalNames(scope, ctx);
            CheckLocalTypes(scope, ctx);
        }

        for (const auto& child : scope->children)
        {
            if (child)
            {
                worklist.push_back(child.get());
            }
        }
    }
}
} // namespace angel_lsp::analysis
