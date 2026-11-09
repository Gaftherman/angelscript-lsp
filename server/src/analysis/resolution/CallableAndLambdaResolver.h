#pragma once

#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"

#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

struct CallArgumentInfo
{
    std::string name;
    TSNode exprNode{};
    TSNode nameNode{};
    uint32_t index = 0;
};

struct CallNodes
{
    TSNode callNode{};
    TSNode argListNode{};
};

struct CallResolutionContext
{
    std::vector<TSNode> argNodes;
    std::vector<std::string> argTypes;
    std::vector<std::string> argNames;
    std::vector<bool> argIsLValue;
    bool allArgsResolved = true;
};

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

struct LambdaParameter
{
    std::string typeName;
    ParameterModifier modifier = ParameterModifier::None;
    bool hasWrittenType : 1 = false;
    bool isConst : 1 = false;
    bool isHandle : 1 = false;
    bool isReference : 1 = false;
};

struct FuncdefAgreement
{
    std::optional<Symbol> agreed;
    bool sawCandidate = false;
    bool conflict = false;
};

TSNode ResolveArgumentListNode(TSNode node);
std::vector<CallArgumentInfo> ExtractCallArguments(TSNode argumentList, std::string_view sourceCode);
size_t CountCallArguments(TSNode argumentList);
std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const ExpressionTypeContext& ctx);
std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, TSNode contextNode,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable);
uint32_t CalculateActiveCallParameter(TSNode argListNode, size_t cursorByte, std::string_view sourceCode);
std::vector<LambdaParamASTInfo> ExtractLambdaParameters(TSNode lambdaParamList, std::string_view sourceCode,
                                                        const NodeIndex* index = nullptr);
std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode);
bool LambdaContradictsFuncdef(const std::vector<LambdaParameter>& lambdaParameters, const FuncdefSignature& funcdefSig,
                              const SymbolTable& table);
bool LambdaContradictsFuncdef(TSNode lambdaNode, const FuncdefSignature& funcdefSig, const SymbolTable& table,
                              std::string_view sourceCode);
[[nodiscard]] bool IsLambdaExpression(TSNode node) noexcept;
std::optional<Symbol> FindFuncdefSymbol(std::string_view typeName, const SymbolTable& table);
[[nodiscard]] bool IsHandleType(std::string_view typeName, const SymbolTable& symbolTable) noexcept;

/**
 * @brief Resolves the funcdef that a lambda expression is converted to, from its indexed context.
 * @param[in] target Declaration, assignment or call context read by ReadLambdaTarget.
 * @param[in] table Symbol table searched for funcdefs, variables and callee overloads.
 * @return Funcdef symbol, or std::nullopt when the context names none or the callee candidates disagree.
 */
std::optional<Symbol> FuncdefTargetOfLambda(const LambdaTargetFact& target, const SymbolTable& table);
std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode, const SymbolTable& table, std::string_view sourceCode);
std::string InferLambdaParamType(TSNode nodeInLambda, std::string_view paramName, const SymbolTable& symbolTable,
                                 std::string_view sourceCode);
Symbol FuncdefToFunctionSymbol(const Symbol& funcdefSym, std::string_view callName);
std::optional<Symbol> TryResolveCallableFuncdef(std::string_view name, const Scope* scope, const SymbolTable& table,
                                                uint32_t refByte = UINT32_MAX);

bool IsListConstructorSignature(const FunctionSignature& fn);
bool IsListConstructor(const Symbol& sym);
/// Tests whether an expression denotes a function or method without invoking it.
[[nodiscard]] bool IsFunctionReference(TSNode node, const ExpressionTypeContext& ctx);
} // namespace angel_lsp::analysis
