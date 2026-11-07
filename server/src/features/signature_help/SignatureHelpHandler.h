#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a signature help request.
 */
struct SignatureHelpRequest
{
    std::string_view uri;
    std::string_view sourceCode;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    lsp::Position position;
    const analysis::NodeIndex* nodeIndex = nullptr; ///< Document node index for AST fact queries.
};

/**
 * @brief Computes signature help info and active parameter index for function call.
 * @param request Immutable context for signature help.
 * @return Optional SignatureHelp struct; nullopt if position is not in a function call.
 */
std::optional<lsp::SignatureHelp> GetSignatureHelp(const SignatureHelpRequest& request);
} // namespace angel_lsp::features
