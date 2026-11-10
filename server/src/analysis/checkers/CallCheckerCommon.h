#pragma once

#include "analysis/checkers/CallChecker.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"

#include <ankerl/unordered_dense.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Trims leading and trailing whitespace in place.
 * @param[in,out] s String to trim.
 */
void TrimString(std::string& s);

/**
 * @brief Counts the arguments written between one call's parentheses.
 * @param[in] argumentList AST node representing the argument list.
 * @return Count of written arguments.
 */
uint32_t CountArguments(TSNode argumentList);

/**
 * @brief Extracts argument expression nodes from an argument list AST node.
 * @param[in] argumentList AST node representing the argument list.
 * @return Vector of AST nodes for each argument expression.
 */
std::vector<TSNode> GetArgumentNodes(TSNode argumentList);

/**
 * @brief Computes the diagnostic source range for a function or method call.
 * @param[in] callee AST node of the callee expression.
 * @return Precise source range of the callee.
 */
SourceRange GetCalleeDiagnosticRange(TSNode callee);

/**
 * @brief True when a name also denotes a type, which makes `Name(...)` a construction.
 * @param[in] name Identifier name.
 * @param[in] table Symbol table.
 * @return True if the name denotes a type.
 */
bool NamesAType(const std::string& name, const SymbolTable& table);

/**
 * @brief True when a symbol represents a function with a valid signature.
 * @param[in] sym Symbol to check.
 * @return True if the symbol is a function.
 */
bool IsFunctionSymbol(const Symbol& sym);

/**
 * @brief Evaluates whether candidate set accepts an argument count.
 * @param[in] candidates Available function candidates.
 * @param[in] argumentCount Number of arguments provided.
 * @return CandidateSet verdict.
 */
CandidateSet JudgeAgainst(const std::vector<Symbol>& candidates, uint32_t argumentCount);

/**
 * @brief True when a member name is its own class's constructor or destructor.
 * @param[in] memberName Member identifier text.
 * @param[in] typeName Owner type name.
 * @return True if name denotes constructor or destructor.
 */
bool IsConstructorOrDestructorName(const std::string& memberName, const std::string& typeName);

/**
 * @brief Declarations of a method, across a type's whole visible hierarchy.
 * @param[in] typeName Name of the class type.
 * @param[in] methodName Name of the method.
 * @param[in] table Symbol table.
 * @param[in] cache Optional expression type cache for memoizing type hierarchies.
 * @return Candidate symbols matching the method name.
 */
std::vector<Symbol> FindMethodCandidates(const std::string& typeName, const std::string& methodName,
                                         const SymbolTable& table, ExpressionTypeCache* cache = nullptr);

/**
 * @brief Collects enclosing namespace scopes up to global scope.
 * @param[in] callNode AST node of call expression.
 * @param[in] sourceCode Document source text.
 * @param[in] nodeIndex Optional precomputed node index.
 * @return Ordered list of reachable namespace scopes.
 */
std::vector<std::string> CollectReachableScopes(TSNode callNode, std::string_view sourceCode,
                                                const NodeIndex* nodeIndex = nullptr);

/**
 * @brief Declarations an unqualified call can reach, when that question is answerable.
 * @param[in] name Function identifier name.
 * @param[in] ctx Free candidate lookup context.
 * @return Reachable function candidate symbols.
 */
std::vector<Symbol> FindFreeCandidates(const std::string& name, const FreeLookupContext& ctx);

/**
 * @brief Finds indices of arguments that are lambda expressions.
 * @param[in] argNodes List of argument AST nodes.
 * @return 0-based indices where arguments are lambdas.
 */
std::vector<size_t> FindLambdaPositions(const std::vector<TSNode>& argNodes);

/**
 * @brief Judges a lambda argument against the funcdef parameter it lands on.
 * @param[in] candidates Candidate function overloads.
 * @param[in] lctx Lambda verification context.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckLambdaArguments(std::span<const Symbol* const> candidates, const LambdaCheckContext& lctx,
                          DiagnosticContext& ctx);

/**
 * @brief Checks whether an object expression node resolves to a const variable or property.
 * @param[in] objectNode AST node of object expression.
 * @param[in] valCtx Call validation context.
 * @return True if the object node is const.
 */
bool IsObjectNodeConst(TSNode objectNode, const CallValidationContext& valCtx);

/**
 * @brief Applies template argument substitutions across candidate methods.
 * @param[in,out] candidates Candidate symbols to specialize.
 * @param[in] typeName Owner type name.
 * @param[in] fallbackArgs Fallback template arguments when generic binding is absent.
 * @param[in] table Symbol table.
 */
