#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/TypeSanitization.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks that every call passes a number of arguments some declaration can accept.
 *
 * The third use-site pass, beside AccessChecker and ConstChecker. AngelScript answers a call it
 * cannot match with "No matching signatures to 'Take(const int)'" - a failed overload lookup,
 * not a refusal of one signature - so this asks the same question the engine does: is there
 * *any* visible declaration of this name that could take this many arguments?
 *
 * Originally only the count, and this comment said so for a long time after it stopped being
 * true: argument *types* are checked too, through OverloadResolver's ScoreArgumentMatch, which
 * is what picks the best overload and what reports `as-err-no-implicit-conversion` when none of
 * them can take the argument. The paragraph below survives because the reasoning in it still
 * governs where the checking stops.
 *
 * Matching argument types would need the engine's conversion and promotion
 * rules applied across a whole overload set, which this analyzer does not do and which
 * TypeConversionChecker says as much about at its own boundary. The count is decidable from the
 * declarations alone, and it is what catches the mistake people actually make.
 *
 * Silent unless everything is visible, which is the contract the other two carry:
 *
 * - a callee that resolves to no declaration is an engine-registered function, and the corpus
 *   is mostly those;
 * - a call through an object whose type does not resolve, or whose base chain does not, could
 *   reach a declaration written down nowhere here;
 * - one candidate taking `...` accepts any count, so the question is answered and closed.
 */
class NodeIndex;

using CallCheckRequest = AstCheckContext;

/**
 * @brief Runs the call-argument pass over one document.
 * @param request Tree, source text and scope tree for the document.
 * @param ctx Diagnostic sink and the analysis request behind it.
 */
void CheckCallArguments(const CallCheckRequest& request, DiagnosticContext& ctx);

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
    const CallCheckRequest& request;
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
    const CallCheckRequest& request;
    DiagnosticContext& ctx;
};
} // namespace angel_lsp::analysis
