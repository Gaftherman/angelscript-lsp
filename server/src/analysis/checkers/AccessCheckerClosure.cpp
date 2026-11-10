#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a name is declared within the closure's local scope chain.
 *
 * @param[in] scope Current inner scope.
 * @param[in] enclosingClosure Enclosing closure scope.
 * @param[in] idText Name to check.
 * @return True if declared within closure.
 */
static bool IsDeclaredInClosure(const Scope* scope, const Scope* enclosingClosure, std::string_view idText)
{
    for (const Scope* s = scope; s != nullptr; s = s->parent)
    {
        for (const auto& def : s->definitions)
        {
            if (def.name == idText)
            {
                return true;
            }
        }
        if (s == enclosingClosure)
        {
            break;
        }
    }
    return false;
}

/**
 * @brief Checks if a name is declared as a local variable or parameter in outer non-class scopes.
 *
 * @param[in] enclosingClosure Enclosing closure scope.
 * @param[in] idText Name to check.
 * @return True if declared in an outer function scope.
 */
static bool IsOuterLocalOrParameter(const Scope* enclosingClosure, std::string_view idText)
{
    for (const Scope* s = enclosingClosure->parent; s != nullptr; s = s->parent)
    {
        if (s->kind == ScopeKind::Class || s->kind == ScopeKind::Namespace || s->kind == ScopeKind::Global)
        {
            break;
        }
        for (const auto& def : s->definitions)
        {
            if (def.name == idText &&
                (def.kind == LocalDefinitionKind::Variable || def.kind == LocalDefinitionKind::Parameter))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Finds the innermost class enclosing the closure containing node, if any.
 *
 * @param[in] node AST node inside the closure.
 * @param[in] sourceCode Document source text.
 * @param[in] nodeIndex Optional pre-indexed node index.
 * @return Name of the outer class enclosing the lambda, or empty if none.
 */
static std::string_view EnclosingClassOfLambda(TSNode node, std::string_view sourceCode, const NodeIndex* nodeIndex)
{
    if (ts_node_is_null(node) || !nodeIndex)
    {
        return {};
    }

    TSNode classNode = nodeIndex->FindEnclosingClassOrMixin(ts_node_start_byte(node));
    if (!ts_node_is_null(classNode))
    {
        TSNode nameNode = parser::GetChildByField(classNode, parser::GetGrammarSymbols().fieldName);
        if (!ts_node_is_null(nameNode))
        {
            return parser::NodeText(nameNode, sourceCode);
        }
    }
    return {};
}

/**
 * @brief Checks whether an identifier attempts forbidden outer variable capture in a closure.
 *
 * @param[in] node AST identifier node.
 * @param[in] scope Current lexical scope.
 * @param[in] idText Identifier name.
 * @param[in,out] ctx Diagnostic context.
 * @return True if an error diagnostic was emitted, false otherwise.
 */
bool CheckClosureDisallowedAccess(TSNode node, const Scope* scope, std::string_view idText, DiagnosticContext& ctx)
{
    const Scope* enclosingClosure = FindEnclosingClosure(scope);
    if (!enclosingClosure)
    {
        return false;
    }

    if (idText == "this" || idText == "super")
    {
        const TSPoint start = ts_node_start_point(node);
        const TSPoint end = ts_node_end_point(node);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::LambdaThisDisallowed);
        return true;
    }

    if (IsDeclaredInClosure(scope, enclosingClosure, idText))
    {
        return false;
    }

    if (IsOuterLocalOrParameter(enclosingClosure, idText))
    {
        const TSPoint start = ts_node_start_point(node);
        const TSPoint end = ts_node_end_point(node);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::LambdaClosureDisallowed,
                        idText);
        return true;
    }

    const std::string_view outerClass = EnclosingClassOfLambda(node, ctx.request.sourceCode, ctx.request.nodeIndex);
    if (!outerClass.empty())
    {
        const MemberLookupResult member =
            FindMember(std::string(outerClass), std::string(idText), ctx.request.symbolTable, false);
        if (member.found)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::LambdaMemberAccess,
                            idText);
            return true;
        }
    }

    return false;
}

} // namespace angel_lsp::analysis
