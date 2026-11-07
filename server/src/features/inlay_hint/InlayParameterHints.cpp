#include "analysis/documentation/InlayHintFormatter.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/ScopeTree.h"
#include "features/common/FeatureSharedUtils.h"
#include "features/inlay_hint/InlayHintInternal.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Builds a parameter inlay hint for a single argument.
 * @param[in] ctx Parameter hint context containing call and config details.
 * @param[in] param Callee parameter information.
 * @param[in] hintPos Position for hint placement.
 * @param[in] label Formatted parameter label.
 * @return Populated InlayHint.
 */
static lsp::InlayHint BuildParameterHint(const ParamHintContext& ctx, const analysis::ParameterInformation& param,
                                         const lsp::Position& hintPos, std::string_view label)
{
    lsp::InlayHint hint;
    hint.position = hintPos;
    const bool enableTooltip = !ctx.request.config || ctx.request.config->features.inlayHintsEnableTooltip;
    const bool enableLocation = !ctx.request.config || ctx.request.config->features.inlayHintsEnableLocation;
    const bool showRefMods = !ctx.request.config || ctx.request.config->features.inlayHintsParameterReferenceModifiers;
    std::string_view prefix = "";
    if (showRefMods)
    {
        if (param.modifier == analysis::ParameterModifier::Out)
        {
            prefix = "&out ";
        }
        else if (param.modifier == analysis::ParameterModifier::InOut)
        {
            prefix = "&inout ";
        }
    }

    lsp::InlayHintLabelPart part;
    part.value = analysis::FormatParameterLabel(label, prefix, ctx.request.maxLength);

    if (enableLocation)
    {
        part.location = ResolveParameterLocation(ctx.request.uri, ctx.callee.fileUri, param, ctx.callee.selectionRange);
    }
    if (enableTooltip && !part.location.has_value())
    {
        part.tooltip = lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                          analysis::FormatParameterTooltip(param, label)};
    }
    hint.label = std::vector<lsp::InlayHintLabelPart>{std::move(part)};
    hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Parameter);
    hint.paddingRight = false;
    hint.paddingLeft = false;
    hint.tooltip = std::nullopt;
    return hint;
}

/**
 * @brief Adds parameter inlay hints for matched parameters and arguments.
 * @param[in] callee Callee resolution result.
 * @param[in] args Parsed call arguments.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated parameter hints.
 */
static void AddParameterHints(const analysis::ResolvedCallee& callee,
                              std::span<const analysis::IndexedCallArgument> args, const InlayHintRequest& request,
                              std::vector<lsp::InlayHint>& hints)
{
    if (request.config && !request.config->features.inlayHintsParameters)
    {
        return;
    }
    const auto& parameters = callee.parameters;
    const ParamHintContext ctx{callee, request};
    for (size_t i = 0; i < args.size() && i < parameters.size(); ++i)
    {
        if (request.maxParameters > 0 && i >= request.maxParameters)
        {
            break;
        }

        const auto& param = parameters[i];
        const auto& arg = args[i];

        if (!arg.name.empty() || analysis::IsVarargParameter(param))
        {
            continue;
        }

        const std::string_view label = ResolveParameterLabel(param);
        const bool suppress =
            (request.suppressWhenArgumentMatchesName ||
             (request.config && request.config->features.inlayHintsSuppressWhenArgumentMatchesName)) &&
            ArgumentMatchesParameterName(arg.text, label);
        if (label.empty() || suppress)
        {
            continue;
        }

        lsp::Position hintPos{arg.hintLine, arg.hintCharacter};
        if (common::IsPositionInRange(hintPos, request.range))
        {
            hints.push_back(BuildParameterHint(ctx, param, hintPos, label));
        }
    }
}

/**
 * @brief Binds extracted call arguments to parameter indices and collects their positions.
 * @param[in] args Sliced call argument records.
 * @param[in] parameters Function parameter signatures.
 * @param[out] boundParamIndices Set of parameter indices that have arguments passed.
 * @return Vector of CallArgPosition elements.
 */
