#pragma once

#include "analysis/checkers/CallChecker.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/InitializerListFacts.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief True when a name also denotes a type, which makes `Name(...)` a construction.
 * @param[in] name Identifier name string view.
 * @param[in] table Symbol table.
 * @return True if the name denotes a type.
 */
bool NamesAType(std::string_view name, const SymbolTable& table);

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
bool IsConstructorOrDestructorName(std::string_view memberName, std::string_view typeName);

/**
 * @brief Declarations of a method, across a type's whole visible hierarchy.
 * @param[in] typeName Name of the class type.
 * @param[in] methodName Name of the method.
 * @param[in] table Symbol table.
 * @param[in,out] cache Optional expression type cache for memoizing type hierarchies.
 * @return Candidate symbols matching the method name.
 */
std::vector<Symbol> FindMethodCandidates(std::string_view typeName, std::string_view methodName,
                                         const SymbolTable& table, ExpressionTypeCache* cache = nullptr);

/**
 * @brief Collects enclosing namespace scopes up to global scope starting at byteOffset.
 * @param[in] byteOffset Document byte position of the call.
 * @param[in] sourceCode Document source text.
 * @param[in] nodeIndex Optional precomputed node index.
 * @return Ordered list of reachable namespace scopes.
 */
std::vector<std::string> CollectReachableScopes(uint32_t byteOffset, std::string_view sourceCode,
                                                const NodeIndex* nodeIndex = nullptr);

/**
 * @brief Declarations an unqualified call can reach, when that question is answerable.
 * @param[in] name Function identifier name.
 * @param[in] ctx Free candidate lookup context.
 * @return Reachable function candidate symbols.
 */
std::vector<Symbol> FindFreeCandidates(std::string_view name, const FreeLookupContext& ctx);

/**
 * @brief Parameters for validating lambda arguments against candidate callable signatures.
 */
struct LambdaCheckFactsContext
{
    SourceRange calleeRange;                     ///< Callee diagnostic source range.
    std::string_view reportedName;               ///< Callee name for error messages.
    const SymbolTable& table;                    ///< Symbol table for funcdef lookup.
    std::span<const CallArgumentFact> arguments; ///< Call argument facts.
    std::vector<size_t> lambdaPositions;         ///< Sliced argument indices of lambdas.
};

/**
 * @brief Finds indices of arguments that are lambda expressions.
 * @param[in] arguments List of argument facts.
 * @return 0-based indices where arguments are lambdas.
 */
std::vector<size_t> FindLambdaPositions(std::span<const CallArgumentFact> arguments);

/**
 * @brief Judges lambda arguments against the funcdef parameters they land on.
 * @param[in] candidates Candidate function overloads.
 * @param[in] lctx Lambda verification context.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckLambdaArguments(std::span<const Symbol* const> candidates, const LambdaCheckFactsContext& lctx,
                          DiagnosticContext& ctx);

/**
 * @brief Checks whether an object operand fact resolves to a const variable or property.
 * @param[in] objectOperand Binary operand fact for the object receiver.
 * @param[in] scope Lexical scope at call site.
 * @param[in] symbolTable Symbol table.
 * @return True if the object is const.
 */
bool IsObjectOperandConst(const BinaryOperandFact& objectOperand, const Scope* scope, const SymbolTable& symbolTable);

/**
 * @brief Applies template argument substitutions across candidate methods.
 * @param[in,out] candidates Candidate symbols to specialize.
 * @param[in] typeName Owner type name.
 * @param[in] fallbackArgs Fallback template arguments when generic binding is absent.
 * @param[in] table Symbol table.
 */
void ApplyTemplateSubstitutions(std::vector<Symbol>& candidates, std::string_view typeName,
                                std::span<const std::string> fallbackArgs, const SymbolTable& table);

/**
 * @brief Filters method candidates based on the constness of the receiver.
 * @param[in,out] candidates Candidate symbols to filter.
 * @param[in] isReceiverConst Whether the receiver object is const.
 */
