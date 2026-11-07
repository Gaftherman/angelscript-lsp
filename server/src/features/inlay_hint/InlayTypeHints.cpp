#include "analysis/documentation/InlayHintFormatter.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/InlayCalleeResolution.h"
#include "features/common/FeatureSharedUtils.h"
#include "features/inlay_hint/InlayHintInternal.h"

#include <optional>
#include <span>
#include <vector>

namespace angel_lsp::features
{

void ProcessAutoVariable(const analysis::IndexedAutoDeclarator& autoVar, const InlayHintRequest& request,
                         std::vector<lsp::InlayHint>& hints)
{
    if (request.config && !request.config->features.inlayHintsVariableTypes)
    {
        return;
    }
    if (autoVar.initExprStartByte == 0 && autoVar.initExprEndByte == 0)
    {
        return;
    }
    lsp::Position hintPos{autoVar.nameEndLine, autoVar.nameEndCharacter};
    if (!common::IsPositionInRange(hintPos, request.range))
    {
        return;
    }
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const auto* scope =
        rootScope ? analysis::FindInnermostScope(rootScope.get(), autoVar.nameEndLine, autoVar.nameEndCharacter)
                  : nullptr;
    const auto exprCtx = MakeExpressionTypeContext(request, scope);
    const std::optional<analysis::AutoVariableHintText> hintText =
        analysis::FormatAutoVariableHint(autoVar, exprCtx, request.i18n);
    if (!hintText.has_value())
    {
        return;
    }
    lsp::InlayHint hint;
    hint.label = hintText->label;
    hint.position = hintPos;
    hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Type);
    hint.paddingLeft = true;
    hint.paddingRight = false;
    if (!request.config || request.config->features.inlayHintsEnableTooltip)
    {
        hint.tooltip = lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), hintText->tooltip};
    }
    hints.push_back(std::move(hint));
}

void ProcessConstructorInit(const analysis::IndexedConstructorInitDeclarator& ctorInit,
                            std::span<const analysis::IndexedCallArgument> args, const InlayHintRequest& request,
                            std::vector<lsp::InlayHint>& hints)
{
    if (request.config && !request.config->features.inlayHintsParameters)
    {
        return;
    }
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const auto* scope = rootScope ? analysis::FindInnermostScope(rootScope.get(), ctorInit.declRange.startLine,
                                                                 ctorInit.declRange.startCharacter)
                                  : nullptr;
    const auto exprCtx = MakeExpressionTypeContext(request, scope);
    auto callee = analysis::ResolveConstructorInitCallee(ctorInit, args, exprCtx);
    if (!callee.has_value())
    {
        return;
    }
    ProcessConstructorInitParameters(*callee, args, ctorInit.emptyArgsLine, ctorInit.emptyArgsCharacter,
                                     ctorInit.hasArgumentList, request, hints);
}

void ProcessLambda(const analysis::IndexedLambdaSite& lambda, const InlayHintRequest& request,
                   std::vector<lsp::InlayHint>& hints)
{
    if (request.config && !request.config->features.inlayHintsFunctionReturnTypes)
    {
        return;
    }
    if (lambda.hasExplicitReturnType)
    {
        return;
    }
    lsp::Position pos{lambda.returnTypeHintLine, lambda.returnTypeHintCharacter};
    if (!common::IsPositionInRange(pos, request.range))
    {
        return;
    }
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const auto* scope = rootScope ? analysis::FindInnermostScope(rootScope.get(), lambda.returnTypeHintLine,
                                                                 lambda.returnTypeHintCharacter)
                                  : nullptr;
    const auto exprCtx = MakeExpressionTypeContext(request, scope);
    lsp::InlayHintLabelPart part;
    part.value = analysis::FormatLambdaReturnLabel(lambda, exprCtx);
    if (!part.value.empty())
    {
        lsp::InlayHint hint;
        hint.position = pos;
        hint.label = std::vector<lsp::InlayHintLabelPart>{std::move(part)};
        hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Type);
        hint.paddingLeft = true;
        hint.paddingRight = true;
        hints.push_back(std::move(hint));
    }
}

void ProcessEnum(std::span<const analysis::IndexedEnumValue> enumValues, const InlayHintRequest& request,
                 std::vector<lsp::InlayHint>& hints)
{
    if (request.config && !request.config->features.inlayHintsEnumMemberValues)
    {
        return;
    }
    for (const auto& val : enumValues)
    {
        if (val.hasExplicitValue || val.memberName.empty())
        {
            continue;
        }
        lsp::Position pos{val.hintLine, val.hintCharacter};
        if (common::IsPositionInRange(pos, request.range))
        {
            lsp::InlayHint hint;
            hint.position = pos;
            lsp::InlayHintLabelPart part;
            part.value = analysis::FormatEnumMemberLabel(val.implicitValue);
            hint.label = std::vector<lsp::InlayHintLabelPart>{std::move(part)};
            hint.paddingLeft = false;
            hint.paddingRight = false;
            hints.push_back(std::move(hint));
        }
    }
}

} // namespace angel_lsp::features
