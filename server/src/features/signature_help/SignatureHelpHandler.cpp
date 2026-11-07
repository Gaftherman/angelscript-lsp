#include "features/signature_help/SignatureHelpHandler.h"

#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CallContextFacts.h"
#include "analysis/resolution/CallCandidates.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"

#include <optional>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Selects active signature matching the active parameter index.
 * @param[in] signatures Available signature list.
 * @param[in] activeParameter Active parameter index.
 * @return Zero-based index of the matching signature.
 */
static uint32_t SelectActiveSignature(const std::vector<lsp::SignatureInformation>& signatures,
                                      uint32_t activeParameter)
{
    for (size_t i = 0; i < signatures.size(); ++i)
    {
        if (signatures[i].parameters.has_value() && activeParameter < signatures[i].parameters->size())
        {
            return static_cast<uint32_t>(i);
        }
    }
    return 0;
}

std::optional<lsp::SignatureHelp> GetSignatureHelp(const SignatureHelpRequest& request)
{
    if (request.sourceCode.empty() || !request.nodeIndex)
    {
        return std::nullopt;
    }

    const size_t cursorByte = utils::PositionToOffset(request.sourceCode, request.position.line,
                                                      request.position.character, utils::PositionEncoding::Utf16);
    const auto context = analysis::FindCallContext(*request.nodeIndex, static_cast<uint32_t>(cursorByte));
    if (!context)
    {
        return std::nullopt;
    }

    const auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope =
        rootScope ? FindInnermostScope(rootScope.get(), request.position.line, request.position.character) : nullptr;

    const auto candidates = analysis::ResolveCallCandidates(*context, request.sourceCode, scope, request.symbolTable);
    if (candidates.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::SignatureInformation> signatures;
    signatures.reserve(candidates.size());

    for (const auto& sym : candidates)
    {
        if (sym.type != analysis::SymbolType::Function && sym.type != analysis::SymbolType::Funcdef)
        {
            continue;
        }

        analysis::FormattedSignature formatted = analysis::FormatSignatureWithParameterOffsets(sym);
        lsp::SignatureInformation sigInfo;
        sigInfo.label = std::move(formatted.label);

        if (!formatted.parameterOffsets.empty())
        {
            std::vector<lsp::ParameterInformation> params;
            params.reserve(formatted.parameterOffsets.size());
            for (const auto& offset : formatted.parameterOffsets)
            {
                lsp::ParameterInformation pInfo;
                pInfo.label = lsp::Tuple<lsp::uint, lsp::uint>{offset.first, offset.second};
                params.push_back(std::move(pInfo));
            }
            sigInfo.parameters = std::move(params);
        }
        signatures.push_back(std::move(sigInfo));
    }

    if (signatures.empty())
    {
        return std::nullopt;
    }

    const uint32_t activeSig = SelectActiveSignature(signatures, context->activeParameter);

    lsp::SignatureHelp result;
    result.signatures = std::move(signatures);
    result.activeSignature = activeSig;
    result.activeParameter = context->activeParameter;
    return result;
}

} // namespace angel_lsp::features
