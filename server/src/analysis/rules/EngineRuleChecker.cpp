#include "analysis/rules/EngineRuleChecker.h"

#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "i18n/i18n.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"
#include "spdlog/fmt/fmt.h"
#include "utils/Utils.h"

#include <algorithm>
#include <regex>

namespace angel_lsp::analysis::rules
{

static DiagnosticSeverity ResolveSeverity(std::string_view sev)
{
    if (sev == "error")
    {
        return DiagnosticSeverity::Error;
    }
    if (sev == "hint")
    {
        return DiagnosticSeverity::Hint;
    }
    if (sev == "info" || sev == "information")
    {
        return DiagnosticSeverity::Information;
    }
    return DiagnosticSeverity::Warning;
}

static bool MatchesTypeName(const std::string& baseType, const config::StorageRule& rule, const SymbolTable& table)
{
    if (std::find(rule.types.begin(), rule.types.end(), baseType) != rule.types.end())
    {
        return true;
    }
    if (!rule.inheritsFrom.empty())
    {
        if (baseType == rule.inheritsFrom)
        {
            return true;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(baseType, table);
        if (std::find(hierarchy.begin(), hierarchy.end(), rule.inheritsFrom) != hierarchy.end())
        {
            return true;
        }
    }
    if (!rule.typeRegex.empty())
    {
        try
        {
            const std::regex re(rule.typeRegex);
            if (std::regex_match(baseType, re))
            {
                return true;
            }
        }
        catch (const std::regex_error&)
        {
            // Ignore malformed regex pattern.
        }
    }
    return false;
}

static std::string ExtractArrayElement(const VariableSignature& sig, const SemanticAnalysisRequest& request)
{
    if (!sig.templateName.empty() && request.IsArrayLikeTemplate(sig.templateName))
    {
        const auto parsed = ParseTemplateType(sig.typeName);
        if (!parsed.templateArgs.empty())
        {
            return parsed.templateArgs.front();
        }
        if (!sig.templateArgumentTypes.empty())
        {
            return sig.templateArgumentTypes.front();
        }
    }
    return {};
}

static void ReportStorageViolation(const Symbol& sym, const config::StorageRule& rule, const DiagnosticContext& ctx)
{
    std::string msg = rule.message;
    if (msg.empty())
    {
        msg = i18n::FormatMessage(ctx.request.i18n, "engine-rule-storage-default",
                                  "Variable '{}' violates storage rule '{}'. Consider using '{}'.", sym.name, rule.id,
                                  rule.suggestReplacement);
    }
    else
    {
        msg = fmt::format(fmt::runtime(msg), sym.name, rule.suggestReplacement);
    }

    ctx.Emit(sym, diagnostics::codes::EngineStorageRule, msg, ResolveSeverity(rule.severity));
}

/**
 * @brief Validates a variable symbol against an engine storage restriction rule (e.g. member/global/array bans).
 * @param[in] sym Variable symbol being validated.
 * @param[in] sig Variable signature details containing type and modifiers.
 * @param[in] rule Storage rule specifying disallowed scopes and type patterns.
 * @param[in] ctx Diagnostic collection and analysis context.
 */
static void EvaluateStorageRule(const Symbol& sym, const VariableSignature& sig, const config::StorageRule& rule,
                                const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const bool isMember = HasContainerOfType(sym, table, SymbolType::Class);
    const bool isGlobal = !isMember && sym.containerName.empty();
    const std::string arrayElem = ExtractArrayElement(sig, ctx.request);
    const bool isArray = !arrayElem.empty();

    const std::string& targetType = isArray ? arrayElem : sig.baseTypeName;
    const std::string cleanTarget = CleanBaseType(targetType, ctx.request.GetArrayTypeName());

    if (!MatchesTypeName(cleanTarget, rule, table))
    {
        return;
    }

    const bool hasHandle = isArray ? IsHandleTypeView(targetType) : sig.modifiers.isHandle;
    if (rule.handleOnly && !hasHandle)
    {
        return;
    }

    if ((rule.disallowInMembers && isMember) || (rule.disallowInGlobals && isGlobal) ||
        (rule.disallowInArrays && isArray))
    {
        ReportStorageViolation(sym, rule, ctx);
    }
}

/**
 * @brief Verifies that an argument passed to a scheduler invocation satisfies engine safety constraints.
 * @param[in] req Scheduler call request details including arguments and callee.
 * @param[in] index Argument index being checked.
 * @param[in] rule Scheduler rule specifying parameter requirements.
 * @param[in] ctx Diagnostic context used to report unsafe argument warnings.
 */
static void CheckSchedulerArgSafety(const SchedulerCallRequest& req, size_t index, const config::SchedulerRule& rule,
                                    const DiagnosticContext& ctx)
{
    if (rule.disallowArgInheritsFrom.empty() && rule.disallowArgRegex.empty())
    {
        return;
    }

    const std::string& argType = req.argTypes[index];
    const TSNode argNode = req.argNodes[index];
    const bool hasHandle = IsHandleTypeView(argType);
    if (rule.disallowArgHandleOnly && !hasHandle)
    {
        return;
    }

    config::StorageRule checkRule{"",
                                  {},
                                  rule.disallowArgInheritsFrom,
                                  rule.disallowArgRegex,
                                  rule.disallowArgHandleOnly,
                                  false,
                                  false,
                                  false,
                                  rule.argSuggestReplacement,
                                  "",
                                  ""};
    const std::string cleanTarget = CleanBaseType(argType, ctx.request.GetArrayTypeName());
    if (MatchesTypeName(cleanTarget, checkRule, ctx.request.symbolTable))
    {
        std::string msg =
            rule.message.empty()
                ? i18n::FormatMessage(ctx.request.i18n, "engine-rule-scheduler-safety-default",
                                      "Passing raw handle '{}' to '{}' is unsafe.", argType, req.methodName)
                : rule.message;
        const TSPoint start = ts_node_start_point(argNode);
        const TSPoint end = ts_node_end_point(argNode);
        ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                        diagnostics::codes::EngineSchedulerSafety, msg, ResolveSeverity(rule.severity));
    }
}

static void CheckSchedulerEnumDiscriminant(const SchedulerCallRequest& req, size_t index,
                                           const config::SchedulerRule& rule, const DiagnosticContext& ctx)
{
    if (!rule.requireExplicitEnumConstruct)
    {
        return;
    }

    const std::string& argType = req.argTypes[index];
    const TSNode argNode = req.argNodes[index];
    if (!parser::primitives::IsInteger(CleanBaseType(argType, ctx.request.GetArrayTypeName())))
    {
        return;
    }

    for (const auto* cand : req.candidates)
    {
        if (!cand || !std::holds_alternative<FunctionSignature>(cand->signature))
        {
            continue;
        }
        const auto& sig = std::get<FunctionSignature>(cand->signature);
        if (index < sig.parameters.size())
        {
            const auto& param = sig.parameters[index];
            if (HasSymbolOfType(ctx.request.symbolTable, param.baseTypeName, SymbolType::Enum))
            {
                std::string msg = i18n::FormatMessage(
                    ctx.request.i18n, "engine-rule-scheduler-enum-discriminant",
                    "Passing discriminant to enum type '{}' in scheduled call; explicitly construct or typecast first "
                    "(e.g. {}({})).",
                    param.baseTypeName, param.baseTypeName, parser::GetNodeText(argNode, ctx.request.sourceCode));
                const TSPoint start = ts_node_start_point(argNode);
                const TSPoint end = ts_node_end_point(argNode);
                ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                                diagnostics::codes::EngineScheduledEnumDiscriminant, msg, DiagnosticSeverity::Warning);
                break;
            }
        }
    }
}

