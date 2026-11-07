#pragma once

#include "analysis/index/InlayHintFacts.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if an AST node's row span overlaps with [startLine, endLine].
 * @param[in] node AST node to check.
 * @param[in] startLine Inclusive 0-based start line (0 means unbounded).
 * @param[in] endLine Inclusive 0-based end line (0 means unbounded).
 * @return True if node overlaps the queried row range.
 */
inline bool IsNodeOverlapping(TSNode node, uint32_t startLine, uint32_t endLine) noexcept
{
    return (startLine == 0 && endLine == 0) ||
           (ts_node_end_point(node).row >= startLine && ts_node_start_point(node).row <= endLine);
}

/**
 * @brief Slices a string_view safely within source bounds.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] startByte Inclusive start byte offset.
 * @param[in] endByte Exclusive end byte offset.
 * @return Substring view if offsets are valid; empty view otherwise.
 */
inline std::string_view SliceSource(std::string_view sourceCode, uint32_t startByte, uint32_t endByte) noexcept
{
    return (startByte < endByte && endByte <= sourceCode.size()) ? sourceCode.substr(startByte, endByte - startByte)
                                                                 : std::string_view{};
}

/**
 * @brief Populates coordinate positions for empty argument lists and closing parenthesis.
 * @param[in] listNode AST argument_list node.
 * @param[out] emptyLine Line for hint placement in an empty argument list.
 * @param[out] emptyCol Character for hint placement in an empty argument list.
 * @param[out] closeLine Line coordinate of closing parenthesis.
 * @param[out] closeCol Character coordinate of closing parenthesis.
 */
void PopulateArgListCoords(TSNode listNode, uint32_t& emptyLine, uint32_t& emptyCol, uint32_t& closeLine,
                           uint32_t& closeCol) noexcept;

/**
 * @brief Extracts call arguments from an AST argument list into the flat sites container.
 * @param[in] listNode AST argument_list node.
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] sites Destination flat container for call arguments.
 * @return Count of arguments appended.
 */
uint32_t ExtractArguments(TSNode listNode, std::string_view sourceCode, InlayHintSites& sites);

/**
 * @brief Processes a variable_declaration AST node to extract auto and constructor init declarators.
 * @param[in] node AST variable_declaration node.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] gs Precompiled grammar symbols.
 * @param[in,out] sites Destination container for extracted declarator facts.
 */
void ProcessVariableDeclarationNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs,
                                    InlayHintSites& sites);

/**
 * @brief Processes a lambda_expression AST node to extract untyped parameter and return facts.
 * @param[in] node AST lambda_expression node.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] gs Precompiled grammar symbols.
 * @param[in,out] sites Destination container for extracted lambda facts.
 */
void ProcessLambdaNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs,
                       InlayHintSites& sites);

/**
 * @brief Processes an enum_declaration AST node to extract member value numbering facts.
 * @param[in] node AST enum_declaration node.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] gs Precompiled grammar symbols.
 * @param[in,out] sites Destination container for extracted enum facts.
 */
void ProcessEnumNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs, InlayHintSites& sites);

} // namespace angel_lsp::analysis
