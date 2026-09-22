#include "analysis/SemanticAnalyzer.h"
#include "analysis/ASTUtils.h"
#include "analysis/AccessChecker.h"
#include "analysis/CallChecker.h"
#include "analysis/ConstChecker.h"
#include "analysis/ControlFlowChecker.h"
#include "analysis/DefiniteAssignmentChecker.h"
#include "analysis/InitializerListChecker.h"
#include "analysis/IsolationChecker.h"
#include "analysis/LValueChecker.h"
#include "analysis/NamespaceChecker.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/rules/TypeRules.h"
#include "analysis/rules/VariableRules.h"
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

void SemanticAnalyzer::RunScopeRules(const SemanticAnalysisRequest& request, DiagnosticContext& ctx) const
{
    if (!request.scopeRoot)
    {
        return;
    }
    CheckUndefinedIdentifiers(request.scopeRoot.get(), request.GetRuleIndex().allNames, ctx);

    ankerl::unordered_dense::set<const LocalDefinition*> used;
    CollectUsedDefinitions(request.scopeRoot.get(), used);
    CheckUnusedVariables(request.scopeRoot.get(), used, ctx);

    CheckNullAssignedToNonHandleInScope(request.scopeRoot.get(), ctx);
    CheckLocalVariableDeclarations(request.scopeRoot.get(), ctx);
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
    else
    {
        rules::ValidateStandaloneLambda(ts_tree_root_node(request.tree), ctx);
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
    if (indexPtr)
    {
        CheckEngineDialectRules(*indexPtr, ctx);
    }
    else
    {
        CheckEngineDialectRules(ts_tree_root_node(request.tree), ctx);
    }
}

void SemanticAnalyzer::RunTypeAndStructureRules(const SemanticAnalysisRequest& request, const NodeIndex* indexPtr,
                                                DiagnosticContext& ctx) const
{
    if (request.enableTypeConversionChecks)
    {
        const TypeConversionCheckRequest conversionRequest{ts_tree_root_node(request.tree), request.sourceCode,
                                                           request.scopeRoot.get(), request.mutableScopeRoot, indexPtr};
        CheckTypeConversions(conversionRequest, ctx);
    }

    const IsolationCheckRequest isolationRequest{ts_tree_root_node(request.tree), request.sourceCode,
                                                 request.scopeRoot.get()};
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

std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest& request) const
{
    std::vector<Diagnostic> diagnostics;
    LogSymbolDump(request);

    {
        DiagnosticContext ctx{request, diagnostics, m_logger};
        CheckNullAssignedToNonHandle(request.symbolTable, ctx);
        CheckDeclarationRules(request.symbolTable, ctx);

        utils::HighResTimer scopeTimer;
        RunScopeRules(request, ctx);
        double scopeMs = scopeTimer.ElapsedMs();

        std::unique_ptr<NodeIndex> localNodeIndex;
        const NodeIndex* indexPtr = request.nodeIndex;
        if (!indexPtr && request.tree)
        {
            localNodeIndex =
                std::make_unique<NodeIndex>(ts_tree_root_node(request.tree), nullptr, request.traversalBudget);
            indexPtr = localNodeIndex.get();
        }

        double stmtMs = 0.0;
        double exprMs = 0.0;
        double typeMs = 0.0;
        if (request.tree && !request.sourceCode.empty())
        {
            utils::HighResTimer stmtTimer;
            RunStatementAndControlFlowRules(request, indexPtr, ctx);
            stmtMs = stmtTimer.ElapsedMs();

            utils::HighResTimer exprTimer;
            RunExpressionRules(request, indexPtr, ctx);
            exprMs = exprTimer.ElapsedMs();

            utils::HighResTimer typeTimer;
            RunTypeAndStructureRules(request, indexPtr, ctx);
            typeMs = typeTimer.ElapsedMs();
        }

        CheckDirectivesAndModules(request, ctx);

        if (m_logger && m_logger->IsEnabled(utils::LogLevel::Info))
        {
            m_logger->LogInfo(fmt::format("[Checkers Breakdown] File: {} | ScopeRules: {:.2f} ms, StmtFlow: {:.2f} ms, "
                                          "ExprRules: {:.2f} ms, TypeRules: {:.2f} ms",
                                          request.fileUri, scopeMs, stmtMs, exprMs, typeMs));
        }
    }

    if (!request.excludedLineRanges.empty())
    {
        std::erase_if(diagnostics, [&request](const Diagnostic& d)
                      { return utils::IsLineExcluded(request.excludedLineRanges, d.range.start.line); });
    }

    return diagnostics;
}

static void CheckDialectForeach(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (!ctx.request.SupportsForeach())
    {
        for (TSNode node : nodeIndex.Nodes(parser::nodes::ForeachStatement))
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
        for (TSNode node : nodeIndex.Nodes(parser::nodes::InitializerList))
        {
            const uint32_t childCount = ts_node_child_count(node);
            for (uint32_t i = 1; i < childCount; ++i)
            {
                const std::string_view previous = ts_node_type(ts_node_child(node, i - 1));
                const std::string_view current = ts_node_type(ts_node_child(node, i));
                if (previous == "," && (current == "," || current == "}"))
                {
                    const TSPoint at = ts_node_start_point(ts_node_child(node, i - 1));
                    ctx.EmitAtRange({at.row, at.column, at.row, at.column + 1}, "as-err-empty-list-element",
                                    DiagnosticSeverity::Error);
                }
            }
        }
    }
}

static void CheckCharLiteralDeclarator(TSNode declarator, const std::string& declared, DiagnosticContext& ctx)
{
    if (std::string_view(ts_node_type(declarator)) != "variable_declarator")
    {
        return;
    }

    const TSNode value = parser::GetChildByField(declarator, parser::fields::Value);
    if (ts_node_is_null(value) || std::string_view(ts_node_type(value)) != "string_literal")
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

    const TSNode typeNode = parser::GetChildByField(node, parser::fields::VarType);
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

    for (TSNode node : nodeIndex.Nodes(parser::nodes::VariableDeclaration))
    {
        CheckDialectVariableDeclarationNode(node, ctx);
    }
}

static bool IsIntegerLiteral(TSNode candidate, std::string_view sourceCode)
{
    if (ts_node_is_null(candidate) || std::string_view(ts_node_type(candidate)) != "number_literal")
    {
        return false;
    }
    const std::string text = GetNodeText(candidate, sourceCode);
    return text.find_first_of(".eEfF") == std::string::npos;
}

static void CheckDialectIntegerDivisionNode(TSNode node, DiagnosticContext& ctx)
{
    const TSNode op = parser::GetChildByField(node, parser::fields::Operator);
    if (ts_node_is_null(op) || std::string_view(ts_node_type(op)) != "/")
    {
        return;
    }

    const TSNode left = parser::GetChildByField(node, parser::fields::Left);
    const TSNode right = parser::GetChildByField(node, parser::fields::Right);

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
        for (TSNode node : nodeIndex.Nodes(parser::nodes::BinaryExpression))
        {
            CheckDialectIntegerDivisionNode(node, ctx);
        }
    }
}

