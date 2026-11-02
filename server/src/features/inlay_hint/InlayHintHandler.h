#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and immutable input parameters for an Inlay Hint request.
 */
struct InlayHintRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    lsp::Range range;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    bool suppressWhenArgumentMatchesName = false;
    angel_lsp::utils::LspLogger* logger = nullptr;
    size_t maxParameters = 0;
    size_t maxLength = 0;
    config::OmittedDefaultArgumentsMode omittedDefaultArguments = config::OmittedDefaultArgumentsMode::Off;
    const config::ServerConfig* config = nullptr;
    const angel_lsp::i18n::I18n* i18n = nullptr;
};

/**
 * @brief List of inlay hints to display in the editor.
 */
using InlayHintResult = std::vector<lsp::InlayHint>;

/**
 * @brief Computes inlay hints (parameter names for function/method calls,
 *        and type deduction for auto variables) within the requested document range.
 * @param request Immutable context for inlay hint computation.
 * @return Optional vector of InlayHint items; nullopt if document cannot be parsed or no hints found.
 */
std::optional<InlayHintResult> GetInlayHints(const InlayHintRequest& request);

/**
 * @brief Resolved callee callable and parameter information for inlay hint generation.
 */
struct CalleeResolutionResult
{
    std::string fileUri;
    std::string calleeName;
    std::vector<analysis::ParameterInformation> parameters;
    std::optional<lsp::Range> selectionRange;
};

/**
 * @brief Argument expression AST node and evaluated parameter binding for inlay hints.
 */
struct ArgInfo
{
    TSNode exprNode;
    bool isNamed = false;
    std::string argName;
    lsp::Position hintPosition;
    std::string text;
};

/**
 * @brief Context for generating parameter name inlay hints at a call site.
 */
struct ParamHintContext
{
    const CalleeResolutionResult& callee;
    const InlayHintRequest& request;
};

/**
 * @brief Context for suppressing parameter inlay hints on self-explanatory arguments.
 */
struct OmittedHintContext
{
    const InlayHintRequest& request;
    std::vector<lsp::InlayHint>& hints;
};

/**
 * @brief Position and character offsets of an argument within a call parameter list.
 */
struct CallArgPosition
{
    TSNode startNode;
    TSNode endNode;
    int boundParamIndex = -1;
};

/**
 * @brief Tracks reasons for omitting an inlay hint on a specific call argument.
 */
struct OmittedParamHint
{
    std::string label;
    std::string tooltip;
    std::string fileUri;
    const analysis::ParameterInformation* param = nullptr;
};

/**
 * @brief Helper for deducing expression types during inlay hint generation.
 */
struct InlayHintExpressionDeducer
{
    /**
     * @brief Deduces the type string for an expression AST node.
     * @param[in] exprNode Target expression AST node.
     * @param[in] request Inlay hint request context.
     * @return Deduced type string or empty string.
     */
    static std::string DeduceExpressionType(TSNode exprNode, const InlayHintRequest& request);
};
} // namespace angel_lsp::features