static void CheckSingleSchedulerArg(const SchedulerCallRequest& req, size_t index, const config::SchedulerRule& rule,
                                    const DiagnosticContext& ctx)
{
    const std::string& argType = req.argTypes[index];
    const TSNode argNode = req.argNodes[index];
    if (argType.empty() || ts_node_is_null(argNode))
    {
        return;
    }

    CheckSchedulerArgSafety(req, index, rule, ctx);
    CheckSchedulerEnumDiscriminant(req, index, rule, ctx);
}

static std::string FormatTypeSuggestionMessage(std::string_view varName, const config::TypeSuggestionRule& suggestion,
                                               const DiagnosticContext& ctx)
{
    if (!suggestion.message.empty())
    {
        return suggestion.message;
    }
    return i18n::FormatMessage(ctx.request.i18n, "engine-rule-type-suggestion-default",
                               "Variable '{}' uses '{}'. Consider using '{}'.", varName, suggestion.fromType,
                               suggestion.toType);
}

void CheckEngineStorageRules(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (!ctx.request.engineRules || ctx.request.engineRules->storageRules.empty())
    {
        return;
    }

    for (const auto& rule : ctx.request.engineRules->storageRules)
    {
        EvaluateStorageRule(sym, sig, rule, ctx);
    }
}

void CheckEngineTypeSuggestions(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (!ctx.request.engineRules || ctx.request.engineRules->typeSuggestions.empty())
    {
        return;
    }

    for (const auto& s : ctx.request.engineRules->typeSuggestions)
    {
        if (sig.baseTypeName == s.fromType)
        {
            const std::string msg = FormatTypeSuggestionMessage(sym.name, s, ctx);
            ctx.Emit(sym, diagnostics::codes::EngineTypeSuggestion, msg, ResolveSeverity(s.severity));
        }
    }
}

