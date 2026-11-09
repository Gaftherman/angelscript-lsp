#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Tells whether the visible definition of a name is the one declared at the given position.
 * @param[in] scope Lexical scope to resolve the name in; nullptr never matches.
 * @param[in] name Variable name to resolve.
 * @param[in] nameStart Span of the declarator name; its start is compared with the definition position.
 * @return True when the resolved local definition starts exactly at the declarator name.
 */
static bool IsMatchingLocalDefinition(const Scope* scope, std::string_view name, const SourceRange& nameStart)
{
    if (scope == nullptr)
    {
        return false;
    }
    const LocalDefinition* def = ResolveInScope(scope, name);
    return def != nullptr && def->name == name && def->startLine == nameStart.startLine &&
           def->startCharacter == nameStart.startCharacter;
}

/**
 * @brief Detects an `auto` initializer that reads the variable it declares.
 * @param[in] declarator Indexed declarator with a present initializer.
 * @param[in] scope Lexical scope the declaration lives in.
 * @param[in] request Analysis request carrying the node index and source text.
 * @return True when an unscoped identifier in the initializer resolves to this declarator.
 */
static bool IsCyclicAutoDependency(const AutoDeclaratorFact& declarator, const Scope* scope,
                                   const TypeConversionCheckRequest& request)
{
    const std::string_view varName = declarator.name;
    if (varName.empty())
    {
        return false;
    }

    const uint32_t end = declarator.value.bytes.endByte;
    if (end > request.sourceCode.size())
    {
        return false;
    }
    for (const auto& candidate : request.nodeIndex.InitializerIdentifiers(declarator.value.bytes.startByte, end))
    {
        const auto& [name, scoped] = candidate;
        if (name.endByte > end || name.endByte - name.startByte != varName.size() ||
            request.sourceCode.substr(name.startByte, varName.size()) != varName)
        {
            continue;
        }
        const auto scopedText = request.sourceCode.substr(scoped.startByte, scoped.endByte - scoped.startByte);
        if (!utils::ContainsScopeSeparator(scopedText))
        {
            return !scope || IsMatchingLocalDefinition(scope, varName, declarator.nameRange);
        }
    }
    return false;
}

/**
 * @brief Validates one `auto` declarator and writes the inferred type back into its local definition.
 * @param[in] declarator Indexed declarator of an `auto` declaration.
 * @param[in] request Analysis request carrying the scope tree and source text.
 * @param[in,out] ctx Diagnostic context.
 */
static void ProcessAutoDeclarator(const AutoDeclaratorFact& declarator, const TypeConversionCheckRequest& request,
                                  DiagnosticContext& ctx)
{
    const ConversionExpressionFact& value = declarator.value;
    if (!value.Present())
    {
        ctx.EmitAtRange(declarator.declaratorRange, "as-err-auto-requires-initializer");
        return;
    }

    const Scope* scope = ResolveNodeScope(declarator.declaratorRange, request);
    if (IsCyclicAutoDependency(declarator, scope, request))
    {
        ctx.EmitAtRange(value.range, "as-err-cyclic-auto-dependency", declarator.name);
        return;
    }

    if (const auto dataTypeName = IsBareDataType(value.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(value, ctx, *dataTypeName);
        return;
    }

    const auto rhsType = ResolveExpressionType(value.bytes, ExpressionTypeContext(scope, ctx));

    if (rhsType == "void")
    {
        ctx.EmitAtRange(value.range, "as-err-cannot-infer-void");
    }
    else if (rhsType == "null")
    {
        ctx.EmitAtRange(value.range, "as-err-cannot-infer-null");
    }
    else if (!rhsType.empty() && request.mutableScopeRoot && scope)
    {
        const LocalDefinition* def = ResolveInScope(scope, declarator.name);
        if (def && CleanBaseType(def->typeName) == "auto")
        {
            auto finalType = rhsType;
            if ((def->isHandleType || IsHandleTypeView(def->typeName)) && !IsHandleTypeView(finalType))
            {
                finalType += '@';
            }
            const_cast<LocalDefinition*>(def)->typeName = finalType;
        }
    }
}

void ProcessAutoDeclarationSite(const AutoDeclarationSiteFact& site, const TypeConversionCheckRequest& request,
                                DiagnosticContext& ctx)
{
    for (uint32_t i = 0; i < site.declaratorCount; ++i)
    {
        ProcessAutoDeclarator(request.conversionSites->autoDeclarators[site.declaratorBegin + i], request, ctx);
    }
}

} // namespace angel_lsp::analysis
