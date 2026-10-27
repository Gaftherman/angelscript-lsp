#pragma once

/**
 * @file BreakpointStatementClassifier.h
 * @brief Helper routines to classify Tree-Sitter AST nodes for executable breakpoint locations.
 */

#include "features/breakpoints/BreakpointLocationsHandler.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Context passed to breakpoint collectors and statement classifiers.
 */
struct CollectorContext
{
    const BreakpointLocationsRequest& req;
    const parser::GrammarSymbols& gs;
    bool inStatementBlock = false;
    std::vector<BreakpointLocationItem>& out;
};

/**
 * @brief Checks whether an AST subtree should be skipped entirely.
 * @param[in] sym Tree-Sitter symbol of the node.
 * @param[in] gs Pre-cached grammar symbols.
 * @return True if the subtree cannot contain executable statements.
 */
bool ShouldSkipSubtree(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept;

/**
 * @brief Validates if an AST node is valid and non-empty.
 * @param[in] node AST node to validate.
 * @return True if null, missing, error, or zero length.
 */
bool IsNodeInvalid(TSNode node) noexcept;

/**
 * @brief Determines if a source row falls within the query request range and is not preprocessor-excluded.
 * @param[in] row 0-based source line.
 * @param[in] req Breakpoint locations request.
 * @return True if the row is within range and not excluded.
 */
bool IsRowInRangeAndNotExcluded(uint32_t row, const BreakpointLocationsRequest& req) noexcept;

/**
 * @brief Emits a breakpoint location if its starting row falls within the query request range.
 * @param[in] start Start position in the source document.
 * @param[in] end End position in the source document.
 * @param[in] req Breakpoint locations request.
 * @param[in,out] out Output container for breakpoint locations.
 */
void EmitLocationIfInRange(TSPoint start, TSPoint end, const BreakpointLocationsRequest& req,
                           std::vector<BreakpointLocationItem>& out);

/**
 * @brief Determines if a type node represents a bare primitive or handle type without a default constructor.
 * @param[in] typeNode Type AST node.
 * @param[in] gs Pre-cached grammar symbols.
 * @return True for bare primitives and uninitialized handles; false for object types and arrays (`int[]`).
 */
bool IsBarePrimitiveOrHandleType(TSNode typeNode, const parser::GrammarSymbols& gs) noexcept;

/**
 * @brief Determines whether a variable declaration emits runtime executable bytecode.
 * @param[in] node Variable declaration AST node.
 * @param[in] inStatementBlock Whether the declaration is inside an executable statement block.
 * @param[in] gs Pre-cached grammar symbols.
 * @return True if the declaration requires runtime execution.
 */
bool IsExecutableVariableDecl(TSNode node, bool inStatementBlock, const parser::GrammarSymbols& gs) noexcept;

/**
 * @brief Emits closing brace breakpoint location for function-like constructs.
 * @param[in] node Function, accessor, or lambda AST node.
 * @param[in] sym Tree-Sitter symbol of the node.
 * @param[in,out] ctx Active collector context.
 */
void HandleFunctionLike(TSNode node, TSSymbol sym, CollectorContext& ctx);

/**
 * @brief Emits breakpoint locations for loop and branch statement headers and column positions.
 * @param[in] node Compound statement AST node.
 * @param[in] sym Tree-Sitter symbol of the node.
 * @param[in,out] ctx Active collector context.
 * @return True if the node was handled as a loop or branch construct.
 */
bool HandleLoopsAndBranches(TSNode node, TSSymbol sym, CollectorContext& ctx);

/**
 * @brief Emits breakpoint locations for leaf statements (expressions, returns, jumps).
 * @param[in] node Leaf statement AST node.
 * @param[in] sym Tree-Sitter symbol of the node.
 * @param[in,out] ctx Active collector context.
 */
void HandleLeafStatement(TSNode node, TSSymbol sym, CollectorContext& ctx);

} // namespace angel_lsp::features
