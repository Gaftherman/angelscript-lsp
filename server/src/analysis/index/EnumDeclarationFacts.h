#pragma once

#include "analysis/index/CodeActionFacts.h"
#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief One enumerator of an enum declaration: its spelling and where the name sits, used to report duplicates.
 */
struct IndexedEnumMember
{
    /**
     * @brief Identifier name of the enum member borrowing from document source.
     */
    std::string_view name{};

    /**
     * @brief Source span of the enum member identifier node.
     */
    IndexedSourceSpan span{};
};

/**
 * @brief One enum declaration with its enumerators in source order, found by the declaration line and name.
 */
struct IndexedEnumDeclaration
{
    /**
     * @brief Identifier name of the enum declaration borrowing from document source.
     */
    std::string_view name{};

    /**
     * @brief 0-based start coordinate point of the enum declaration AST node.
     */
    IndexedSourcePoint start{};

    /**
     * @brief Member elements of the enum recorded in syntactic declaration order.
     */
    std::vector<IndexedEnumMember> members{};
};

/**
 * @brief Extracts AST-free facts for all enum declarations in the document.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] source Document source text buffer.
 * @return Collection of extracted enum declarations in syntactic declaration order.
 */
[[nodiscard]] std::vector<IndexedEnumDeclaration> BuildEnumDeclarationFacts(const NodeIndex& index,
                                                                            std::string_view source);

} // namespace angel_lsp::analysis
