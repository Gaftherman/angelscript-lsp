#pragma once

#include "analysis/index/HoverFacts.h"
#include "parser/ASTUtils.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks if a token text and TSSymbol match hoverable identifier rules.
 * @param[in] txt Token string view.
 * @param[in] sym Grammar symbol ID.
 * @return True if hoverable, false otherwise.
 */
[[nodiscard]] bool IsHoverableIdentifier(std::string_view txt, TSSymbol sym);

/**
 * @brief Extracts the hover target AST node under the cursor point with previous-column fallback.
 * @param[in] rootNode Tree root node.
 * @param[in] sourceCode Source text buffer.
 * @param[in] point Cursor position coordinates.
 * @return Hover target TSNode if found, null node otherwise.
 */
[[nodiscard]] TSNode ExtractHoverNode(TSNode rootNode, std::string_view sourceCode, TSPoint point);

/**
 * @brief Resolves enclosing call or construct call node from a candidate AST node.
 * @param[in] node Starting syntax node.
 * @return Enclosing call or construct call node, or null node.
 */
[[nodiscard]] TSNode FindEnclosingCallNode(TSNode node);

/**
 * @brief Checks if a node corresponds to a direct-initialization variable declarator.
 * @param[in] node AST node.
 * @return True if declarator with argument list, false otherwise.
 */
[[nodiscard]] bool IsDirectInitDeclarator(TSNode node);

/**
 * @brief Determines if an AST node is positioned in a type specifier or using namespace context.
 * @param[in] node AST node to inspect.
 * @return Context preference indicating class, namespace, or neutral.
 */
[[nodiscard]] ContainerContextPreference CheckContextPreference(TSNode node);

/**
 * @brief Checks if an AST node acts as a scope qualifier preceding double colons.
 * @param[in] node Syntax node to inspect.
 * @return True if preceding scope separator, false otherwise.
 */
[[nodiscard]] bool IsScopeQualifier(TSNode node) noexcept;

/**
 * @brief Locates the next identifier node following scope colons in a scoped identifier.
 * @param[in] parent Scoped identifier parent AST node.
 * @param[in] node Current qualifier syntax node.
 * @return Next member TSNode or null node.
 */
[[nodiscard]] TSNode FindNextScopedMemberNode(TSNode parent, TSNode node);

/**
 * @brief Verifies whether the node is the member field of a member expression.
 * @param[in] node Member candidate AST node.
 * @param[in] parent Parent member expression AST node.
 * @return True if member child, false otherwise.
 */
[[nodiscard]] bool IsMemberChildOfExpression(TSNode node, TSNode parent);

/**
 * @brief Verifies whether the cursor node is the label before colon in a named argument.
 * @param[in] node Argument label candidate node.
 * @param[in] parent Parent named argument AST node.
 * @return True if named argument label, false otherwise.
 */
[[nodiscard]] bool IsNamedArgumentLabelAtCursor(TSNode node, TSNode parent);

/**
 * @brief Finds the innermost enclosing lambda expression for a syntax node.
 * @param[in] node Starting syntax node.
 * @return Enclosing lambda TSNode or null node.
 */
[[nodiscard]] TSNode FindEnclosingLambda(TSNode node);

/**
 * @brief Finds the enclosing variable or foreach declaration for an auto keyword.
 * @param[in] node Starting syntax node.
 * @return Enclosing declaration TSNode or null node.
 */
[[nodiscard]] TSNode FindEnclosingAutoDeclaration(TSNode node);

/**
 * @brief Finds the topmost binary plus expression enclosing a string concatenation operand.
 * @param[in] node Starting syntax node.
 * @return Topmost binary plus TSNode or original node.
 */
[[nodiscard]] TSNode FindTopmostBinaryPlus(TSNode node);

/**
 * @brief Determines if a syntax node represents a qualified member or scope leaf.
 * @param[in] node Syntax node to inspect.
 * @return True if qualified member, false otherwise.
 */
[[nodiscard]] bool IsQualifiedMemberNode(TSNode node);

/**
 * @brief Finds the non-empty `scope` child of the `type` that holds a datatype, as `ns::` in `ns::T`.
 * @param[in] datatypeNode Datatype node whose parent is inspected.
 * @return The first `scope` child when it spans text; a null node otherwise.
 * @note Same node `ExtractDatatypeScopePrefix` reads its prefix text from.
 */
[[nodiscard]] TSNode FindDatatypeScopeNode(TSNode datatypeNode);
} // namespace angel_lsp::analysis