void FilterMethodCandidatesConstness(std::vector<Symbol>& candidates, bool isReceiverConst);

/**
 * @brief Checks if a call site fact is enclosed within a const method declaration.
 * @param[in] site Call site fact.
 * @return True if enclosed within a const method.
 */
inline bool IsEnclosingMethodConst(const CallSiteFact& site) noexcept
{
    return site.isEnclosingMethodConst;
}

/**
 * @brief Attempts to resolve a member variable or property returning a funcdef handle.
 * @param[in] objectType Enclosing object type name.
 * @param[in] memberName Member identifier.
 * @param[in] table Symbol table.
 * @param[in,out] cache Optional type cache.
 * @return Optional synthesized function symbol.
 */
std::optional<Symbol> TryResolveMemberCallableFuncdef(std::string_view objectType, std::string_view memberName,
                                                      const SymbolTable& table, ExpressionTypeCache* cache = nullptr);

/**
 * @brief Input parameters for verifying whether a variable symbol is non-callable or a callable funcdef.
 */
struct NonCallableCheckRequest
{
    std::string_view name;     ///< Identifier name of the variable or member.
    std::string_view typeName; ///< Declared type name of the variable.
    SourceRange calleeRange;   ///< Source range where the call or identifier occurred.
};

/**
 * @brief Checks whether a member variable in a type hierarchy is non-callable or a callable funcdef.
 * @param[in] site Call site fact.
 * @param[in] objType Enclosing object type name view.
 * @param[in,out] ctx Diagnostic context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if variable was handled.
 */
bool CheckMemberNonCallableVariable(const CallSiteFact& site, std::string_view objType, DiagnosticContext& ctx,
                                    CalleeResolution& res);

/**
 * @brief Resolves candidates and reports invalid constructors for member calls from fact.
 * @param[in] site Call site fact.
 * @param[in] scope Lexical scope at call site.
 * @param[in,out] ctx Diagnostic context.
 * @return Resolved CalleeResolution.
 */
