#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
struct ExpressionTypeContext;

/**
 * @brief Semantic AST checker verifying that mutations and method invocations respect const qualifiers.
 * @note Rejects assignments to const identifiers and non-const method calls on const receivers.
 */
using ConstCheckRequest = AstCheckContext;

/**
 * @brief Identifies the enclosing method symbol, declaring class, and const-qualifier status during const checks.
 */
struct EnclosingMethodInfo
{
    bool inClass = false;
    bool isConstMethod = false;
    std::string className;
};

/**
 * @brief Runs the const-correctness pass over one document.
 * @param[in] request Tree, source text and scope tree for the document.
 * @param[in,out] ctx Diagnostic sink and the analysis request behind it.
 */
void CheckConstCorrectness(const ConstCheckRequest& request, DiagnosticContext& ctx);

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
 * @param[in] target Target AST node of the assignment.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Expression type context containing symbols and source.
 * @param[out] outMemberName Name of the modified member.
 * @return True if target modifies 'this' or a member of 'this' in a const context.
 */
bool IsMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ExpressionTypeContext& ctx,
                    std::string& outMemberName);

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
