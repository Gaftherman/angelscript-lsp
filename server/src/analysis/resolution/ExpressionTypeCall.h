#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/ExpressionTypeCache.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Parameters for resolving member method call returns.
 */
struct MemberCallResolutionRequest
{
    std::string_view objType;
    std::string_view memName;
    std::string_view fallbackObjName;
    std::string_view arrayTypeName;
};

/**
 * @brief Appends call candidates from mixins included in a class.
 * @param[in] containerName Enclosing container name.
 * @param[in] funcName Function name being called.
 * @param[in] symbolTable Symbol table to query.
 * @param[in,out] candidates Accumulating candidate symbols.
 */
void CollectMixinCallCandidates(std::string_view containerName, std::string_view funcName,
                                const SymbolTable& symbolTable, std::vector<Symbol>& candidates);

/**
 * @brief Collects candidate function symbols from enclosing class/interface hierarchy.
 * @param[in] containers Enclosing container declarations.
 * @param[in] funcName Function name being called.
 * @param[in] symbolTable Symbol table to query.
 * @param[in,out] cache Optional expression type cache.
 * @return Vector of candidate function symbols.
 */
[[nodiscard]] std::vector<Symbol> CollectEnclosingCallCandidates(std::span<const ContainerInfo> containers,
                                                                 std::string_view funcName,
                                                                 const SymbolTable& symbolTable,
                                                                 ExpressionTypeCache* cache);

/**
 * @brief Resolves return type from call candidates given argument types.
 * @param[in] candidates Function overload candidates.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved return type, or empty string.
 */
[[nodiscard]] std::string ResolveCallOverloadReturn(std::span<const Symbol> candidates,
                                                    std::span<const std::string_view> argTypes,
                                                    const SymbolTable& symbolTable);

[[nodiscard]] std::string ResolveCallOverloadReturn(std::span<const Symbol> candidates,
                                                    const std::vector<std::string>& argTypes,
                                                    const SymbolTable& symbolTable);

/**
 * @brief Collects function symbols from scope and global table matching funcName.
 * @param[in] funcName Target function name.
 * @param[in] inScopeSymbols Symbols visible in current lexical scope.
 * @param[in] symbolTable Symbol table to query.
 * @param[in,out] candidates Accumulating candidate symbols.
 */
void CollectScopeAndGlobalFunctions(std::string_view funcName, std::span<const Symbol> inScopeSymbols,
                                    const SymbolTable& symbolTable, std::vector<Symbol>& candidates);

/**
 * @brief Checks if funcName refers to a class or interface type (for constructor-like bare calls).
 * @param[in] funcName Candidate type name.
 * @param[in] symbolTable Symbol table to query.
 * @return Matched type name, or empty string.
 */
[[nodiscard]] std::string FindClassOrInterfaceName(std::string_view funcName, const SymbolTable& symbolTable);

/**
 * @brief Resolves member method call return type.
 * @param[in] req Member call parameters.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved return type, or empty string.
 */
[[nodiscard]] std::string ResolveMemberCallReturn(const MemberCallResolutionRequest& req,
                                                  std::span<const std::string_view> argTypes,
                                                  const SymbolTable& symbolTable);

[[nodiscard]] std::string ResolveMemberCallReturn(const MemberCallResolutionRequest& req,
                                                  const std::vector<std::string>& argTypes,
                                                  const SymbolTable& symbolTable);

/**
 * @brief Resolves return type from a callee funcdef symbol if applicable.
 * @param[in] calleeType Type of the callee expression.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved funcdef return type, or original calleeType.
 */
[[nodiscard]] std::string ResolveCalleeFuncdefReturn(std::string_view calleeType, const SymbolTable& symbolTable);

} // namespace angel_lsp::analysis
