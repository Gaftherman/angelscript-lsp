#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/rules/VariableRules.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
static SourceRange GetDefinitionTypeRange(const LocalDefinition& def)
{
    const bool hasExplicitType = (def.typeEndCharacter > def.typeStartCharacter || def.typeEndLine > def.typeStartLine);
    return SourceRange{
        hasExplicitType ? def.typeStartLine : def.startLine,
        hasExplicitType ? def.typeStartCharacter : def.startCharacter,
        hasExplicitType ? def.typeEndLine : def.endLine,
        hasExplicitType ? def.typeEndCharacter : def.endCharacter,
    };
}

static void ValidateTemplateArguments(const LocalDefinition& def, const TemplateTypeInfo& tmplInfo,
                                      std::string_view enclosingNs, DiagnosticContext& ctx)
{
    const SourceRange range = GetDefinitionTypeRange(def);
    if (!IsKnownType(tmplInfo.containerName, ctx, enclosingNs))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", tmplInfo.containerName, DiagnosticSeverity::Error);
    }
    for (size_t i = 0; i < tmplInfo.templateArgs.size(); ++i)
    {
        const std::string_view cleanArg = CleanBaseTypeView(tmplInfo.templateArgs[i]);
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

static bool CheckMissingFuncdefHint(std::string_view base, const SourceRange& range, DiagnosticContext& ctx)
{
    if (ctx.request.diagnostics && ctx.request.diagnostics->reportMissingFuncdef && base != "auto" &&
        NamesAFunctionNotAType(base, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(range, "as-hint-funcdef-missing", base, DiagnosticSeverity::Hint);
        return true;
    }
    return false;
}

static bool CheckIllegalHandleOnPrimitive(const LocalDefinition& def, std::string_view base, const SourceRange& range,
                                          DiagnosticContext& ctx)
{
    if (def.isHandleType && def.typeKind != TypeKind::Array && base != "auto" && IsPrimitiveTypeName(base))
    {
        ctx.EmitAtRange(range, "as-err-handle-on-primitive", base, DiagnosticSeverity::Error);
        return true;
    }
    return false;
}

/**
 * @brief Resolves the enclosing namespace for a local definition using the scope tree.
 * @param[in] def Local definition to resolve the enclosing namespace for.
 * @param[in] ctx Diagnostic context containing the scope tree root.
 * @return Enclosing namespace name, or an empty view if not inside a namespace or scope is missing.
 */
static std::string_view ResolveEnclosingNamespaceForDefinition(const LocalDefinition& def, const DiagnosticContext& ctx)
{
    if (!ctx.request.scopeRoot)
    {
        return {};
    }
    const Scope* scope = FindInnermostScope(ctx.request.scopeRoot.get(), def.startLine, def.startCharacter);
    if (!scope)
    {
        return {};
    }
    return scope->GetEnclosingNamespace();
}

static void ValidateVariableType(const LocalDefinition& def, DiagnosticContext& ctx)
{
    const SourceRange range = GetDefinitionTypeRange(def);
    const std::string_view base = CleanBaseTypeView(def.typeName);

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

    const std::string_view enclosingNs = ResolveEnclosingNamespaceForDefinition(def, ctx);
    const TemplateTypeInfo tmplInfo = ParseTemplateType(def.typeName);
    if (!tmplInfo.templateArgs.empty())
    {
        ValidateTemplateArguments(def, tmplInfo, enclosingNs, ctx);
    }
    else if (!base.empty() && base != "auto" && !IsReservedKeyword(base) && !IsKnownType(base, ctx, enclosingNs))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", base, DiagnosticSeverity::Error);
    }

    CheckEngineLocalTypeSuggestions(def, ctx);
}

void CheckLocalNames(const Scope* scope, DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string_view> declaredHere;

    if (scope->parent && scope->parent->isFunctionScope)
    {
        for (const auto& param : scope->parent->definitions)
        {
            if (param.kind == LocalDefinitionKind::Parameter)
            {
                declaredHere.insert(param.name);
            }
        }
    }

    for (const auto& def : scope->definitions)
    {
        if (def.kind != LocalDefinitionKind::Variable && def.kind != LocalDefinitionKind::Parameter)
        {
            continue;
        }

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

void CheckLocalTypes(const Scope* scope, DiagnosticContext& ctx)
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

void CheckLocalVariableDeclarations(const Scope* rootScope, DiagnosticContext& ctx)
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

} // namespace angel_lsp::analysis::rules
