#pragma once

#include "analysis/index/CallArgumentFacts.h"
#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

struct CallResolutionContext
{
    std::vector<std::string> argTypes;
    std::vector<std::string> argNames;
    std::vector<bool> argIsLValue;
    bool allArgsResolved = true;
};

struct FuncdefAgreement
{
    std::optional<Symbol> agreed;
    bool sawCandidate = false;
    bool conflict = false;
};

std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const ExpressionTypeContext& ctx);
std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, uint32_t byteOffset,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable,
                                                 const NodeIndex* nodeIndex = nullptr);
std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, TSNode contextNode,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable);
bool LambdaContradictsFuncdef(const std::vector<LambdaParameter>& lambdaParameters, const FuncdefSignature& funcdefSig,
                              const SymbolTable& table);
bool LambdaContradictsFuncdef(TSNode lambdaNode, const FuncdefSignature& funcdefSig, const SymbolTable& table,
                              std::string_view sourceCode);
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
