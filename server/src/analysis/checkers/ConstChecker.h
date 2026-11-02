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
 * @brief Checks that nothing written through a `const` object tries to change it.
 *
 * A use-site pass, like AccessChecker: the declaration rules ask whether `const` was spelled
 * somewhere it may not be, and this asks whether the code then respects it. Two things the
 * engine refuses, each verified against a real build before the rule was written:
 *
 * - assigning to something declared const, which it answers with "Expression is not an
 *   l-value" - the same sentence for a const global, a const local, and a field reached
 *   through a const reference;
 * - calling a non-const method through a const object, which it answers with "No matching
 *   signatures to 'Entity::Mutate() const'". The wording is the point: the engine is not
 *   refusing the call, it is looking for a const overload and not finding one. So an overload
 *   set with one const member in it is legal, and this pass only speaks when *no* declaration
 *   of that name is const.
 *
 * One thing that looks like it belongs here and does not: calling a non-const method from
 * inside a const one, through the implicit `this`. C++ rejects it and AngelScript compiles it -
 * checked, not assumed - so there is no rule, and `this` is never treated as const.
 *
 * The pass stays silent unless it can see everything the judgement rests on, which is the same
 * contract AccessChecker carries and for the same reason: an unresolved type is what an
 * engine-registered one looks like from here, and those carry members - and const overloads -
 * written down in no source this analyzer reads.
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
