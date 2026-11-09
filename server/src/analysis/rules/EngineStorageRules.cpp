#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/types/TypeSanitization.h"
#include "i18n/i18n.h"
#include "parser/Primitives.h"
#include "spdlog/fmt/fmt.h"

#include <algorithm>
#include <regex>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{

/**
 * @brief Converts an indexed source span to a diagnostic source range.
 */
static constexpr SourceRange ToRange(const IndexedSourceSpan& s) noexcept
{
    return {s.startRow, s.startColumn, s.endRow, s.endColumn};
}

/**
 * @brief Maps an engine rule severity descriptor to a diagnostic severity enum.
 */
static DiagnosticSeverity ResolveSeverity(std::string_view sev) noexcept
{
    return (sev == "error")
               ? DiagnosticSeverity::Error
               : ((sev == "hint") ? DiagnosticSeverity::Hint
                                  : ((sev == "info" || sev == "information") ? DiagnosticSeverity::Information
                                                                             : DiagnosticSeverity::Warning));
}

/**
 * @brief Checks whether a type name matches the constraints of a storage rule.
 */
static bool MatchesTypeName(std::string_view baseType, const config::StorageRule& rule, const SymbolTable& table)
{
    if (std::find(rule.types.begin(), rule.types.end(), baseType) != rule.types.end() ||
        (!rule.inheritsFrom.empty() && baseType == rule.inheritsFrom))
    {
        return true;
    }
    if (!rule.inheritsFrom.empty())
    {
        const auto hier = GetInheritedTypeHierarchy(baseType, table);
        if (std::find(hier.begin(), hier.end(), rule.inheritsFrom) != hier.end())
        {
            return true;
        }
    }
    try
    {
        return !rule.typeRegex.empty() &&
               std::regex_match(baseType.begin(), baseType.end(), std::regex(rule.typeRegex));
    }
    catch (...)
    {
        return false;
    }
}

/**
 * @brief Selects the element type of an array-like template variable, preferring the spelled template argument
 * (which keeps handle decorations such as '@') over the collector's sanitized argument list.
 * @param[in] sig Variable signature whose template name and argument types are inspected.
 * @param[in] parsed Template decomposition of sig.typeName; the returned view may borrow from it.
 * @param[in] request Analysis request that knows which templates behave like arrays.
 * @return View of the first element type, or an empty view when the variable is not an array-like template.
 */
static std::string_view ExtractArrayElement(const VariableSignature& sig, const TemplateTypeInfo& parsed,
                                            const SemanticAnalysisRequest& request)
{
    if (sig.templateName.empty() || !request.IsArrayLikeTemplate(sig.templateName))
    {
        return {};
    }
    if (!parsed.templateArgs.empty())
    {
        return parsed.templateArgs.front();
    }
    if (!sig.templateArgumentTypes.empty())
    {
        return sig.templateArgumentTypes.front();
    }
    return {};
}

/**
 * @brief Validates a variable symbol against an engine storage restriction rule.
 */
static void EvaluateStorageRule(const Symbol& sym, const VariableSignature& sig, const config::StorageRule& rule,
                                const DiagnosticContext& ctx)
{
    const bool isMember = HasContainerOfType(sym, ctx.request.symbolTable, SymbolType::Class);
    const bool isGlobal = !isMember && sym.containerName.empty();
    const TemplateTypeInfo parsed = ParseTemplateType(sig.typeName);
    const std::string_view elem = ExtractArrayElement(sig, parsed, ctx.request);
    const bool isArray = !elem.empty();
    const std::string_view target = isArray ? elem : std::string_view(sig.baseTypeName);
    const bool blocked = (rule.disallowInMembers && isMember) || (rule.disallowInGlobals && isGlobal) ||
                         (rule.disallowInArrays && isArray);

    if (blocked && (!rule.handleOnly || (isArray ? IsHandleTypeView(target) : sig.modifiers.isHandle)) &&
        MatchesTypeName(CleanBaseTypeView(target, ctx.request.GetArrayTypeName()), rule, ctx.request.symbolTable))
    {
        const auto msg = rule.message.empty()
                             ? i18n::FormatMessage(ctx.request.i18n, "engine-rule-storage-default",
                                                   "Variable '{}' violates storage rule '{}'. Consider using '{}'.",
                                                   sym.name, rule.id, rule.suggestReplacement)
                             : fmt::format(fmt::runtime(rule.message), sym.name, rule.suggestReplacement);
        ctx.Emit(sym, diagnostics::codes::EngineStorageRule, msg, ResolveSeverity(rule.severity));
    }
}

void CheckEngineStorageRules(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (ctx.request.engineRules)
    {
        for (const auto& rule : ctx.request.engineRules->storageRules)
        {
            EvaluateStorageRule(sym, sig, rule, ctx);
        }
    }
}

/**
 * @brief Formats an informative message recommending replacement of a type.
 */
static auto FormatTypeSuggestionMessage(std::string_view varName, const config::TypeSuggestionRule& s,
                                        const DiagnosticContext& ctx)
{
    return s.message.empty()
               ? i18n::FormatMessage(ctx.request.i18n, "engine-rule-type-suggestion-default",
                                     "Variable '{}' uses '{}'. Consider using '{}'.", varName, s.fromType, s.toType)
               : s.message;
}

void CheckEngineTypeSuggestions(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (ctx.request.engineRules)
    {
        for (const auto& s : ctx.request.engineRules->typeSuggestions)
        {
            if (sig.baseTypeName == s.fromType)
            {
                ctx.Emit(sym, diagnostics::codes::EngineTypeSuggestion, FormatTypeSuggestionMessage(sym.name, s, ctx),
                         ResolveSeverity(s.severity));
            }
        }
    }
}

