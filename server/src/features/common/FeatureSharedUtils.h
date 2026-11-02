#pragma once

#include "analysis/SymbolTable.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include <concepts>
#include <cstdint>
#include <lsp/types.h>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::features::common
{

/**
 * @brief Converts a Tree-Sitter AST node range into an LSP protocol Range.
 * @param[in] node Valid Tree-Sitter AST node.
 * @return LSP Range spanning node start and end points.
 */
[[nodiscard]] inline lsp::Range ToLspRange(TSNode node) noexcept
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return lsp::Range{lsp::Position{start.row, start.column}, lsp::Position{end.row, end.column}};
}

/**
 * @brief Converts any range-like structure with line and character fields into an LSP protocol Range.
 * @tparam T Structure type containing startLine, startCharacter, endLine, and endCharacter.
 * @param[in] item Source range object.
 * @return Converted LSP Range.
 */
template <typename T>
    requires requires(const T& t) {
        { t.startLine } -> std::convertible_to<uint32_t>;
        { t.startCharacter } -> std::convertible_to<uint32_t>;
        { t.endLine } -> std::convertible_to<uint32_t>;
        { t.endCharacter } -> std::convertible_to<uint32_t>;
    }
[[nodiscard]] inline lsp::Range ToLspRange(const T& item) noexcept
{
    return lsp::Range{lsp::Position{item.startLine, item.startCharacter},
                      lsp::Position{item.endLine, item.endCharacter}};
}

/**
 * @brief Checks if two LSP protocol ranges are identical in start and end coordinates.
 * @param[in] a First LSP range.
 * @param[in] b Second LSP range.
 * @return True if start and end positions match exactly; false otherwise.
 */
[[nodiscard]] inline bool SameRange(const lsp::Range& a, const lsp::Range& b) noexcept
{
    return a.start.line == b.start.line && a.start.character == b.start.character && a.end.line == b.end.line &&
           a.end.character == b.end.character;
}

/**
 * @brief Finds the AST identifier node and text located at an LSP cursor position.
 * @param[in] tree Parsed Tree-Sitter syntax tree.
 * @param[in] position Cursor line and column.
 * @param[in] sourceCode Document source text buffer.
 * @param[out] outNode AST node matching the identifier under the cursor.
 * @return Extracted identifier text or empty string if not on an identifier.
 */
inline std::string FindIdentifierAtPosition(TSTree* tree, lsp::Position position, std::string_view sourceCode,
                                            TSNode& outNode)
{
    outNode = TSNode{};
    if (!tree)
    {
        return "";
    }

    const TSNode root = ts_tree_root_node(tree);
    const TSPoint point{position.line, position.character};
    TSNode node = ts_node_descendant_for_point_range(root, point, point);
    if (ts_node_is_null(node))
    {
        return "";
    }

    outNode = node;
    if (ts_node_symbol(node) != parser::GetGrammarSymbols().symIdentifier)
    {
        return "";
    }

    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return "";
    }

    return std::string(sourceCode.substr(start, end - start));
}

/**
 * @brief Maps internal AngelScript SymbolType to corresponding LSP SymbolKind.
 * @param[in] type Internal SymbolType enum value.
 * @param[in] isMember True if symbol is contained in a class or struct.
 * @return Equivalent LSP SymbolKind.
 */
[[nodiscard]] inline lsp::SymbolKind ToLspSymbolKind(analysis::SymbolType type, bool isMember = false) noexcept
{
    switch (type)
    {
    case analysis::SymbolType::Class:
        return lsp::SymbolKind::Class;
    case analysis::SymbolType::Interface:
        return lsp::SymbolKind::Interface;
    case analysis::SymbolType::Enum:
        return lsp::SymbolKind::Enum;
    case analysis::SymbolType::Namespace:
        return lsp::SymbolKind::Namespace;
    case analysis::SymbolType::Funcdef:
        return lsp::SymbolKind::Function;
    case analysis::SymbolType::Typedef:
        return lsp::SymbolKind::Class;
    case analysis::SymbolType::Property:
        return lsp::SymbolKind::Property;
    case analysis::SymbolType::Function:
        return isMember ? lsp::SymbolKind::Method : lsp::SymbolKind::Function;
    case analysis::SymbolType::Variable:
        return isMember ? lsp::SymbolKind::Field : lsp::SymbolKind::Variable;
    default:
        return lsp::SymbolKind::Variable;
    }
}

} // namespace angel_lsp::features::common
