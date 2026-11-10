#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static void CheckPrimitiveDirectInit(SourceRange argumentListRange, const std::vector<std::string>& argTypes,
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

void CheckVariableDirectInitialization(const DirectInitSiteFact& init, const AstCheckContext& request,
                                       const CallSiteFacts& facts, DiagnosticContext& ctx)
{
    if (HasOverlappingError(init.bytes.startByte, init.bytes.endByte, request.nodeIndex))
    {
        return;
    }

    const std::string declaredType = CleanExpressionType(std::string(init.declaredType));
    if (declaredType.empty() || declaredType == "auto" || init.declaredType.find('[') != std::string_view::npos)
    {
        return;
    }

    const TemplateTypeInfo tmplInfo = ParseTemplateType(declaredType);
    std::string baseName = tmplInfo.containerName.empty() ? declaredType : tmplInfo.containerName;
    baseName = CleanBaseType(baseName);

    const VarInitContext vctx{declaredType, baseName, tmplInfo, request, ctx};
    const Scope* scope = request.scopeRoot
                             ? FindInnermostScope(request.scopeRoot, init.range.startLine, init.range.startCharacter)
                             : nullptr;

    const auto argSlice = std::span(facts.arguments).subspan(init.argumentBegin, init.argumentCount);
    std::vector<std::string> argTypes;
    argTypes.reserve(argSlice.size());
    for (const auto& arg : argSlice)
    {
        argTypes.push_back(ResolveExpressionType(arg.bytes, ExpressionTypeContext(scope, ctx)));
    }
    if (std::any_of(argTypes.begin(), argTypes.end(),
                    [](const std::string& t) { return ExpressionTypeCache::IsPoisonType(t); }))
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
    CheckConstructorOverload(init.argumentListRange, candidates, argTypes, vctx);
}

} // namespace angel_lsp::analysis
