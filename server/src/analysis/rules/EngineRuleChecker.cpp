#include "analysis/rules/EngineRuleChecker.h"

#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/TypeSanitization.h"
#include "i18n/i18n.h"
#include "parser/Primitives.h"
#include "spdlog/fmt/fmt.h"
#include "utils/Utils.h"

#include <algorithm>
#include <regex>

namespace angel_lsp::analysis::rules
{

namespace
{

DiagnosticSeverity ResolveSeverity(std::string_view sev)
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

bool MatchesTypeName(const std::string& baseType, const config::StorageRule& rule, const SymbolTable& table)
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

bool IsClassMember(const Symbol& sym, const SymbolTable& table)
{
    if (sym.containerName.empty())
    {
        return false;
    }
    const auto container = table.FindSymbolsPtr(sym.containerName);
    return container && std::any_of(container->begin(), container->end(),
                                    [](const Symbol& owner) { return owner.type == SymbolType::Class; });
}

bool IsEnumType(const std::string& typeName, const SymbolTable& table)
{
    const auto syms = table.FindSymbolsPtr(typeName);
    return syms && std::any_of(syms->begin(), syms->end(), [](const Symbol& s) { return s.type == SymbolType::Enum; });
}

std::string ExtractArrayElement(const VariableSignature& sig, const SemanticAnalysisRequest& request)
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

void ReportStorageViolation(const Symbol& sym, const config::StorageRule& rule, const DiagnosticContext& ctx)
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

void EvaluateStorageRule(const Symbol& sym, const VariableSignature& sig, const config::StorageRule& rule,
                         const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const bool isMember = IsClassMember(sym, table);
    const bool isGlobal = !isMember && sym.containerName.empty();
    const std::string arrayElem = ExtractArrayElement(sig, ctx.request);
    const bool isArray = !arrayElem.empty();

    const std::string& targetType = isArray ? arrayElem : sig.baseTypeName;
    const std::string cleanTarget = CleanBaseType(targetType, ctx.request.GetArrayTypeName());

    if (!MatchesTypeName(cleanTarget, rule, table))
    {
        return;
    }

    const bool hasHandle = isArray ? targetType.find('@') != std::string::npos : sig.modifiers.isHandle;
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

void CheckSchedulerArgSafety(const SchedulerCallRequest& req, size_t index, const config::SchedulerRule& rule,
                             const DiagnosticContext& ctx)
{
    if (rule.disallowArgInheritsFrom.empty() && rule.disallowArgRegex.empty())
    {
        return;
    }

    const std::string& argType = req.argTypes[index];
    const TSNode argNode = req.argNodes[index];
    const bool hasHandle = argType.find('@') != std::string::npos;
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

void CheckSchedulerEnumDiscriminant(const SchedulerCallRequest& req, size_t index, const config::SchedulerRule& rule,
                                    const DiagnosticContext& ctx)
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
            if (IsEnumType(param.baseTypeName, ctx.request.symbolTable))
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

void CheckSingleSchedulerArg(const SchedulerCallRequest& req, size_t index, const config::SchedulerRule& rule,
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

std::string FormatTypeSuggestionMessage(std::string_view varName, const config::TypeSuggestionRule& suggestion,
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

} // namespace

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

} // namespace angel_lsp::analysis::rules
