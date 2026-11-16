#pragma once

#include "analysis/index/OperatorSiteIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/TemplateTypeResolver.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Context for resolving AST-free indexed expression types in Second Pass.
 */
struct ExprTypeEvalContext
{
    const Scope* scope = nullptr;
    const IndexedOperatorSites& sites;
    const SymbolTable& symbolTable;
    std::vector<std::string>& memo;
    std::vector<uint8_t>& state;
};

/**
 * @brief Winning overload symbol paired with its first-argument conversion rank.
 */
struct ResolvedOverloadMatch
{
    Symbol symbol;
    ArgumentConversion primaryConversion;
    bool hasMatch = false;
};

/**
 * @brief Parameters for resolving a member overload on a receiver type.
 */
struct MemberOverloadQuery
{
    std::string_view ownerType;
    std::string_view methodName;
    std::span<const std::string> argTypes;
    bool isReceiverConst = false;
};

/**
 * @brief Resolves the raw type string of an indexed expression node.
 * @param[in] exprId Expression index into ctx.sites.exprs.
 * @param[in,out] ctx Evaluation context and memoization table.
 * @return Resolved type string (empty when unknown).
 */
[[nodiscard]] std::string ResolveIndexedExprType(uint32_t exprId, ExprTypeEvalContext& ctx);

/**
 * @brief Checks if an expression resolves to a const-qualified variable or method result.
 * @param[in] exprId Expression index into ctx.sites.exprs.
 * @param[in] ctx Evaluation context.
 * @return True if expression is const-qualified.
 */
[[nodiscard]] bool IsIndexedExprConst(uint32_t exprId, const ExprTypeEvalContext& ctx);

/**
 * @brief Resolves the winning overload among candidate member methods for an owner type.
 * @param[in] query Member overload lookup parameters.
 * @param[in] symbolTable Workspace symbol table.
 * @return Winning Symbol and conversion rank, or hasMatch = false if none viable.
 */
[[nodiscard]] ResolvedOverloadMatch ResolveMemberOverload(const MemberOverloadQuery& query,
                                                          const SymbolTable& symbolTable);

/**
 * @brief Resolves a member overload called with exactly one argument, as binary and assignment operators do.
 * @param[in] head Owner, method name and receiver constness; its argTypes are ignored.
 * @param[in] argType Type of the single argument.
 * @param[in] symbolTable Symbol table searched for the owner's members.
 * @return Winning Symbol and conversion rank, or hasMatch = false if none viable.
 */
[[nodiscard]] ResolvedOverloadMatch ResolveSingleArgOverload(const MemberOverloadQuery& head, std::string_view argType,
                                                             const SymbolTable& symbolTable);

/**
 * @brief Substitutes template parameters in a return type using the receiver type's template arguments.
 * @param[in] returnType Raw return type from method signature.
 * @param[in] ownerType Normalized receiver type (may carry `<...>`).
 * @param[in] symbolTable Workspace symbol table.
 * @return Substituted return type string.
 */
[[nodiscard]] std::string SubstituteMemberReturnType(std::string_view returnType, std::string_view ownerType,
                                                     const SymbolTable& symbolTable);

/**
 * @brief Resolves the raw type string of an indexed identifier expression node.
 * @param[in] node Identifier expression node.
 * @param[in,out] ctx Evaluation context.
 * @return Resolved type string, or empty when unknown.
 */
[[nodiscard]] std::string ResolveIdentifierExprType(const IndexedExprNode& node, ExprTypeEvalContext& ctx);

/**
 * @brief Resolves the return type of an indexed call expression node.
 * @param[in] node Call expression node.
 * @param[in,out] ctx Evaluation context.
 * @return Resolved return type string, or empty when unknown.
 */
[[nodiscard]] std::string ResolveCallExprType(const IndexedExprNode& node, ExprTypeEvalContext& ctx);

/**
 * @brief Collects original and template-substituted overload candidate pairs.
 */
struct MemberCandidateBuffer
{
    std::vector<Symbol>& originals;
    std::vector<Symbol>& substituted;
    const TemplateBinding& binding;
    const SymbolTable& symbolTable;
};

/**
 * @brief Returns true if argCount satisfies the function signature's required and variadic arity bounds.
 * @param[in] sig Function signature to inspect.
 * @param[in] argCount Call-site argument count.
 * @return True if argument count is valid for the signature.
 */
[[nodiscard]] bool MatchesMethodArity(const FunctionSignature& sig, size_t argCount) noexcept;

/**
 * @brief Returns true if typeName names a declared class, interface, enum, typedef, or funcdef in symbolTable.
 * @param[in] typeName Candidate unqualified or qualified type name.
 * @param[in] symbolTable Workspace symbol table.
 * @return True if at least one matching type symbol exists.
 */
[[nodiscard]] bool IsDeclaredTypeSymbol(std::string_view typeName, const SymbolTable& symbolTable);
} // namespace angel_lsp::analysis
