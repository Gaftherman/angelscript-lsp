#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Extracted argument expression metadata and hint placement coordinates.
 */
struct IndexedCallArgument
{
    SourceRange range{};            ///< Source coordinate range of the argument expression.
    uint32_t startByte = 0;         ///< Start byte offset in document text.
    uint32_t endByte = 0;           ///< End byte offset in document text.
    uint32_t hintLine = 0;          ///< 0-based line coordinate for parameter name hint.
    uint32_t hintCharacter = 0;     ///< 0-based character coordinate for parameter name hint.
    uint32_t argStartLine = 0;      ///< 0-based line coordinate of start of argument (including name if named).
    uint32_t argStartCharacter = 0; ///< 0-based character coordinate of start of argument (including name if named).
    std::string_view name{};        ///< Named argument identifier if explicitly written (e.g. name: expr).
    std::string_view text{};        ///< Raw argument expression text.
};

/**
 * @brief Extracted call site metadata and argument slice indices.
 */
struct IndexedCallSite
{
    SourceRange callRange{};              ///< Source coordinate range of the call expression.
    uint32_t startByte = 0;               ///< Start byte offset of the call expression.
    uint32_t endByte = 0;                 ///< End byte offset of the call expression.
    uint32_t calleeStartByte = 0;         ///< Start byte offset of the callee function/method identifier.
    uint32_t calleeEndByte = 0;           ///< End byte offset of the callee function/method identifier.
    uint32_t receiverStartByte = 0;       ///< Start byte offset of receiver object expression (0 if none).
    uint32_t receiverEndByte = 0;         ///< End byte offset of receiver object expression (0 if none).
    std::string_view calleeText{};        ///< Callee name or member text.
    std::string_view constructTypeName{}; ///< Explicit constructor type name for construct expressions.
    uint32_t emptyArgsLine = 0;           ///< 0-based line for omitted hints in empty argument lists.
    uint32_t emptyArgsCharacter = 0;      ///< 0-based character for omitted hints in empty argument lists.
    uint32_t closeParenLine = 0;          ///< 0-based line coordinate of closing parenthesis.
    uint32_t closeParenCharacter = 0;     ///< 0-based character coordinate of closing parenthesis.
    uint32_t firstArgument = 0;           ///< First index in InlayHintSites::callArguments.
    uint32_t argumentCount = 0;           ///< Number of arguments in InlayHintSites::callArguments.
    bool isMemberCall = false;            ///< True if callee is a member expression (e.g. obj.method()).
    bool isConstructorCall = false;       ///< True if construct_call_expression.
    bool hasArgumentList = false;         ///< True if an argument list was syntactically present.
};

/**
 * @brief Extracted auto variable declarator coordinates and initializer range.
 */
struct IndexedAutoDeclarator
{
    SourceRange nameRange{};        ///< Source coordinate range of variable identifier.
    uint32_t startByte = 0;         ///< Start byte offset of the declarator.
    uint32_t endByte = 0;           ///< End byte offset of the declarator.
    uint32_t nameEndLine = 0;       ///< 0-based line coordinate immediately following variable name.
    uint32_t nameEndCharacter = 0;  ///< 0-based character coordinate immediately following variable name.
    uint32_t initExprStartByte = 0; ///< Start byte offset of initializer expression.
    uint32_t initExprEndByte = 0;   ///< End byte offset of initializer expression.
    bool isHandle = false;          ///< True if declared with handle modifier (e.g. auto@).
};

/**
 * @brief Extracted direct-initialization variable declarator coordinates and arguments.
 */
struct IndexedConstructorInitDeclarator
{
    SourceRange declRange{};             ///< Source coordinate range of variable declarator.
    std::string_view declaredTypeName{}; ///< Declared type identifier of the variable.
    uint32_t startByte = 0;              ///< Start byte offset of the declarator.
    uint32_t endByte = 0;                ///< End byte offset of the declarator.
    uint32_t emptyArgsLine = 0;          ///< 0-based line coordinate for empty argument list hint.
    uint32_t emptyArgsCharacter = 0;     ///< 0-based character coordinate for empty argument list hint.
    uint32_t closeParenLine = 0;         ///< 0-based line coordinate of closing parenthesis.
    uint32_t closeParenCharacter = 0;    ///< 0-based character coordinate of closing parenthesis.
    uint32_t firstArgument = 0;          ///< First index in InlayHintSites::callArguments.
    uint32_t argumentCount = 0;          ///< Number of arguments in InlayHintSites::callArguments.
    bool hasArgumentList = false;        ///< True if an argument list was syntactically present.
};

