#include "features/inlay_hint/InlayHintInternal.h"

#include "features/common/FeatureSharedUtils.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

namespace angel_lsp::features
{

std::string_view ResolveParameterLabel(const analysis::ParameterInformation& param) noexcept
{
    if (!param.name.empty() && param.name != "...")
    {
        return param.name;
    }
    return (!param.typeName.empty() && param.typeName != "...") ? std::string_view(param.typeName) : std::string_view{};
}

bool ArgumentMatchesParameterName(std::string_view argText, std::string_view paramName) noexcept
{
    if (paramName.empty())
    {
        return false;
    }
    if (argText.starts_with('@'))
    {
        argText.remove_prefix(1);
    }
    if (argText.starts_with("this."))
    {
        argText.remove_prefix(5);
    }
    if (argText == paramName)
    {
        return true;
    }
    if (argText.size() == paramName.size())
    {
        return std::equal(
            argText.begin(), argText.end(), paramName.begin(), [](char a, char b) noexcept
            { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    }
    return false;
}

std::optional<lsp::Location> ResolveParameterLocation(std::string_view fallbackUri, std::string_view calleeUri,
                                                      const analysis::ParameterInformation& param,
                                                      const std::optional<analysis::SourceRange>& fallbackRange)
{
    std::string_view targetUri = calleeUri.empty() ? fallbackUri : calleeUri;
    if (targetUri.empty())
    {
        return std::nullopt;
    }
    const bool hasNameLoc =
        (param.nameStartLine != param.nameEndLine || param.nameStartCharacter != param.nameEndCharacter);
    const bool hasValidLoc =
        hasNameLoc || (param.startLine != param.endLine || param.startCharacter != param.endCharacter);
    if (!hasValidLoc)
    {
        if (fallbackRange.has_value())
        {
            lsp::Range paramRange{lsp::Position{fallbackRange->startLine, fallbackRange->startCharacter},
                                  lsp::Position{fallbackRange->endLine, fallbackRange->endCharacter}};
            return lsp::Location{common::ParseOrConvertUri(targetUri), paramRange};
        }
        return std::nullopt;
    }
    const uint32_t sL = hasNameLoc ? param.nameStartLine : param.startLine;
    const uint32_t sC = hasNameLoc ? param.nameStartCharacter : param.startCharacter;
    const uint32_t eL = hasNameLoc ? param.nameEndLine : param.endLine;
    const uint32_t eC = hasNameLoc ? param.nameEndCharacter : param.endCharacter;
    return lsp::Location{common::ParseOrConvertUri(targetUri),
                         lsp::Range{lsp::Position{sL, sC}, lsp::Position{eL, eC}}};
}

analysis::ExpressionTypeContext MakeExpressionTypeContext(const InlayHintRequest& request,
                                                          const analysis::Scope* scope) noexcept
{
    const std::string_view strType = (request.config && !request.config->types.stringTypeName.empty())
                                         ? std::string_view(request.config->types.stringTypeName)
                                         : request.symbolTable.GetStringType();
    const std::string_view arrType = (request.config && !request.config->types.arrayTypeName.empty())
                                         ? std::string_view(request.config->types.arrayTypeName)
                                         : request.symbolTable.GetDefaultArrayType();

    analysis::ExpressionTypeContext exprCtx{scope, request.symbolTable, request.sourceCode, request.uri};
    exprCtx.nodeIndex = request.nodeIndex;
    exprCtx.stringTypeName = strType;
    exprCtx.arrayTypeName = arrType;
    return exprCtx;
}

} // namespace angel_lsp::features
