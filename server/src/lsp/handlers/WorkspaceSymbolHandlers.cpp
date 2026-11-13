#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp
{

lsp::requests::Workspace_TextDocumentContent::Result
Server::HandleRequestsWorkspace_TextDocumentContent(lsp::requests::Workspace_TextDocumentContent::Params&& req)
{
    const std::string requested = req.uri.toString();
    static constexpr std::string_view k_scheme = "angelscript-predefined:";
    static constexpr std::string_view k_virtual_scheme = "angelscript-virtual:";

    if (requested.starts_with(k_virtual_scheme))
    {
        lsp::TextDocumentContentResult result;
        result.text = GenerateVirtualMixinDocument(requested);
        return result;
    }

    if (!requested.starts_with(k_scheme))
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams, "Unsupported scheme: " + requested);
    }

    const std::string wanted =
        angel_lsp::utils::IncludeResolver::NormalizePath(std::string_view(requested).substr(k_scheme.size()));

    std::string fileUri;
    if (const auto uriOpt = m_predefinedManager.GetUriByPath(wanted))
    {
        fileUri = *uriOpt;
    }

    if (fileUri.empty())
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams, "No predefined stub is loaded for: " + requested);
    }

    std::ifstream file(angel_lsp::utils::UriToPath(fileUri), std::ios::binary);
    if (!file.is_open())
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams,
                                "The predefined stub could not be read: " + requested);
    }

    lsp::TextDocumentContentResult result;
    result.text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return result;
}

lsp::requests::Workspace_Symbol::Result
Server::HandleRequestsWorkspace_Symbol(lsp::requests::Workspace_Symbol::Params&& req)
{
    if (!m_config.features.enableWorkspaceSymbols)
    {
        return lsp::Array<lsp::SymbolInformation>{};
    }
    features::WorkspaceSymbolRequest wr{req.query, m_symbolTable};
    auto symbols = features::GetWorkspaceSymbols(wr);
    if (symbols.has_value())
    {
        EncodeAcrossDocuments(symbols.value());
        return symbols.value();
    }
    return lsp::Array<lsp::SymbolInformation>{};
}

lsp::requests::WorkspaceSymbol_Resolve::Result
Server::HandleRequestsWorkspaceSymbol_Resolve(lsp::requests::WorkspaceSymbol_Resolve::Params&& params)
{
    if (!m_config.features.enableWorkspaceSymbols)
    {
        return std::move(params);
    }

    // Workspace symbols are indexed and returned with their full container and location
    // information in workspace/symbol. This handler returns the symbol unchanged so
    // clients resolving workspace symbols receive a valid response instead of MethodNotFound.
    return std::move(params);
}

} // namespace angel_lsp
