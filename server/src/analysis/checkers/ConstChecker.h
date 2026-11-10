#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ConstSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Identifies the enclosing method symbol, declaring class, and const-qualifier status during const checks.
 */
struct EnclosingMethodInfo
{
    bool inClass = false;
    bool isConstMethod = false;
    std::string_view className;
};

/**
 * @brief Runs the const-correctness pass over one document.
 * @param[in] request Tree, source text and scope tree for the document.
 * @param[in,out] ctx Diagnostic sink and the analysis request behind it.
 */
void CheckConstCorrectness(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief AST checker for const-correctness rules.
 */
struct ConstChecker
{
    static constexpr std::string_view Name = "ConstChecker";
    static bool IsEnabled([[maybe_unused]] const SemanticAnalysisRequest& request)
    {
        return true;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckConstCorrectness(ast, ctx);
    }
};

/**
 * @brief Determines if an assignment target refers to a member of 'this' in a const method.
 * @param[in] target Pre-extracted assignment target fact.
 * @param[in] facts Document-wide const site facts.
 * @param[in] ctx Expression type context containing symbols and source.
 * @param[out] outMemberName Name of the modified member.
 * @return True if target modifies 'this' or a member of 'this' in a const context.
 */
bool IsMemberOfThis(const ConstAssignmentTargetFact& target, const ConstSiteFacts& facts,
                    const ExpressionTypeContext& ctx, std::string_view& outMemberName);

/**
 * @brief Const qualification classification of an expression, member, or return type.
 */
enum class Constness
{
    Unknown, ///< Nothing could be established. Stay silent.
    Mutable,
    Const
};
} // namespace angel_lsp::analysis