void CheckEngineLocalTypeSuggestions(const LocalDefinition& def, DiagnosticContext& ctx)
{
    if (!ctx.request.engineRules || ctx.request.engineRules->typeSuggestions.empty())
    {
        return;
    }

    const std::string base = CleanBaseType(def.typeName, ctx.request.GetArrayTypeName());
    for (const auto& s : ctx.request.engineRules->typeSuggestions)
    {
        if (base == s.fromType)
        {
            const std::string msg = FormatTypeSuggestionMessage(def.name, s, ctx);
            const SourceRange range{def.typeStartLine, def.typeStartCharacter, def.typeEndLine, def.typeEndCharacter};
            ctx.EmitAtRange(range, diagnostics::codes::EngineTypeSuggestion, msg, ResolveSeverity(s.severity));
        }
    }
}

static std::string_view ExtractReceiverText(TSNode callNode, std::string_view sourceCode) noexcept
{
    if (ts_node_is_null(callNode))
    {
        return {};
    }
    TSNode callee = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldFunction);
    if (ts_node_is_null(callee) && ts_node_child_count(callNode) > 0)
    {
        callee = ts_node_child(callNode, 0);
    }
    if (ts_node_is_null(callee))
    {
        return {};
    }
    TSNode objectNode = parser::GetChildByField(callee, parser::GetGrammarSymbols().fieldObject);
    if (!ts_node_is_null(objectNode))
    {
        return parser::GetNodeTextView(objectNode, sourceCode);
    }
    return {};
}

static bool RuleMatchesReceiver(const SchedulerCallRequest& req, const config::SchedulerRule& rule,
                                const DiagnosticContext& ctx)
{
    if (rule.receiverTypes.empty())
    {
        return true;
    }

    const std::string_view receiverText = ExtractReceiverText(req.callNode, ctx.request.sourceCode);
    if (!receiverText.empty())
    {
        for (const auto& rcv : rule.receiverTypes)
        {
            if (rcv == receiverText)
            {
                return true;
            }
        }
    }

    for (const auto* cand : req.candidates)
    {
        if (cand)
        {
            for (const auto& rcv : rule.receiverTypes)
            {
                if (rcv == cand->containerName)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

void CheckEngineSchedulerCall(const SchedulerCallRequest& req, const DiagnosticContext& ctx)
{
    if (!ctx.request.engineRules || ctx.request.engineRules->schedulerRules.empty())
    {
        return;
    }

    for (const auto& rule : ctx.request.engineRules->schedulerRules)
    {
        if (std::find(rule.methodNames.begin(), rule.methodNames.end(), req.methodName) == rule.methodNames.end())
        {
            continue;
        }

        if (!RuleMatchesReceiver(req, rule, ctx))
        {
            continue;
        }

        if (rule.disallowMethod)
        {
            std::string msg = rule.message;
            if (msg.empty())
            {
                msg = i18n::FormatMessage(ctx.request.i18n, diagnostics::codes::EngineCallSafetyDisallowed,
                                          "Calling '{0}' is disallowed for safety reasons. Use '{1}' instead.",
                                          req.methodName, rule.suggestReplacement);
            }
            else if (msg.find("{}") != std::string::npos)
            {
                msg = fmt::format(fmt::runtime(msg), req.methodName, rule.suggestReplacement);
            }
            TSPoint start = ts_node_start_point(req.callNode);
            TSPoint end = ts_node_end_point(req.callNode);
            ctx.EmitAtRange(SourceRange{start.row, start.column, end.row, end.column},
                            diagnostics::codes::EngineCallSafety, msg, ResolveSeverity(rule.severity));
            return;
        }

        const size_t count = std::min(req.argNodes.size(), req.argTypes.size());
        for (size_t i = 0; i < count; ++i)
        {
            CheckSingleSchedulerArg(req, i, rule, ctx);
        }
    }
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

    const std::string declared = CleanBaseType(parser::GetNodeText(typeNode, ctx.request.sourceCode));
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
    const std::string text = parser::GetNodeText(candidate, sourceCode);
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
                const std::string name = parser::GetNodeText(target, ctx.request.sourceCode);

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

    if (parser::IsHandleAssignmentTarget(target))
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

void CheckEngineDialectRules(const NodeIndex& nodeIndex, DiagnosticContext& ctx)
{
    CheckDialectForeach(nodeIndex, ctx);
    CheckDialectEmptyListElements(nodeIndex, ctx);
    CheckDialectCharacterLiterals(nodeIndex, ctx);
    CheckDialectIntegerDivision(nodeIndex, ctx);
    CheckDialectNamedArguments(nodeIndex, ctx);
    CheckDialectValueAssignForRef(nodeIndex, ctx);
    CheckDialectMultilineStrings(nodeIndex, ctx);
}

} // namespace angel_lsp::analysis::rules
