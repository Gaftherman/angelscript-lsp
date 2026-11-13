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

std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, uint32_t byteOffset,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable,
                                                 const NodeIndex* nodeIndex = nullptr);
bool LambdaContradictsFuncdef(const std::vector<LambdaParameter>& lambdaParameters, const FuncdefSignature& funcdefSig,
                              const SymbolTable& table);
std::optional<Symbol> FindFuncdefSymbol(std::string_view typeName, const SymbolTable& table);
[[nodiscard]] bool IsHandleType(std::string_view typeName, const SymbolTable& symbolTable) noexcept;

/**
 * @brief Resolves the funcdef that a lambda expression is converted to, from its indexed context.
 * @param[in] target Declaration, assignment or call context read by ReadLambdaTarget.
 * @param[in] table Symbol table searched for funcdefs, variables and callee overloads.
 * @return Funcdef symbol, or std::nullopt when the context names none or the callee candidates disagree.
 */
std::optional<Symbol> FuncdefTargetOfLambda(const LambdaTargetFact& target, const SymbolTable& table);

/**
 * @brief Deduces the type of an untyped lambda parameter from the funcdef the lambda converts to.
 * @param[in] paramLine Zero-based line of the parameter name.
 * @param[in] paramCharacter Zero-based column of the parameter name.
 * @param[in] lambdas Lambda facts of the document, as built by BuildLambdaSiteFacts.
 * @param[in] table Symbol table searched for the target funcdef.
 * @return Declared type of the matching funcdef parameter, or an empty string when the parameter is typed,
 *         belongs to no indexed lambda, or the lambda has no funcdef target.
 */
[[nodiscard]] std::string DeduceLambdaParameterType(uint32_t paramLine, uint32_t paramCharacter,
                                                    const std::vector<LambdaSiteFact>& lambdas,
                                                    const SymbolTable& table);
Symbol FuncdefToFunctionSymbol(const Symbol& funcdefSym, std::string_view callName);
std::optional<Symbol> TryResolveCallableFuncdef(std::string_view name, const Scope* scope, const SymbolTable& table,
                                                uint32_t refByte = UINT32_MAX);

bool IsListConstructorSignature(const FunctionSignature& fn);
bool IsListConstructor(const Symbol& sym);
} // namespace angel_lsp::analysis
