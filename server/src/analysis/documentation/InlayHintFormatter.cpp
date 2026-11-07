#include "analysis/documentation/InlayHintFormatter.h"

#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/InlayTypeDeduction.h"
#include "analysis/types/TypeSanitization.h"
#include "i18n/i18n.h"

#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

std::string FormatParameterTooltip(const ParameterInformation& param, std::string_view label)
{
    const auto sig = FormatParameter(param);
    return fmt::format("```angelscript\n(parameter) {}\n```", !sig.empty() ? std::string_view(sig) : label);
}

std::string FormatParameterLabel(std::string_view label, std::string_view prefix, size_t maxLength)
{
    if (maxLength > 0 && label.size() > maxLength)
    {
        return fmt::format("{}{}...:", prefix, label.substr(0, maxLength));
    }
    return fmt::format("{}{}:", prefix, label);
}

std::string FormatOmittedDefaultLabel(const ParameterInformation& param, config::OmittedDefaultArgumentsMode mode,
                                      size_t maxLength)
{
    const auto text = (mode == config::OmittedDefaultArgumentsMode::Declaration)
                          ? fmt::format("{} {} = {}", param.typeName, param.name, param.defaultValue)
                          : fmt::format("{}: {}", param.name, param.defaultValue);
    if (maxLength > 0 && text.size() > maxLength)
    {
        return fmt::format("{}...", text.substr(0, maxLength));
    }
    return text;
}

std::string FormatOmittedDefaultArg(const ParameterInformation& param, config::OmittedDefaultArgumentsMode mode,
                                    size_t maxLength, bool prefixComma, bool suffixComma)
{
    const std::string base = FormatOmittedDefaultLabel(param, mode, maxLength);
    if (prefixComma && suffixComma)
    {
        return fmt::format(", {}, ", base);
    }
    if (prefixComma)
    {
        return fmt::format(", {}", base);
    }
    if (suffixComma)
    {
        return fmt::format("{}, ", base);
    }
    return base;
}

std::string FormatGoToDefinitionLink(std::string_view uri, uint32_t line1Based, uint32_t col1Based,
                                     const angel_lsp::i18n::I18n* i18n)
{
    const auto label = angel_lsp::i18n::FormatMessage(i18n, "inlay-go-to-definition", "Go to Definition");
    return fmt::format("[{}]({}#L{},{})", label, uri, line1Based, col1Based);
}

std::string FormatDefaultParamTooltip(const ParameterInformation& param, std::string_view definitionLink,
                                      const angel_lsp::i18n::I18n* i18n)
{
    const auto header = angel_lsp::i18n::FormatMessage(i18n, "inlay-default-parameter", "Default parameter:");
    if (!definitionLink.empty())
    {
        return fmt::format("{}\n```angelscript\n{} {} = {}\n```\n\n{}", header, param.typeName, param.name,
                           param.defaultValue, definitionLink);
    }
    return fmt::format("{}\n```angelscript\n{} {} = {}\n```", header, param.typeName, param.name, param.defaultValue);
}

std::optional<AutoVariableHintText> FormatAutoVariableHint(const IndexedAutoDeclarator& autoVar,
                                                           const ExpressionTypeContext& exprCtx,
                                                           const angel_lsp::i18n::I18n* i18n)
{
    const auto deduced = DeduceExpressionTypeAt(autoVar.initExprStartByte, autoVar.initExprEndByte, exprCtx);
    if (deduced.empty() || deduced == "auto" || deduced == "null" || deduced == "void")
    {
        return std::nullopt;
    }
    const bool appendHandle = autoVar.isHandle && !IsHandleTypeView(deduced);
    const auto fullType = appendHandle ? fmt::format("{}@", deduced) : std::string(deduced);
    AutoVariableHintText result;
    result.label = fmt::format(": {}", fullType);
    result.tooltip = angel_lsp::i18n::FormatMessage(i18n, "inlay-deduced-type", "Deduced type: {}", fullType);
    return result;
}

std::string FormatLambdaReturnLabel(const IndexedLambdaSite& lambda, const ExpressionTypeContext& exprCtx)
{
    const auto deduced = DeduceLambdaReturnTypeAt(lambda, exprCtx);
    if (!deduced.empty() && deduced != "void" && CleanBaseTypeView(deduced) != "auto")
    {
        return fmt::format(": {}", deduced);
    }
    return {};
}

std::string FormatEnumMemberLabel(int64_t implicitValue)
{
    return fmt::format(" = {}", implicitValue);
}

} // namespace angel_lsp::analysis
