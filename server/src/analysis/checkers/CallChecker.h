#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/InitializerListFacts.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/TransparentHash.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Runs the call-argument pass over one document.
 * @param[in] request Tree, source text and scope tree for the document.
 * @param[in,out] ctx Diagnostic sink and the analysis request behind it.
 */
void CheckCallArguments(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief Runs the call-argument pass over precomputed call site facts.
 * @param[in] facts Precomputed call site facts for the document.
 * @param[in] request Tree, source text and scope tree for the document.
 * @param[in,out] ctx Diagnostic sink and the analysis request behind it.
 */
void CheckCallArguments(const CallSiteFacts& facts, const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief Runs the call-argument pass over precomputed call site and initializer list facts.
 * @param[in] facts Precomputed call site facts for the document.
 * @param[in] initFacts Precomputed initializer list facts for the document.
 * @param[in] request Tree, source text and scope tree for the document.
 * @param[in,out] ctx Diagnostic sink and the analysis request behind it.
 */
void CheckCallArguments(const CallSiteFacts& facts, const InitializerListFacts& initFacts,
                        const AstCheckContext& request, DiagnosticContext& ctx);

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
    uint32_t byteOffset = 0;
    std::string_view sourceCode;
    std::string_view fileUri;
    std::string_view predefinedExtension;
    const SymbolTable& table;
    const rules::RuleIndex& index;
    const TransparentStringSet& moduleFileUris;
    const NodeIndex* nodeIndex = nullptr;
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
 * @brief Result of resolving a callee expression to a function signature or variable target.
 */
struct CalleeResolution
{
    std::vector<Symbol> candidates;
    std::string_view reportedName;
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
    std::string_view declaredType;
    std::string_view baseName;
    TemplateTypeInfo tmplInfo;
    const AstCheckContext& request;
    DiagnosticContext& ctx;
};

} // namespace angel_lsp::analysis
