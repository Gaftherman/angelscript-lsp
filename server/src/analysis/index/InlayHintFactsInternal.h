#pragma once

#include "analysis/index/InlayHintFacts.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

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

/**
 * @brief Extracted coordinate spans and classification for a call target.
 */
struct CallTargetSpans
{
    TSNode funcNode{};               ///< Function or callee expression AST node.
    uint32_t calleeStartByte = 0;    ///< Inclusive start byte offset of the callee identifier.
    uint32_t calleeEndByte = 0;      ///< Exclusive end byte offset of the callee identifier.
    uint32_t receiverStartByte = 0;  ///< Inclusive start byte of the member-call object expression (0 if none).
    uint32_t receiverEndByte = 0;    ///< Exclusive end byte of the member-call object expression (0 if none).
    uint32_t qualifierStartByte = 0; ///< Inclusive start byte of the `A::B` qualifier of a scoped callee (0 if none).
    uint32_t qualifierEndByte = 0;   ///< Exclusive end byte of the qualifier; equals start for a global `::f`.
    bool isMember = false;           ///< True if callee is a member expression (e.g. obj.method()).
    bool isQualified = false;        ///< True if callee is written with a `::` qualifier, including global `::f`.
};

/**
 * @brief Extracts callee and receiver byte coordinates and classification from a function expression node.
 * @param[in] funcNode AST function node (member_expression, scoped_identifier, or identifier).
 * @param[in] gs Precompiled grammar symbols.
 * @return Extracted coordinate spans and classification.
 */
CallTargetSpans ExtractTargetSpansFromFunctionNode(TSNode funcNode, const parser::GrammarSymbols& gs) noexcept;

/**
 * @brief Extracts callee and receiver byte coordinates and classification from a call_expression.
 * @param[in] callNode AST call_expression node.
 * @param[in] gs Precompiled grammar symbols.
 * @return Extracted coordinate spans and classification; default-initialized if no callable child.
 */
CallTargetSpans ExtractCallTargetSpans(TSNode callNode, const parser::GrammarSymbols& gs) noexcept;

} // namespace angel_lsp::analysis