void ApplyTemplateSubstitutions(std::vector<Symbol>& candidates, const std::string& typeName,
                                const std::vector<std::string>& fallbackArgs, const SymbolTable& table);

/**
 * @brief Filters method candidates based on the constness of the receiver.
 * @param[in,out] candidates Candidate symbols to filter.
 * @param[in] isReceiverConst Whether the receiver object is const.
 */
void FilterMethodCandidatesConstness(std::vector<Symbol>& candidates, bool isReceiverConst);

/**
 * @brief Checks if a node is enclosed within a const method declaration.
 * @param[in] node AST node.
 * @return True if enclosed within a const method.
 */
bool IsEnclosingMethodConst(TSNode node);

/**
 * @brief Attempts to resolve a member variable or property returning a funcdef handle.
 * @param[in] objectType Enclosing object type name.
 * @param[in] memberName Member identifier.
 * @param[in] table Symbol table.
 * @param[in,out] cache Optional type cache.
 * @return Optional synthesized function symbol.
 */
std::optional<Symbol> TryResolveMemberCallableFuncdef(const std::string& objectType, const std::string& memberName,
                                                      const SymbolTable& table, ExpressionTypeCache* cache = nullptr);

/**
 * @brief Checks whether a member variable in a type hierarchy is non-callable or a callable funcdef.
 * @param[in] objType Enclosing object type name.
 * @param[in] memberName Member identifier.
 * @param[in] valCtx Call validation context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if variable was handled.
 */
bool CheckMemberNonCallableVariable(const std::string& objType, const std::string& memberName,
                                    const CallValidationContext& valCtx, CalleeResolution& res);

/**
 * @brief Resolves candidates and reports invalid constructors for member calls.
 * @param[in] valCtx Call validation context.
 * @return Resolved CalleeResolution.
 */
CalleeResolution ResolveMemberCallee(const CallValidationContext& valCtx);

/**
 * @brief Checks if a local symbol shadows a callable or is a non-callable variable.
 * @param[in] shortName Unqualified identifier.
 * @param[in] valCtx Call validation context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if handled as local non-callable.
 */
bool CheckLocalNonCallable(const std::string& shortName, const CallValidationContext& valCtx, CalleeResolution& res);

/**
 * @brief Checks if a scoped symbol refers to a non-callable variable.
 * @param[in] written Full scoped identifier text.
 * @param[in] shortName Unqualified identifier.
 * @param[in] valCtx Call validation context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if handled as scoped non-callable variable.
 */
bool CheckScopedNonCallableVariable(std::string_view written, const std::string& shortName,
                                    const CallValidationContext& valCtx, CalleeResolution& res);

/**
 * @brief Checks if a global or reachable namespace symbol refers to a non-callable variable.
 * @param[in] written Full identifier text.
 * @param[in] shortName Unqualified identifier.
 * @param[in] valCtx Call validation context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if handled as global non-callable variable.
 */
bool CheckGlobalNonCallableVariable(std::string_view written, const std::string& shortName,
                                    const CallValidationContext& valCtx, CalleeResolution& res);

/**
 * @brief Collects function candidates matching a scoped written name.
 * @param[in] written Scoped function name.
 * @param[in] table Symbol table.
 * @return Vector of matching function symbols.
 */
std::vector<Symbol> CollectScopedFunctionCandidates(std::string_view written, const SymbolTable& table);

/**
 * @brief Validates argument ordering to prevent positional arguments after named ones.
 * @param[in] arguments AST arguments node.
 * @param[out] sawNamedArg True if at least one named argument was found.
 * @param[in,out] ctx Diagnostic context.
 * @return True if argument ordering is valid.
 */
bool ValidateArgumentOrdering(TSNode arguments, bool& sawNamedArg, DiagnosticContext& ctx);

/**
 * @brief Unwraps a parenthesized expression AST node to its inner child.
 * @param[in] node Input AST node.
 * @return Inner node if parenthesized, otherwise the node itself.
 */
TSNode UnwrapParenNode(TSNode node);

/**
 * @brief Resolves expression types for each call argument and flags bare types.
 * @param[in] valCtx Call validation context.
 * @return Resolved CallResolutionContext structure.
 */
CallResolutionContext ResolveCallArguments(const CallValidationContext& valCtx);

/**
 * @brief Checks initializer list arguments against parameter target types.
 * @param[in] args Resolved call arguments.
 * @param[in] fn Target function signature.
 * @param[in] valCtx Call validation context.
 */