/**
 * @brief Extracted untyped lambda parameter identifier and placement.
 */
struct IndexedLambdaParam
{
    std::string_view name{};    ///< Identifier of the untyped lambda parameter.
    uint32_t startByte = 0;     ///< Start byte offset of parameter node.
    uint32_t endByte = 0;       ///< End byte offset of parameter node.
    uint32_t hintLine = 0;      ///< 0-based line coordinate for parameter type hint.
    uint32_t hintCharacter = 0; ///< 0-based character coordinate for parameter type hint.
};

/**
 * @brief Extracted lambda expression metadata and return expression range.
 */
struct IndexedLambdaSite
{
    SourceRange lambdaRange{};            ///< Source coordinate range of the lambda expression.
    uint32_t startByte = 0;               ///< Start byte offset of the lambda expression.
    uint32_t endByte = 0;                 ///< End byte offset of the lambda expression.
    uint32_t returnTypeHintLine = 0;      ///< 0-based line coordinate at parameter list close.
    uint32_t returnTypeHintCharacter = 0; ///< 0-based character coordinate at parameter list close.
    uint32_t returnExprStartByte = 0;     ///< Start byte offset of return expression (0 if none).
    uint32_t returnExprEndByte = 0;       ///< End byte offset of return expression (0 if none).
    uint32_t firstParam = 0;              ///< First index in InlayHintSites::lambdaParams.
    uint32_t paramCount = 0;              ///< Number of untyped parameters in InlayHintSites::lambdaParams.
    bool hasExplicitReturnType = false;   ///< True if return type is explicitly specified.
    bool hasReturnStatement = false;      ///< True if lambda body contains at least one return.
};

/**
 * @brief Extracted enum member coordinate, name, and evaluated integer values.
 */
struct IndexedEnumValue
{
    std::string_view memberName{};          ///< Identifier of the enum member.
    uint32_t startByte = 0;                 ///< Start byte offset of the enum member.
    uint32_t endByte = 0;                   ///< End byte offset of the enum member.
    uint32_t hintLine = 0;                  ///< 0-based line coordinate immediately following member name.
    uint32_t hintCharacter = 0;             ///< 0-based character coordinate immediately following member name.
    std::optional<int64_t> explicitValue{}; ///< Explicit integer literal if assigned in code.
    int64_t implicitValue = 0;              ///< Calculated integer value for implicit member hints.
    bool hasExplicitValue = false;          ///< True if an explicit value initializer is present.
};

/**
 * @brief Extracted enum declaration source range and member slice indices.
 */
struct IndexedEnumDeclaration
{
    SourceRange enumRange{}; ///< Source coordinate range of the enum declaration.
    uint32_t startByte = 0;  ///< Start byte offset of the enum declaration.
    uint32_t endByte = 0;    ///< End byte offset of the enum declaration.
    uint32_t firstValue = 0; ///< First index in InlayHintSites::enumValues.
    uint32_t valueCount = 0; ///< Number of enum members in InlayHintSites::enumValues.
};

/**
 * @brief Aggregated flat container of syntactic inlay hint sites across a document range.
 */
struct InlayHintSites
{
    std::vector<IndexedCallSite> calls;                      ///< Extracted call expressions.
    std::vector<IndexedCallArgument> callArguments;          ///< Flat vector of call arguments.
    std::vector<IndexedAutoDeclarator> autoVars;             ///< Extracted auto declarators.
    std::vector<IndexedConstructorInitDeclarator> ctorInits; ///< Direct constructor declarators.
    std::vector<IndexedLambdaSite> lambdas;                  ///< Extracted lambda expressions.
    std::vector<IndexedLambdaParam> lambdaParams;            ///< Flat vector of untyped lambda params.
    std::vector<IndexedEnumDeclaration> enumDeclarations;    ///< Extracted enum declarations.
    std::vector<IndexedEnumValue> enumValues;                ///< Flat vector of enum members.
};

/**
 * @brief Collects all syntactic inlay hint sites within [startLine, endLine] into flat record vectors.
 * @param[in] index Precomputed AST node index to traverse.
 * @param[in] startLine Inclusive 0-based start line.
 * @param[in] endLine Inclusive 0-based end line.
 * @param[in] sourceCode Document source text buffer for string views and literal parsing.
 * @return Aggregated flat container of syntactic inlay hint sites.
 */
InlayHintSites CollectInlayHintSites(const NodeIndex& index, uint32_t startLine, uint32_t endLine,
                                     std::string_view sourceCode);

} // namespace angel_lsp::analysis