static std::vector<CallArgPosition>
MapArgumentsToParameters(std::span<const analysis::IndexedCallArgument> args,
                         const std::vector<analysis::ParameterInformation>& parameters,
                         std::unordered_set<size_t>& boundParamIndices)
{
    std::vector<CallArgPosition> positions;
    positions.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& arg = args[i];
        CallArgPosition pos;
        pos.startLine = arg.argStartLine;
        pos.startCharacter = arg.argStartCharacter;
        pos.endLine = arg.range.endLine;
        pos.endCharacter = arg.range.endCharacter;
        if (!arg.name.empty())
        {
            for (size_t p = 0; p < parameters.size(); ++p)
            {
                if (parameters[p].name == arg.name)
                {
                    pos.boundParamIndex = static_cast<int>(p);
                    boundParamIndices.insert(p);
                    break;
                }
            }
        }
        else if (i < parameters.size())
        {
            pos.boundParamIndex = static_cast<int>(i);
            boundParamIndices.insert(i);
        }
        positions.push_back(pos);
    }
    return positions;
}

/**
 * @brief Constructs an InlayHint item for an omitted default parameter.
 * @param[in] pos Target document position.
 * @param[in] param Callee parameter information.
 * @param[in] fileUri Callee definition document URI.
 * @param[in] prefixComma True if a leading comma should precede the label.
 * @param[in] suffixComma True if a trailing comma should follow the label.
 * @param[in] request Inlay hint request context.
 * @return InlayHint object.
 */
static lsp::InlayHint MakeOmittedDefaultHint(const lsp::Position& pos, const analysis::ParameterInformation& param,
                                             std::string_view fileUri, bool prefixComma, bool suffixComma,
                                             const InlayHintRequest& request)
{
    lsp::InlayHint hint;
    hint.position = pos;
    hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Parameter);
    hint.paddingLeft = false;
    hint.paddingRight = false;

    const bool enableTooltip = !request.config || request.config->features.inlayHintsEnableTooltip;
    const bool enableLocation = !request.config || request.config->features.inlayHintsEnableLocation;

    lsp::InlayHintLabelPart part;
    part.value = analysis::FormatOmittedDefaultArg(param, request.omittedDefaultArguments, request.maxLength,
                                                   prefixComma, suffixComma);
    const bool hasValidLocation = (param.startLine != param.endLine || param.startCharacter != param.endCharacter);
    if (enableLocation && !fileUri.empty() && hasValidLocation)
    {
        lsp::Range paramRange{lsp::Position{param.startLine, param.startCharacter},
                              lsp::Position{param.endLine, param.endCharacter}};
        part.location = lsp::Location{common::ParseOrConvertUri(fileUri), paramRange};
    }
    if (enableTooltip)
    {
        if (part.location.has_value())
        {
            part.tooltip =
                lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                   analysis::FormatDefaultParamTooltip(
                                       param,
                                       analysis::FormatGoToDefinitionLink(
                                           part.location->uri.toString(), part.location->range.start.line + 1,
                                           part.location->range.start.character + 1, request.i18n),
                                       request.i18n)};
        }
        else
        {
            part.tooltip = lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown),
                                              analysis::FormatDefaultParamTooltip(param, {}, request.i18n)};
        }
    }
    hint.label = std::vector<lsp::InlayHintLabelPart>{std::move(part)};
    hint.tooltip = std::nullopt;
    return hint;
}

/**
 * @brief Emits an omitted parameter hint into an empty argument list.
 * @param[in] emptyLine Line for hint placement.
 * @param[in] emptyCol Character for hint placement.
 * @param[in] isFirstInEmptyList True if this is the first hint in the empty list.
 * @param[in] param Callee parameter information.
 * @param[in] fileUri Callee definition document URI.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint vector receiving generated hints.
 */
static void EmitEmptyListOmittedHint(uint32_t emptyLine, uint32_t emptyCol, bool isFirstInEmptyList,
                                     const analysis::ParameterInformation& param, std::string_view fileUri,
                                     const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    lsp::Position pos{emptyLine, emptyCol};
    if (common::IsPositionInRange(pos, request.range))
    {
        hints.push_back(MakeOmittedDefaultHint(pos, param, fileUri, !isFirstInEmptyList, false, request));
    }
}

/**
 * @brief Emits an omitted parameter hint into a non-empty argument list.
 * @param[in] argPositions Mapped call argument positions.
 * @param[in] paramIndex Index of the omitted parameter.
 * @param[in] param Callee parameter information.
 * @param[in] fileUri Callee definition document URI.
 * @param[in] request Top-level inlay hint request.
 * @param[in,out] hints Hint vector receiving generated hints.
 */