void CheckInitializerListArgs(const CallResolutionContext& args, const FunctionSignature& fn,
                              const CallValidationContext& valCtx);

/**
 * @brief Checks malformed ternary expressions used as call arguments.
 * @param[in] argNodes Argument AST nodes.
 * @param[in] argTypes Resolved argument types.
 * @param[in] candidates Available matching candidates.
 * @param[in] valCtx Call validation context.
 */
void CheckMalformedTernaryArgs(const std::vector<TSNode>& argNodes, const std::vector<std::string>& argTypes,
                               std::span<const Symbol* const> candidates, const CallValidationContext& valCtx);

/**
 * @brief Emits diagnostics when overload resolution finds no viable candidates.
 * @param[in] candidates Candidate functions.
 * @param[in] args Call arguments info.
 * @param[in] reportedName Reported callee name.
 * @param[in] valCtx Call validation context.
 */
void ReportOverloadResolutionFailure(std::span<const Symbol* const> candidates, const CallResolutionContext& args,
                                     const std::string& reportedName, const CallValidationContext& valCtx);

/**
 * @brief Validates that arguments passed to &out parameters are assignable L-values.
 * @param[in] candidate Chosen function overload.
 * @param[in] argNodes Call argument AST nodes.
 * @param[in] valCtx Call validation context.
 */
void ValidateOutArguments(const Symbol& candidate, const std::vector<TSNode>& argNodes,
                          const CallValidationContext& valCtx);

/**
 * @brief Dispatches overload resolution and checks ambiguity, conversions, and out parameters.
 * @param[in] matchingArity Candidates of matching arity.
 * @param[in] args Resolved argument types.
 * @param[in] calleeRes Callee resolution details.
 * @param[in] valCtx Call validation context.
 */
void CheckCallOverloads(std::span<const Symbol* const> matchingArity, const CallResolutionContext& args,
                        const CalleeResolution& calleeRes, const CallValidationContext& valCtx);

/**
 * @brief Resolves callee candidates based on node type.
 * @param[in] valCtx Call validation context.
 * @return Resolved CalleeResolution.
 */
CalleeResolution ResolveCalleeCandidates(const CallValidationContext& valCtx);

/**
 * @brief Formats an attempted constructor call signature for error reporting.
 * @param[in] targetType Declared type name.
 * @param[in] argTypes Resolved argument types.
 * @return Formatted signature string, e.g. "Type(int, string)".
 */
std::string FormatSignatureAttempt(const std::string& targetType, const std::vector<std::string>& argTypes);

/**
 * @brief Looks up constructor declarations for a given type name in symbol table.
 * @param[in] baseName Cleaned type name.
 * @param[in] table Symbol table.
 * @return Candidate constructor symbols.
 */
std::vector<Symbol> LookupRawConstructors(const std::string& baseName, const SymbolTable& table);

/**
 * @brief Checks if a class declaration is visible in script source (not predefined).
 * @param[in] baseName Type name.
 * @param[in] table Symbol table.
 * @param[in] predefinedExt Predefined file extension.
 * @return True if class declaration is visible in script.
 */
bool IsClassDeclarationVisible(const std::string& baseName, const SymbolTable& table, const std::string& predefinedExt);

/**
 * @brief Specializes raw constructor candidates using template argument mappings.
 * @param[in] rawConstructors Unspecialized constructor symbols.
 * @param[in] tmplInfo Parsed template type information.
 * @param[in] baseName Base type name.
 * @param[in] table Symbol table.
 * @return Specialized constructor symbols.
 */
std::vector<Symbol> SpecializeConstructors(const std::vector<Symbol>& rawConstructors, const TemplateTypeInfo& tmplInfo,
                                           const std::string& baseName, const SymbolTable& table);

/**
 * @brief Verifies constructor candidate overloads against provided arguments.
 * @param[in] argListNode Argument list AST node.
 * @param[in] candidates Candidate constructor symbols.
 * @param[in] argTypes Resolved argument types.
 * @param[in] vctx Variable initialization context.
 */
void CheckConstructorOverload(TSNode argListNode, const std::vector<Symbol>& candidates,
                              const std::vector<std::string>& argTypes, const VarInitContext& vctx);

/**
 * @brief Checks direct variable initialization syntax against accessible constructors.
 * @param[in] varDeclNode AST variable declaration node.
 * @param[in] request Analysis call request.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckVariableDirectInitialization(TSNode varDeclNode, const AstCheckContext& request, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