static void CheckDialectNamedArguments(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (ctx.request.NamedArgumentSyntaxMode() != 2)
    {
        for (TSNode node : nodeIndex.Nodes(parser::nodes::ArgumentList))
        {
            const uint32_t argCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < argCount; ++i)
            {
                const TSNode argument = ts_node_named_child(node, i);
                if (std::string_view(ts_node_type(argument)) != "assignment_expression")
                {
                    continue;
                }

                const TSNode target = parser::GetChildByField(argument, parser::fields::Left);
                if (ts_node_is_null(target))
                {
                    continue;
                }
                const std::string_view targetType = ts_node_type(target);
                if (targetType != "identifier" && targetType != "scoped_identifier")
                {
                    continue;
                }
                if (targetType == "scoped_identifier" && ts_node_named_child_count(target) != 1)
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

static bool IsHandleAssignmentTarget(TSNode target)
{
    if (std::string_view(ts_node_type(target)) != "unary_expression")
    {
        return false;
    }
    const TSNode prefix = parser::GetChildByField(target, parser::fields::Operator);
    return !ts_node_is_null(prefix) && std::string_view(ts_node_type(prefix)) == "@";
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
    const TSNode op = parser::GetChildByField(node, parser::fields::Operator);
    const TSNode target = parser::GetChildByField(node, parser::fields::Left);

    if (ts_node_is_null(op) || std::string_view(ts_node_type(op)) != "=" || ts_node_is_null(target))
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
    const std::string targetType = CleanBaseType(
        ResolveExpressionType(target, {scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.fileUri}));

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
        for (TSNode node : nodeIndex.Nodes(parser::nodes::AssignmentExpression))
        {
            CheckDialectAssignmentExpressionNode(node, ctx);
        }
    }
}

static void CheckDialectMultilineStrings(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    if (!ctx.request.AllowsMultilineStrings())
    {
        for (TSNode node : nodeIndex.Nodes(parser::nodes::StringLiteral))
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

static void CheckDialectNodeForeach(TSNode node, DiagnosticContext& ctx)
{
    if (!ctx.request.SupportsForeach())
    {
        const TSPoint start = ts_node_start_point(node);
        ctx.EmitAtRange({start.row, start.column, start.row, start.column + 7}, "as-err-foreach-unsupported",
                        DiagnosticSeverity::Error);
    }
}

static void CheckDialectNodeInitializerList(TSNode node, DiagnosticContext& ctx)
{
    if (ctx.request.DisallowsEmptyListElements())
    {
        const uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 1; i < childCount; ++i)
        {
            const std::string_view previous = ts_node_type(ts_node_child(node, i - 1));
            const std::string_view current = ts_node_type(ts_node_child(node, i));
            if (previous == "," && (current == "," || current == "}"))
            {
                const TSPoint at = ts_node_start_point(ts_node_child(node, i - 1));
                ctx.EmitAtRange({at.row, at.column, at.row, at.column + 1}, "as-err-empty-list-element",
                                DiagnosticSeverity::Error);
            }
        }
    }
}

static void CheckDialectNodeVariableDeclaration(TSNode node, DiagnosticContext& ctx)
{
    CheckDialectVariableDeclarationNode(node, ctx);
}

static void CheckDialectNodeBinaryExpression(TSNode node, DiagnosticContext& ctx)
{
    if (ctx.request.diagnostics && ctx.request.diagnostics->reportIntegerDivision &&
        !ctx.request.DisablesIntegerDivision())
    {
        CheckDialectIntegerDivisionNode(node, ctx);
    }
}

static void CheckDialectNodeArgumentList(TSNode node, DiagnosticContext& ctx)
{
    if (ctx.request.NamedArgumentSyntaxMode() != 2)
    {
        const uint32_t argCount = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < argCount; ++i)
        {
            const TSNode argument = ts_node_named_child(node, i);
            if (std::string_view(ts_node_type(argument)) != "assignment_expression")
            {
                continue;
            }

            const TSNode target = parser::GetChildByField(argument, parser::fields::Left);
            if (ts_node_is_null(target))
            {
                continue;
            }
            const std::string_view targetType = ts_node_type(target);
            if (targetType != "identifier" && targetType != "scoped_identifier")
            {
                continue;
            }
            if (targetType == "scoped_identifier" && ts_node_named_child_count(target) != 1)
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

static void CheckDialectNodeAssignmentExpression(TSNode node, DiagnosticContext& ctx)
{
    if (ctx.request.DisallowsValueAssignForRef())
    {
        CheckDialectAssignmentExpressionNode(node, ctx);
    }
}

static void CheckDialectNodeStringLiteral(TSNode node, DiagnosticContext& ctx)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);

    if (end.row > start.row && !ctx.request.AllowsMultilineStrings())
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

static void ProcessDialectNode(TSNode node, DiagnosticContext& ctx)
{
    const std::string_view engineNodeType = ts_node_type(node);
    if (engineNodeType == "foreach_statement")
    {
        CheckDialectNodeForeach(node, ctx);
    }
    else if (engineNodeType == "initializer_list")
    {
        CheckDialectNodeInitializerList(node, ctx);
    }
    else if (engineNodeType == "variable_declaration")
    {
        CheckDialectNodeVariableDeclaration(node, ctx);
    }
    else if (engineNodeType == "binary_expression")
    {
        CheckDialectNodeBinaryExpression(node, ctx);
    }
    else if (engineNodeType == "argument_list")
    {
        CheckDialectNodeArgumentList(node, ctx);
    }
    else if (engineNodeType == "assignment_expression")
    {
        CheckDialectNodeAssignmentExpression(node, ctx);
    }
    else if (engineNodeType == "string_literal")
    {
        CheckDialectNodeStringLiteral(node, ctx);
    }
}

void SemanticAnalyzer::CheckEngineDialectRules(TSNode node, DiagnosticContext& ctx, int depth) const
{
    if (ts_node_is_null(node) || depth > k_maxAstDepth)
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(node);
    bool visiting = true;
    while (visiting)
    {
        TSNode current = ts_tree_cursor_current_node(&cursor);
        ProcessDialectNode(current, ctx);

        std::string_view nodeType = ts_node_type(current);
        if (nodeType == "string_literal" || !ts_tree_cursor_goto_first_child(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                continue;
            }
            while (true)
            {
                if (!ts_tree_cursor_goto_parent(&cursor))
                {
                    visiting = false;
                    break;
                }
                if (ts_tree_cursor_goto_next_sibling(&cursor))
                {
                    break;
                }
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
}

static void DispatchDeclarationRule(const Symbol& sym, DiagnosticContext& ctx)
{
    switch (sym.type)
    {
    case SymbolType::Class:
        rules::ValidateClass(sym, ctx);
        break;
    case SymbolType::Interface:
        rules::ValidateClass(sym, ctx);
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
    symbolTable.ForEachSymbolInFile(
        ctx.request.fileUri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            rules::ValidateDuplicates(symbols, ctx);
            for (const auto& sym : symbols)
            {
                if (sym.fileUri == ctx.request.fileUri)
                {
                    DispatchDeclarationRule(sym, ctx);
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
    if (ref.isMemberAccess || ref.isTypeSpecifier)
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

bool CheckEnumScopeDiagnostic(const Scope* scope, const LocalReference& ref, const LocalDefinition* resolved,
                              DiagnosticContext& ctx)
{
    // asEP_REQUIRE_ENUM_SCOPE, checked before the resolution is acted on, because an
    // enumerator DOES resolve from the scope tree - LOCALS_QUERY captures `enum_member` as
    // a definition - and continuing on that is what made the first attempt at this rule
    // never run.
    if (!ctx.request.RequiresEnumScope() || resolved == nullptr || resolved->kind != LocalDefinitionKind::Constant ||
        !ctx.request.GetRuleIndex().enumMemberNames.contains(ref.name))
    {
        return false;
    }

    bool insideFunction = false;
    for (const Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (current->isFunctionScope)
        {
            insideFunction = true;
            break;
        }
    }

    const bool isOwnDeclaration =
        resolved->startLine == ref.startLine && resolved->startCharacter == ref.startCharacter;

    if (insideFunction && !isOwnDeclaration)
    {
        ctx.EmitAtRange({ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter},
                        "as-err-enum-scope-required", ref.name);
        return true;
    }
    return false;
}

bool IsAccessorPropertyOrKeyword(const LocalReference& ref, const DiagnosticContext& ctx)
{
    const auto& index = ctx.request.GetRuleIndex();
    const auto& accessorNames =
        ctx.request.RequiresAccessorKeyword() ? index.keywordAccessorPropertyNames : index.accessorPropertyNames;
    if (accessorNames.contains(ref.name))
        return true;

    return IsReservedKeyword(ref.name);
}

void CheckScopeReferences(
    const Scope* scope,
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames,
    const MixinRanges& mixinRanges, DiagnosticContext& ctx)
{
    for (const auto& ref : scope->references)
    {
        if (ShouldIgnoreReference(ref, ctx) || IsReferenceInMixin(ref, mixinRanges))
            continue;

        const LocalDefinition* resolved = ResolveInScope(scope, ref.name);
        if (CheckEnumScopeDiagnostic(scope, ref, resolved, ctx))
            continue;

        if (resolved != nullptr || knownGlobalNames.contains(ref.name))
            continue;

        if (IsAccessorPropertyOrKeyword(ref, ctx))
            continue;

        ctx.EmitAtRange({ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter},
                        "as-warn-undeclared-identifier", ref.name, DiagnosticSeverity::Warning);
    }
}
struct UndefinedIdentifierContext
{
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames;
    const MixinRanges& mixinRanges;
    DiagnosticContext& ctx;
};

void CheckUndefinedIdentifiersRecursive(const Scope* scope, const UndefinedIdentifierContext& uCtx, int depth)
{
    if (depth > k_maxAstDepth || !scope)
    {
        return;
    }

    CheckScopeReferences(scope, uCtx.knownGlobalNames, uCtx.mixinRanges, uCtx.ctx);

    for (const auto& child : scope->children)
    {
        CheckUndefinedIdentifiersRecursive(child.get(), uCtx, depth + 1);
    }
}
} // namespace

void SemanticAnalyzer::CheckUndefinedIdentifiers(
    const Scope* scope,
    const ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>>& knownGlobalNames,
    DiagnosticContext& ctx, int depth) const
{
    // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth || !scope)
    {
        return;
    }

    const MixinRanges mixinRanges = CollectMixinRanges(ctx.request.symbolTable, ctx.request.fileUri);
    const UndefinedIdentifierContext uCtx{knownGlobalNames, mixinRanges, ctx};
    CheckUndefinedIdentifiersRecursive(scope, uCtx, depth);
}

void SemanticAnalyzer::CollectUsedDefinitions(const Scope* scope,
                                              ankerl::unordered_dense::set<const LocalDefinition*>& used,
                                              int depth) const
{
    // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth)
        return;

    for (const auto& ref : scope->references)
    {
        if (ref.isMemberAccess)
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
        CollectUsedDefinitions(child.get(), used, depth + 1);
}

void SemanticAnalyzer::CheckUnusedVariables(const Scope* scope,
                                            const ankerl::unordered_dense::set<const LocalDefinition*>& used,
                                            DiagnosticContext& ctx, int depth) const
{
    // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth)
        return;

    // A Variable-kind definition only counts as a true local (as opposed to a module/
    // namespace-scope global, which LOCALS_QUERY's @local.definition.var captures under the
    // identical kind - its own comment says "Variables (locals and globals)") if it or some
    // ancestor scope was opened by a function/lambda. Globals need cross-file visibility
    // (another file in the same workspace, or the engine itself, may reference them) that a
    // single document's Scope tree can never see - confirmed by corpus spot-check, where
    // module-level declarations like "string WPN_NAME = ...;" were being flagged "unused"
    // even though "unused" isn't decidable from this file alone for those.
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
        CheckUnusedVariables(child.get(), used, ctx, depth + 1);
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
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
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

void SemanticAnalyzer::CheckNullAssignedToNonHandleInScope(const Scope* scope, DiagnosticContext& ctx, int depth) const
{
    // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth)
        return;

    // Same isFunctionScope ancestor-walk as CheckUnusedVariables: a Variable-kind definition
    // only counts as a true function-body local (not a module/namespace/class-scope global or
    // field, which LOCALS_QUERY's @local.definition.var captures under the identical kind) if
    // it or some ancestor scope was opened by a function/lambda. Module/class-scope
    // declarations are already covered by CheckNullAssignedToNonHandle via SymbolTable.
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
        CheckNullAssignedToNonHandleInScope(child.get(), ctx, depth + 1);
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

void ValidateTemplateArguments(const LocalDefinition& def, const TemplateTypeInfo& tmplInfo, const SourceRange& range,
                               DiagnosticContext& ctx)
{
    if (!IsKnownType(tmplInfo.containerName, ctx))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", tmplInfo.containerName, DiagnosticSeverity::Error);
    }
    for (size_t i = 0; i < tmplInfo.templateArgs.size(); ++i)
    {
        std::string cleanArg = CleanBaseType(tmplInfo.templateArgs[i]);
        if (!IsKnownType(cleanArg, ctx))
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

void ValidateVariableType(const LocalDefinition& def, DiagnosticContext& ctx)
{
    const SourceRange range = GetDefinitionTypeRange(def);
    const std::string base = CleanBaseType(def.typeName);

    if (def.typeName == "void" || base == "void")
    {
        ctx.EmitAtRange(range, "as-err-void-variable", def.name, DiagnosticSeverity::Error);
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

    const TemplateTypeInfo tmplInfo = ParseTemplateType(def.typeName);
    if (!tmplInfo.templateArgs.empty())
    {
        ValidateTemplateArguments(def, tmplInfo, range, ctx);
    }
    else if (!base.empty() && base != "auto" && !IsReservedKeyword(base) && !IsKnownType(base, ctx))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", base, DiagnosticSeverity::Error);
    }
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
        if (def.kind == LocalDefinitionKind::Variable)
        {
            ValidateVariableType(def, ctx);
        }
    }
}

void SemanticAnalyzer::CheckLocalVariableDeclarations(const Scope* scope, DiagnosticContext& ctx, int depth) const
{
    // Scope trees nest as deeply as the source blocks do; see k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth || !scope)
        return;

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
        CheckLocalVariableDeclarations(child.get(), ctx, depth + 1);
    }
}
} // namespace angel_lsp::analysis