static void EmitArgListOmittedHint(std::span<const CallArgPosition> argPositions, size_t paramIndex,
                                   const analysis::ParameterInformation& param, std::string_view fileUri,
                                   const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    const CallArgPosition* nextArg = nullptr;
    for (const auto& ap : argPositions)
    {
        if (ap.boundParamIndex > static_cast<int>(paramIndex))
        {
            nextArg = &ap;
            break;
        }
    }

    if (nextArg)
    {
        lsp::Position pos{nextArg->startLine, nextArg->startCharacter};
        if (common::IsPositionInRange(pos, request.range))
        {
            hints.push_back(MakeOmittedDefaultHint(pos, param, fileUri, false, true, request));
        }
    }
    else
    {
        lsp::Position pos{argPositions.back().endLine, argPositions.back().endCharacter};
        if (common::IsPositionInRange(pos, request.range))
        {
            hints.push_back(MakeOmittedDefaultHint(pos, param, fileUri, true, false, request));
        }
    }
}

/**
 * @brief Adds inlay hints for omitted optional parameters that have default values.
 * @param[in] callee Callee resolution result.
 * @param[in] args Call argument slice.
 * @param[in] emptyArgsLine Line coordinate of empty argument list.
 * @param[in] emptyArgsCharacter Character coordinate of empty argument list.
 * @param[in] hasArgList True if argument list is syntactically present.
 * @param[in] request Inlay hint request context.
 * @param[in,out] hints Hint vector receiving generated inlay hints.
 */
static void AddOmittedDefaultArgumentHints(const analysis::ResolvedCallee& callee,
                                           std::span<const analysis::IndexedCallArgument> args, uint32_t emptyArgsLine,
                                           uint32_t emptyArgsCharacter, bool hasArgList,
                                           const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    const auto& parameters = callee.parameters;
    if (request.omittedDefaultArguments == config::OmittedDefaultArgumentsMode::Off || parameters.empty() ||
        !hasArgList)
    {
        return;
    }

    std::unordered_set<size_t> boundParams;
    auto argPositions = MapArgumentsToParameters(args, parameters, boundParams);
    bool isFirstInEmptyList = true;
    std::string_view targetUri = !callee.fileUri.empty() ? callee.fileUri : request.uri;

    for (size_t p = 0; p < parameters.size(); ++p)
    {
        if (boundParams.contains(p))
        {
            continue;
        }
        const auto& param = parameters[p];
        if (param.defaultValue.empty() || analysis::IsVarargParameter(param))
        {
            continue;
        }

        if (argPositions.empty())
        {
            EmitEmptyListOmittedHint(emptyArgsLine, emptyArgsCharacter, isFirstInEmptyList, param, targetUri, request,
                                     hints);
            isFirstInEmptyList = false;
        }
        else
        {
            EmitArgListOmittedHint(argPositions, p, param, targetUri, request, hints);
        }
    }
}

void ProcessCallSite(const analysis::IndexedCallSite& call, std::span<const analysis::IndexedCallArgument> args,
                     const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const auto* scope = rootScope ? analysis::FindInnermostScope(rootScope.get(), call.callRange.startLine,
                                                                 call.callRange.startCharacter)
                                  : nullptr;
    const auto exprCtx = MakeExpressionTypeContext(request, scope);
    const auto callee = analysis::ResolveCallSiteCallee(call, args, exprCtx);
    if (!callee)
    {
        return;
    }

    AddParameterHints(*callee, args, request, hints);
    AddOmittedDefaultArgumentHints(*callee, args, call.emptyArgsLine, call.emptyArgsCharacter, call.hasArgumentList,
                                   request, hints);
}

void ProcessConstructorInitParameters(const analysis::ResolvedCallee& callee,
                                      std::span<const analysis::IndexedCallArgument> args, uint32_t emptyArgsLine,
                                      uint32_t emptyArgsCharacter, bool hasArgumentList,
                                      const InlayHintRequest& request, std::vector<lsp::InlayHint>& hints)
{
    AddParameterHints(callee, args, request, hints);
    AddOmittedDefaultArgumentHints(callee, args, emptyArgsLine, emptyArgsCharacter, hasArgumentList, request, hints);
}

} // namespace angel_lsp::features
