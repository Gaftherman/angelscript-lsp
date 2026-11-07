#pragma once

#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/MixinInclusion.h"
#include "analysis/resolution/ReferenceCounting.h"
#include "features/code_lens/CodeLensHandler.h"
#include "utils/PositionEncoding.h"

#include <ankerl/unordered_dense.h>
#include <lsp/types.h>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
using GroupMap =
    ankerl::unordered_dense::map<analysis::SourceRange, std::vector<analysis::Symbol>, analysis::SourceRangeHash>;

using KeyTargetMap = ankerl::unordered_dense::map<analysis::SourceRange, size_t, analysis::SourceRangeHash>;

/**
 * @brief Groups symbols by declaration range in document order.
 */
struct SymbolGroups
{
    std::vector<analysis::SourceRange> rangeOrder;
    GroupMap groups;
};

/**
 * @brief Context assembling computed targets and line index for emitting CodeLens results.
 */
struct AssembledLensContext
{
    const std::vector<analysis::ReferenceCountTarget>& targets;
    const KeyTargetMap& keyToTarget;
    const CodeLensRequest& request;
    utils::LineIndex lines;
};

/**
 * @brief Bundles context needed to collect reference targets.
 */
struct TargetCollectionContext
{
    const analysis::MixinClassContext& mixinCtx;
    std::string_view sourceCode;
    KeyTargetMap& keyToTarget;
};

/**
 * @brief Converts a string_view to an lsp::json::Value string without naming std::string.
 * @param[in] view Source character view.
 * @return JSON string value.
 */
inline lsp::json::Value MakeJsonString(std::string_view view)
{
    return lsp::json::Value(lsp::json::String(view));
}

/**
 * @brief Constructs a command code lens with the given title and range.
 * @param[in] key Range covering the symbol.
 * @param[in] title Human-readable command title.
 * @return Configured CodeLens.
 */
inline lsp::CodeLens MakeCommandLens(const analysis::SourceRange& key, lsp::String title)
{
    lsp::CodeLens lens;
    lens.range =
        lsp::Range{lsp::Position{key.startLine, key.startCharacter}, lsp::Position{key.endLine, key.endCharacter}};
    lsp::Command cmd;
    cmd.title = std::move(title);
    cmd.command = "";
    lens.command = std::move(cmd);
    return lens;
}

/**
 * @brief Collects and groups symbols declared in the active document.
 * @param[in] request Active CodeLens request.
 * @return Groups of symbols ordered by declaration range.
 */
[[nodiscard]] SymbolGroups CollectSymbolGroups(const CodeLensRequest& request);

/**
 * @brief Selects the primary symbol from a group, preferring non-synthesized declarations.
 * @param[in] symGroup Group of symbols sharing a declaration range.
 * @return Primary symbol reference.
 */
[[nodiscard]] const analysis::Symbol& SelectPrimarySymbol(const std::vector<analysis::Symbol>& symGroup);

/**
 * @brief Registers reference targets for symbols that require reference counts.
 * @param[in] symGroups Symbol groups in the active document.
 * @param[in] ctx Context bundling mixin map, source code, and key map.
 * @param[out] targets List of collected reference targets.
 */
void CollectReferenceTargets(const SymbolGroups& symGroups, const TargetCollectionContext& ctx,
                             std::vector<analysis::ReferenceCountTarget>& targets);
} // namespace angel_lsp::features
