#pragma once

#include "analysis/index/InlayHintFacts.h"
#include "analysis/index/Symbol.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolved callee callable and parameter information for inlay hint generation.
 */
struct ResolvedCallee
{
    std::string fileUri;
    std::string calleeName;
    std::vector<ParameterInformation> parameters;
    std::optional<SourceRange> selectionRange;
};

/**
 * @brief Resolves the callee function/method and parameter information for an indexed call site.
 * @param[in] site Call site fact record.
 * @param[in] args Slice of argument fact records for the call site.
 * @param[in] ctx Semantic expression type context.
 * @return Resolved callee information, or std::nullopt if unresolvable.
 * @note Transitional: recovers TSNode internally until analysis/resolution is migrated (Rule 4.10).
 */
std::optional<ResolvedCallee> ResolveCallSiteCallee(const IndexedCallSite& site,
                                                    std::span<const IndexedCallArgument> args,
                                                    const ExpressionTypeContext& ctx);

/**
 * @brief Resolves constructor parameters for a variable direct-initialization declarator.
 * @param[in] decl Constructor init declarator fact record.
 * @param[in] args Slice of argument fact records for the declarator.
 * @param[in] ctx Semantic expression type context.
 * @return Resolved callee information, or std::nullopt if unresolvable.
 * @note Transitional: recovers TSNode internally until analysis/resolution is migrated (Rule 4.10).
 */
std::optional<ResolvedCallee> ResolveConstructorInitCallee(const IndexedConstructorInitDeclarator& decl,
                                                           std::span<const IndexedCallArgument> args,
                                                           const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
