#pragma once

#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactically parsed argument from an argument list node.
 */
struct CallArgumentInfo
{
    std::string name;
    TSNode exprNode{};
    TSNode nameNode{};
    uint32_t index = 0;
};

/**
 * @brief Extracted pair of call expression and its argument list node.
 */
struct CallNodes
{
    TSNode callNode{};
    TSNode argListNode{};
};

/**
 * @brief State tracking parser delimiters across argument lists.
 */
struct CallDelimiterState
{
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    int angleDepth = 0;
    bool inString = false;
    char stringChar = '\0';
    bool inLineComment = false;
    bool inBlockComment = false;

    [[nodiscard]] bool AtTopLevel() const noexcept
    {
        return parenDepth == 0 && bracketDepth == 0 && braceDepth == 0 && angleDepth == 0;
    }
};

/**
 * @brief Precalculated AST details for a single lambda parameter.
 */
struct LambdaParamASTInfo
{
    TSNode typeNode{};
    TSNode nameNode{};
    TSNode startNode{};
    std::string name;
    std::string typeName;
    uint32_t index = 0;
    ParameterModifier modifier = ParameterModifier::None;
    bool hasWrittenType : 1 = false;
    bool isConst : 1 = false;
    bool isHandle : 1 = false;
    bool isReference : 1 = false;
};

/**
 * @brief Resolves argument_list node from either argument_list, call_expression, or construct_call_expression.
 * @param[in] node AST node.
 * @return argument_list node or null node.
 */
TSNode ResolveArgumentListNode(TSNode node);

/**
 * @brief Structurally parses arguments from an argument_list AST node into a list of CallArgumentInfo.
 * @param[in] argumentList AST node representing an argument list or call expression.
 * @param[in] sourceCode Document source text buffer.
 * @return Vector of extracted argument information including expressions and explicit names.
 */
std::vector<CallArgumentInfo> ExtractCallArguments(TSNode argumentList, std::string_view sourceCode);

/**
 * @brief Returns the count of arguments in an argument list node.
 * @param[in] argumentList AST node representing an argument list or call expression.
 * @return Number of arguments.
 */
size_t CountCallArguments(TSNode argumentList);

/**
 * @brief Calculates the 0-based active parameter index at cursorByte within an argument list.
 * @param[in] argListNode Argument list AST node.
 * @param[in] cursorByte Byte offset of the cursor.
 * @param[in] sourceCode Document source text.
 * @return Active parameter index.
 */
uint32_t CalculateActiveCallParameter(TSNode argListNode, size_t cursorByte, std::string_view sourceCode);

/**
 * @brief Extracts parameter AST information from a lambda parameter list node.
 * @param[in] lambdaParamList AST node of the lambda parameter list.
 * @param[in] sourceCode Document source text.
 * @param[in] index Optional node index for type extraction.
 * @return Vector of extracted lambda parameter facts.
 */
std::vector<LambdaParamASTInfo> ExtractLambdaParameters(TSNode lambdaParamList, std::string_view sourceCode,
                                                        const NodeIndex* index = nullptr);

/**
 * @brief Reads high-level lambda parameter definitions from a lambda parameter list node.
 * @param[in] listNode AST node of the lambda parameter list.
 * @param[in] sourceCode Document source text.
 * @return Vector of lambda parameters.
 */
std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode);

/**
 * @brief Reports whether an AST node is a lambda expression.
 * @param[in] node AST node to inspect.
 * @return True if node symbol is lambda_expression.
 */
[[nodiscard]] bool IsLambdaExpression(TSNode node) noexcept;

} // namespace angel_lsp::analysis
