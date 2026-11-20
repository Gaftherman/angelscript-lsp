#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static void CheckPrimitiveDirectInit(SourceRange argumentListRange, std::span<const std::string> argTypes,
                                     const VarInitContext& vctx)
{
    if (argTypes.size() != 1)
    {
        vctx.ctx.EmitAtRange(argumentListRange, "as-err-no-matching-constructor",
                             FormatSignatureAttempt(vctx.declaredType, argTypes));
    }
    else
    {
        ParameterInformation dummyParam;
        dummyParam.name = vctx.baseName;
        dummyParam.typeName = vctx.baseName;
        dummyParam.baseTypeName = vctx.baseName;
        const auto conv = EvaluateArgumentConversion(argTypes[0], dummyParam, vctx.ctx.request.symbolTable);
        if (!conv.IsViable())
        {
            vctx.ctx.EmitAtRange(argumentListRange, "as-err-no-implicit-conversion", argTypes[0], vctx.baseName);
        }
    }
}

/**
 * @brief Publishes the constructor a direct initialization selected, keyed on the declarator's own identifier.
 *
 * Declarators that share a written type (`A x(1), y(2);`) or put the name on another line each get their own
 * binding, because the identifier, not the type, is unique to the declarator.
 * @param[in] constructor Winning constructor.
 * @param[in] init Direct initialization site with the indexed span of the variable name.
 * @param[in,out] ctx Diagnostic context owning the revision's call bindings.
 */
static void PublishConstructorUse(const Symbol& constructor, const DirectInitSiteFact& init, DiagnosticContext& ctx)
{
    if (!init.nameBytes.Empty())
    {
        PublishCallBinding(ctx, init.nameBytes.startByte, init.nameRange, constructor);
    }
}

void CheckVariableDirectInitialization(const DirectInitSiteFact& init, const AstCheckContext& request,
                                       const CallSiteFacts& facts, DiagnosticContext& ctx)
{
    if (HasOverlappingError(init.bytes.startByte, init.bytes.endByte, request.nodeIndex))
    {
        return;
    }

    std::string_view rawDeclaredType = CallSiteFacts::SliceText(request.sourceCode, init.declaredTypeBytes);
    TrimTypeWhitespace(rawDeclaredType);
    const std::string declaredType = CleanExpressionType(rawDeclaredType);
    if (declaredType.empty() || parser::primitives::IsAutoType(declaredType) ||
        rawDeclaredType.find('[') != std::string_view::npos)
    {
        return;
    }

    const TemplateTypeInfo tmplInfo = ParseTemplateType(declaredType);
    const std::string_view base =
        tmplInfo.containerName.empty() ? std::string_view(declaredType) : std::string_view(tmplInfo.containerName);
    const std::string_view baseName = CleanBaseTypeView(base);

    const VarInitContext vctx{declaredType, baseName, tmplInfo, request, ctx};
    const Scope* scope = request.scopeRoot
                             ? FindInnermostScope(request.scopeRoot, init.range.startLine, init.range.startCharacter)
                             : nullptr;

    const auto argSlice = facts.ArgumentsOf(init);
    std::vector<std::string> argTypes;
    argTypes.reserve(argSlice.size());
    for (const auto& arg : argSlice)
    {
        argTypes.push_back(ResolveExpressionType(arg.bytes, ExpressionTypeContext(scope, ctx)));
    }
    if (AnyArgumentHasSyntaxError(ctx.request.nodeIndex, argSlice))
    {
        return;
    }

    if (IsCorePrimitive(vctx.baseName))
    {
        CheckPrimitiveDirectInit(init.argumentListRange, argTypes, vctx);
        return;
    }

    const std::vector<Symbol> rawConstructors = LookupRawConstructors(vctx.baseName, ctx.request.symbolTable);
    if (rawConstructors.empty())
    {
        if (argTypes.empty())
        {
            return;
        }
        if (IsClassDeclarationVisible(vctx.baseName, ctx.request.symbolTable, ctx.request.predefinedFileExtension))
        {
            ctx.EmitAtRange(init.argumentListRange, "as-err-no-matching-constructor",
                            FormatSignatureAttempt(vctx.declaredType, argTypes));
        }
        return;
    }

    const std::vector<Symbol> candidates =
        SpecializeConstructors(rawConstructors, vctx.tmplInfo, vctx.baseName, ctx.request.symbolTable);
    if (const Symbol* winner = CheckConstructorOverload(init.argumentListRange, candidates, argTypes, vctx))
    {
        PublishConstructorUse(*winner, init, ctx);
    }
}

} // namespace angel_lsp::analysis