CalleeResolution ResolveMemberCallee(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Checks if a local symbol shadows a callable or is a non-callable variable.
 * @param[in] site Call site fact.
 * @param[in] scope Lexical scope at call site.
 * @param[in,out] ctx Diagnostic context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if handled as local non-callable.
 */
bool CheckLocalNonCallable(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx, CalleeResolution& res);

/**
 * @brief Checks if a scoped symbol refers to a non-callable variable.
 * @param[in] site Call site fact.
 * @param[in,out] ctx Diagnostic context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if handled as scoped non-callable variable.
 */
bool CheckScopedNonCallableVariable(const CallSiteFact& site, DiagnosticContext& ctx, CalleeResolution& res);

/**
 * @brief Checks if a global or reachable namespace symbol refers to a non-callable variable.
 * @param[in] site Call site fact.
 * @param[in,out] ctx Diagnostic context.
 * @param[in,out] res Callee resolution details to update.
 * @return True if handled as global non-callable variable.
 */
bool CheckGlobalNonCallableVariable(const CallSiteFact& site, DiagnosticContext& ctx, CalleeResolution& res);

/**
 * @brief Collects function candidates matching a scoped written name.
 * @param[in] written Scoped function name view.
 * @param[in] table Symbol table.
 * @return Vector of matching function symbols.
 */
std::vector<Symbol> CollectScopedFunctionCandidates(std::string_view written, const SymbolTable& table);

/**
 * @brief Validates argument ordering to prevent positional arguments after named ones.
 * @param[in] arguments Slice of argument facts.
 * @param[out] sawNamedArg True if at least one named argument was found.
 * @param[in,out] ctx Diagnostic context.
 * @return True if argument ordering is valid.
 */
bool ValidateArgumentOrdering(std::span<const CallArgumentFact> arguments, bool& sawNamedArg, DiagnosticContext& ctx);

/**
 * @brief Resolves expression types for each call argument fact and flags bare types.
 * @param[in] arguments Sliced call argument facts.
 * @param[in] scope Lexical scope at call site.
 * @param[in,out] ctx Diagnostic context.
 * @param[in] nodeIndex Optional document node index supplying the tree root.
 * @return Resolved CallResolutionContext structure.
 */
CallResolutionContext ResolveCallArguments(std::span<const CallArgumentFact> arguments, const Scope* scope,
                                           DiagnosticContext& ctx, const NodeIndex* nodeIndex = nullptr);

/**
 * @brief Checks initializer list arguments against parameter target types using facts.
 * @param[in] arguments Sliced call argument facts.
 * @param[in] args Resolved call arguments.
 * @param[in] fn Target function signature.
 * @param[in] initFacts Enclosing initializer list facts.
 * @param[in] scope Lexical scope at call site.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckInitializerListArgs(std::span<const CallArgumentFact> arguments, const CallResolutionContext& args,
                              const FunctionSignature& fn, const InitializerListFacts& initFacts, const Scope* scope,
                              DiagnosticContext& ctx);

/**
 * @brief Parameters for checking malformed ternary expressions in call arguments.
 */
struct MalformedTernaryCheckRequest
{
    std::span<const CallArgumentFact> arguments; ///< Call argument facts.
    std::span<const std::string> argTypes;       ///< Resolved argument types.
    std::span<const Symbol* const> candidates;   ///< Available matching candidates.
    const Scope* scope = nullptr;                ///< Lexical scope at call site.
};

/**
 * @brief Checks malformed ternary expressions used as call arguments.
 * @param[in] req Ternary check request parameters.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckMalformedTernaryArgs(const MalformedTernaryCheckRequest& req, DiagnosticContext& ctx);

/**
 * @brief Input parameters for reporting overload resolution failure diagnostics.
 */
struct OverloadFailureReportRequest
{
    std::span<const Symbol* const> candidates;   ///< Candidate function overloads.
    std::span<const CallArgumentFact> arguments; ///< Sliced argument facts.
    const CallResolutionContext& args;           ///< Resolved argument details.
    const CallSiteFact& site;                    ///< Call site fact.
};

/**
 * @brief Emits diagnostics when overload resolution finds no viable candidates.
 * @param[in] req Overload failure report request parameters.
 * @param[in,out] ctx Diagnostic context.
 */
void ReportOverloadResolutionFailure(const OverloadFailureReportRequest& req, DiagnosticContext& ctx);

/**
 * @brief Validates that arguments passed to &out parameters are assignable L-values.
 * @param[in] candidate Chosen function overload.
 * @param[in] arguments Sliced argument facts.
 * @param[in] scope Lexical scope at call site.
 * @param[in,out] ctx Diagnostic context.
 */
void ValidateOutArguments(const Symbol& candidate, std::span<const CallArgumentFact> arguments, const Scope* scope,
                          DiagnosticContext& ctx);

/**
 * @brief Input parameters for evaluating candidate function overloads against arguments.
 */
struct CallOverloadCheckRequest
{
    std::span<const Symbol* const> matchingArity;    ///< Candidates of matching arity.
    const CallResolutionContext& args;               ///< Resolved argument types.
    const CalleeResolution& calleeRes;               ///< Callee resolution details.
    const CallSiteFact& site;                        ///< Precomputed call site fact.
    std::span<const CallArgumentFact> arguments;     ///< Sliced call argument facts.
    const InitializerListFacts* initFacts = nullptr; ///< Precomputed initializer list facts.
    const Scope* scope = nullptr;                    ///< Lexical scope at call site.
};

/**
 * @brief Dispatches overload resolution using precomputed call site facts.
 * @param[in] request Overload checking request parameters.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckCallOverloads(const CallOverloadCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Input parameters for checking a single call site fact against language and engine rules.
 */
struct CallCheckFactRequest
{
    const CallSiteFact& site;                        ///< Precomputed call site fact.
    std::span<const CallArgumentFact> arguments;     ///< Precomputed argument facts for this call site.
    const InitializerListFacts* initFacts = nullptr; ///< Precomputed initializer list facts for the document.
    const AstCheckContext& request;                  ///< Analysis check context and document metadata.
    const Scope* scope = nullptr;                    ///< Lexical scope at call site position.
};

/**
 * @brief Checks if a byte span overlaps any syntax error range in the document.
 * @param[in] startByte Start of byte span.
 * @param[in] endByte End of byte span.
 * @param[in] nodeIndex Precomputed node index of the document.
 * @return True if [startByte, endByte) overlaps an error range.
 */
inline bool HasOverlappingError(uint32_t startByte, uint32_t endByte, const NodeIndex* nodeIndex) noexcept
{
    if (!nodeIndex || !nodeIndex->HasSyntaxErrors())
    {
        return false;
    }
    const auto errors = nodeIndex->ErrorRanges();
    auto it = std::lower_bound(errors.begin(), errors.end(), startByte,
                               [](const NodeIndex::ErrorRange& err, uint32_t val) { return err.endByte <= val; });
    return it != errors.end() && it->startByte < endByte;
}

/**
 * @brief Resolves callee candidates based on pre-extracted call site fact.
 * @param[in] site Call site fact.
 * @param[in] scope Lexical scope at call site.
 * @param[in,out] ctx Diagnostic context.
 * @return Resolved CalleeResolution.
 */
CalleeResolution ResolveCalleeCandidates(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Formats an attempted constructor call signature for error reporting.
 * @param[in] targetType Declared type name.
 * @param[in] argTypes Resolved argument types.
 * @return Formatted signature string, e.g. "Type(int, string)".
 */
std::string FormatSignatureAttempt(std::string_view targetType, std::span<const std::string> argTypes);

/**
 * @brief Looks up constructor declarations for a given type name in symbol table.
 * @param[in] baseName Cleaned type name.
 * @param[in] table Symbol table.
 * @return Candidate constructor symbols.
 */
std::vector<Symbol> LookupRawConstructors(std::string_view baseName, const SymbolTable& table);

/**
 * @brief Checks if a class declaration is visible in script source (not predefined).
 * @param[in] baseName Type name.
 * @param[in] table Symbol table.
 * @param[in] predefinedExt Predefined file extension.
 * @return True if class declaration is visible in script.
 */
bool IsClassDeclarationVisible(std::string_view baseName, const SymbolTable& table, std::string_view predefinedExt);

/**
 * @brief Specializes raw constructor candidates using template argument mappings.
 * @param[in] rawConstructors Unspecialized constructor symbols.
 * @param[in] tmplInfo Parsed template type information.
 * @param[in] baseName Base type name.
 * @param[in] table Symbol table.
 * @return Specialized constructor symbols.
 */
std::vector<Symbol> SpecializeConstructors(const std::vector<Symbol>& rawConstructors, const TemplateTypeInfo& tmplInfo,
                                           std::string_view baseName, const SymbolTable& table);

/**
 * @brief Verifies constructor candidate overloads against provided arguments.
 * @param[in] argumentListRange Diagnostic source range of argument list.
 * @param[in] candidates Candidate constructor symbols.
 * @param[in] argTypes Resolved argument types.
 * @param[in] vctx Variable initialization context.
 */
void CheckConstructorOverload(SourceRange argumentListRange, const std::vector<Symbol>& candidates,
                              const std::vector<std::string>& argTypes, const VarInitContext& vctx);

/**
 * @brief Checks direct variable initialization syntax against accessible constructors.
 * @param[in] init Direct initialization site fact.
 * @param[in] request Analysis call request.
 * @param[in] facts Enclosing call site facts containing arguments.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckVariableDirectInitialization(const DirectInitSiteFact& init, const AstCheckContext& request,
                                       const CallSiteFacts& facts, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
