#pragma once

#include "analysis/index/SymbolTable.h"
#include "utils/LspLogger.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for an implementation request.
 */
struct ImplementationRequest
{
    std::string_view uri{};
    std::string_view sourceCode{};
    const analysis::SymbolTable& symbolTable;
    lsp::Position position{};
    angel_lsp::utils::LspLogger* logger = nullptr;
};

/**
 * @brief Resolves what implements or overrides the symbol under the cursor.
 *        Definition answers "where is this declared"; this answers the opposite question, "who
 * @param[in] request Document, tree, symbol table and cursor position.
 * @return Locations of the implementations, or nullopt when the cursor is not on a name this
 *        question applies to.
 */
[[nodiscard]] std::optional<std::vector<lsp::Location>> GetImplementations(const ImplementationRequest& request);

/**
 * @brief Accumulates derived classes and interface implementations during type hierarchy search.
 * @warning seen and next borrow type names from the RuleIndex snapshot being walked; that snapshot must outlive
 *          the accumulator.
 */
struct SubtypeAccumulator
{
    ankerl::unordered_dense::set<std::string_view>& seen;
    std::vector<std::string_view>& next;
    std::vector<analysis::Symbol>& subtypes;
};

/**
 * @brief Collects LSP location structures for symbol definitions and implementations.
 */
struct LocationCollector
{
    std::vector<lsp::Location> locations;
    ankerl::unordered_dense::set<std::pair<uint64_t, uint64_t>> seenLocs; ///< (file URI hash, line/column key).

    /**
     * @brief Adds a symbol location if not already collected.
     * @param[in] sym analysis::Symbol whose location should be captured.
     */
    void Add(const analysis::Symbol& sym);
};

/**
 * @brief Context for searching overriding member implementations across derived types.
 */
struct MemberImplementationContext
{
    std::string_view name; ///< Member name under the cursor; borrows from the request source.
    bool isMixinOwner;
    const analysis::SymbolTable& table;
    LocationCollector& collector;
};
} // namespace angel_lsp::features