void CheckEngineLocalTypeSuggestions(const LocalDefinition& def, DiagnosticContext& ctx)
{
    if (ctx.request.engineRules)
    {
        const std::string_view base = CleanBaseTypeView(def.typeName, ctx.request.GetArrayTypeName());
        for (const auto& s : ctx.request.engineRules->typeSuggestions)
        {
            if (base == s.fromType)
            {
                ctx.EmitAtRange({def.typeStartLine, def.typeStartCharacter, def.typeEndLine, def.typeEndCharacter},
                                diagnostics::codes::EngineTypeSuggestion, FormatTypeSuggestionMessage(def.name, s, ctx),
                                ResolveSeverity(s.severity));
            }
        }
    }
}

/**
 * @brief Checks if a scheduler rule applies to the callee receiver of a call site.
 */
static bool RuleMatchesReceiver(const SchedulerCallRequest& req, const config::SchedulerRule& rule)
{
    if (rule.receiverTypes.empty())
    {
        return true;
    }
    const auto hasType = [&](std::string_view type)
    {
        return !type.empty() &&
               std::find(rule.receiverTypes.begin(), rule.receiverTypes.end(), type) != rule.receiverTypes.end();
    };
    return hasType(req.site.receiverText) ||
           std::any_of(req.candidates.begin(), req.candidates.end(),
                       [&](const Symbol* c) { return c && hasType(c->containerName); });
}

/**
 * @brief Validates an individual scheduler call argument for safety rules and enum construction.
 */
static void CheckSchedulerArg(const SchedulerCallRequest& req, size_t index, const config::SchedulerRule& rule,
                              const DiagnosticContext& ctx)
{
    const std::string_view argType = req.argTypes[index];
    const auto& argSpan = req.site.args[index];
    if (argType.empty() || argSpan.bytes.Empty())
    {
        return;
    }
    const SourceRange range = ToRange(argSpan);
    if ((!rule.disallowArgInheritsFrom.empty() || !rule.disallowArgRegex.empty()) &&
        (!rule.disallowArgHandleOnly || IsHandleTypeView(argType)))
    {
        const config::StorageRule checkRule{.inheritsFrom = rule.disallowArgInheritsFrom,
                                            .typeRegex = rule.disallowArgRegex,
                                            .handleOnly = rule.disallowArgHandleOnly,
                                            .suggestReplacement = rule.argSuggestReplacement};
        if (MatchesTypeName(CleanBaseTypeView(argType, ctx.request.GetArrayTypeName()), checkRule,
                            ctx.request.symbolTable))
        {
            const auto msg =
                rule.message.empty()
                    ? i18n::FormatMessage(ctx.request.i18n, "engine-rule-scheduler-safety-default",
                                          "Passing raw handle '{}' to '{}' is unsafe.", argType, req.methodName)
                    : rule.message;
            ctx.EmitAtRange(range, diagnostics::codes::EngineSchedulerSafety, msg, ResolveSeverity(rule.severity));
        }
    }
    if (rule.requireExplicitEnumConstruct &&
        parser::primitives::IsInteger(CleanBaseTypeView(argType, ctx.request.GetArrayTypeName())))
    {
        for (const auto* cand : req.candidates)
        {
            if (cand && std::holds_alternative<FunctionSignature>(cand->signature))
            {
                const auto& sig = std::get<FunctionSignature>(cand->signature);
                if (index < sig.parameters.size() &&
                    HasSymbolOfType(ctx.request.symbolTable, sig.parameters[index].baseTypeName, SymbolType::Enum))
                {
                    const std::string_view enumType = sig.parameters[index].baseTypeName;
                    const std::string_view argText = ctx.request.sourceCode.substr(
                        argSpan.bytes.startByte, argSpan.bytes.endByte - argSpan.bytes.startByte);
                    const auto msg = i18n::FormatMessage(
                        ctx.request.i18n, "engine-rule-scheduler-enum-discriminant",
                        "Passing discriminant to enum type '{}' in scheduled call; explicitly construct or typecast "
                        "first (e.g. {}({})).",
                        enumType, enumType, argText);
                    ctx.EmitAtRange(range, diagnostics::codes::EngineScheduledEnumDiscriminant, msg,
                                    DiagnosticSeverity::Warning);
                    break;
                }
            }
        }
    }
}

void CheckEngineSchedulerCall(const SchedulerCallRequest& req, const DiagnosticContext& ctx)
{
    if (!ctx.request.engineRules)
    {
        return;
    }
    for (const auto& rule : ctx.request.engineRules->schedulerRules)
    {
        if (std::find(rule.methodNames.begin(), rule.methodNames.end(), req.methodName) == rule.methodNames.end() ||
            !RuleMatchesReceiver(req, rule))
        {
            continue;
        }
        if (rule.disallowMethod)
        {
            const std::string_view msgTmpl = rule.message;
            const auto msg =
                msgTmpl.empty()
                    ? i18n::FormatMessage(ctx.request.i18n, diagnostics::codes::EngineCallSafetyDisallowed,
                                          "Calling '{0}' is disallowed for safety reasons. Use '{1}' instead.",
                                          req.methodName, rule.suggestReplacement)
                    : (msgTmpl.find("{}") != std::string_view::npos
                           ? fmt::format(fmt::runtime(msgTmpl), req.methodName, rule.suggestReplacement)
                           : rule.message);
            ctx.EmitAtRange(ToRange(req.site.call), diagnostics::codes::EngineCallSafety, msg,
                            ResolveSeverity(rule.severity));
            return;
        }
        const size_t count = std::min(req.site.args.size(), req.argTypes.size());
        for (size_t i = 0; i < count; ++i)
        {
            CheckSchedulerArg(req, i, rule, ctx);
        }
    }
}

} // namespace angel_lsp::analysis::rules
