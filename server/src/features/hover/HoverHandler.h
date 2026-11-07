#pragma once

#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"

#include <lsp/types.h>
#include <optional>
#include <string_view>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a hover request.
 */
struct HoverRequest
{
    std::string_view uri{};                         ///< Document URI.
    std::string_view sourceCode{};                  ///< Document source text.
    const analysis::NodeIndex* nodeIndex = nullptr; ///< Document node index for AST-free fact queries.
    const analysis::SymbolTable& symbolTable;       ///< Workspace symbol table.
    const analysis::ScopeIndex& scopeIndex;         ///< Lexical scope index.
    lsp::Position position{};                       ///< Cursor line and character offset.
    analysis::HoverDocumentSources sources = {};    ///< Document text and include resolution callbacks.
    const config::ServerConfig* config = nullptr;   ///< Server engine configuration.
    angel_lsp::utils::LspLogger* logger = nullptr;  ///< Optional LSP logger for performance telemetry.
    const angel_lsp::i18n::I18n* i18n = nullptr;    ///< Localization dictionary.
};

/**
 * @brief Computes hover tooltip information for symbol under cursor.
 * @param[in] request Immutable context for hover computation.
 * @return Optional Hover object, nullopt if no symbol found.
 */
std::optional<lsp::Hover> GetHover(const HoverRequest& request);
} // namespace angel_lsp::features
