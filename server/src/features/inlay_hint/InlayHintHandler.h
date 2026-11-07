#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Context and immutable input parameters for an Inlay Hint request.
 */
struct InlayHintRequest
{
    std::string_view uri{};                         ///< Document URI.
    std::string_view sourceCode{};                  ///< Document source text.
    const analysis::NodeIndex* nodeIndex = nullptr; ///< Document node index.
    lsp::Range range{};                             ///< Requested range.
    const analysis::SymbolTable& symbolTable;       ///< Global symbol table.
    const analysis::ScopeIndex& scopeIndex;         ///< Workspace scope index.
    bool suppressWhenArgumentMatchesName = false;   ///< Suppression flag.
    angel_lsp::utils::LspLogger* logger = nullptr;  ///< Logger instance.
    size_t maxParameters = 0;                       ///< Max parameter hint count.
    size_t maxLength = 0;                           ///< Max label length.
    config::OmittedDefaultArgumentsMode omittedDefaultArguments =
        config::OmittedDefaultArgumentsMode::Off; ///< Omitted args mode.
    const config::ServerConfig* config = nullptr; ///< Active config.
    const angel_lsp::i18n::I18n* i18n = nullptr;  ///< Localization instance.
};

/**
 * @brief List of inlay hints to display in the editor.
 */
using InlayHintResult = std::vector<lsp::InlayHint>;

/**
 * @brief Computes inlay hints within the requested document range.
 * @param[in] request Immutable context for inlay hint computation.
 * @return Optional vector of InlayHint items; nullopt if document cannot be parsed or no hints found.
 */
std::optional<InlayHintResult> GetInlayHints(const InlayHintRequest& request);

} // namespace angel_lsp::features
