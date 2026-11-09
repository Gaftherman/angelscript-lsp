#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**

 * @brief Runs the call-argument pass over one document.
 * @param request Tree, source text and scope tree for the document.
 * @param ctx Diagnostic sink and the analysis request behind it.
 */
void CheckCallArguments(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief AST checker for function and method call argument count and type matching.
 */
struct CallChecker
{
    static constexpr std::string_view Name = "CallChecker";
    static bool IsEnabled([[maybe_unused]] const SemanticAnalysisRequest& request)
    {
        return true;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckCallArguments(ast, ctx);
    }
};

/**
 * @brief Filters candidate symbols to those whose arity can accept argumentCount.
 * @param[in] candidates Candidate symbols to inspect.
 * @param[in] argumentCount Argument count supplied.
 * @return Vector of non-owning pointers to matching arity symbols.
 */
std::vector<const Symbol*> FilterMatchingArityCandidates(const std::vector<Symbol>& candidates, uint32_t argumentCount);

/**
 * @brief Tracks overload candidate matches and decision flags during function call argument validation.
 */
struct CandidateSet
{
    bool decided = false; ///< False means stay silent: nothing visible to judge against.
    bool accepts = false; ///< True when some candidate takes the written argument count.
};

/**
 * @brief Carries free function lookup parameters, symbol tables, and rule indices across module scopes.
 */
struct FreeLookupContext
{
    TSNode callNode;
    std::string_view sourceCode;
    std::string fileUri;
    std::string predefinedExtension;
    const SymbolTable& table;
    const rules::RuleIndex& index;
    const ankerl::unordered_dense::set<std::string>& moduleFileUris;
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Parameters for validating lambda and functor arguments against target callable signatures.
 */
struct LambdaCheckContext
{
    TSNode callee;
    TSNode arguments;
    std::string reportedName;
    const SymbolTable& table;
    std::string_view sourceCode;
    const std::vector<TSNode>& argNodes;
    std::vector<size_t> lambdaPositions;
};

/**
 * @brief Candidate status classification for lambda overload selection.
 */
enum class LambdaCandidateStatus
{
    Accepted,
    Rejected,
    UnresolvableFuncdef
};

/**
 * @brief Encapsulates call expression node, resolved callee symbol, and argument list during call checking.
 */
struct CallValidationContext
{
    TSNode callNode;
    TSNode callee;
    TSNode arguments;
    const AstCheckContext& request;
    const Scope* scope;
    DiagnosticContext& ctx;
};

/**
 * @brief Result of resolving a callee expression to a function signature or variable target.
 */
struct CalleeResolution
{
    std::vector<Symbol> candidates;
    std::string reportedName;
    bool candidatesAreFreeFunctions = false;
    bool isUnqualifiedClassCall = false;
    bool isReceiverConst = false;
    bool shouldCheck = true;
};

/**
 * @brief Extracted receiver object type name, qualifiers, and container scope metadata.
 */
struct ObjectTypeInfo
{
    std::string objectType;
    std::vector<std::string> templateArgs;
    TypeQualifiers qualifiers;
};

/**
 * @brief Tracks variable initialization expressions and type conversion compatibility during variable checking.
 */
struct VarInitContext
{
    std::string declaredType;
    std::string baseName;
    TemplateTypeInfo tmplInfo;
    const AstCheckContext& request;
    DiagnosticContext& ctx;
};
} // namespace angel_lsp::analysis
