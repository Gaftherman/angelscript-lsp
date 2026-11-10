#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"

#include <cctype>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if an AST node is a type reference or comment node.
 *
 * @param[in] sym Grammar symbol ID.
 * @return True if node is a type reference or comment.
 */
static bool IsTypeRefOrComment(TSSymbol sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    return sym == gs.symDatatype || sym == gs.symPrimitiveType || sym == gs.symBaseClassList || sym == gs.symComment;
}

/**
 * @brief Checks whether an identifier AST node should be skipped during access validation.
 *
 * @param[in] node AST node to check.
 * @param[in] parent Parent AST node.
 * @return True if the node is a declaration, type reference or irrelevant context.
 */
static bool ShouldSkipIdentifierNode(TSNode node, TSNode parent)
{
    if (ts_node_is_null(parent))
    {
        return true;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symMemberExpression)
    {
        TSNode memberField = parser::GetChildByField(parent, gs.fieldMember);
        return ts_node_eq(node, memberField);
    }

    if (parser::IsDeclarationSymbol(parentSym))
    {
        TSNode nameField = parser::GetChildByField(parent, gs.fieldName);
        return ts_node_eq(node, nameField);
    }

    if (IsTypeRefOrComment(parentSym))
    {
        return true;
    }

    return ts_node_symbol(node) == gs.symIdentifier && parentSym == gs.symScopedIdentifier;
}

/**
 * @brief Extracts trimmed text view from an identifier AST node without heap allocation.
 *
 * @param[in] node AST node of the identifier.
 * @param[in] sourceCode Document source text.
 * @return Clean trimmed identifier string_view.
 */
static std::string_view ExtractCleanIdentifier(TSNode node, std::string_view sourceCode)
{
    std::string_view idText = parser::NodeText(node, sourceCode);
    while (!idText.empty() && isspace(static_cast<unsigned char>(idText.front())))
    {
        idText.remove_prefix(1);
    }
    while (!idText.empty() && isspace(static_cast<unsigned char>(idText.back())))
    {
        idText.remove_suffix(1);
    }
    return idText;
}

/**
 * @brief Checks if the lexical scope chain includes a class scope.
 *
 * @param[in] scope Lexical scope to inspect.
 * @return True if scope is null (unknown) or has an enclosing ScopeKind::Class.
 */
static bool HasEnclosingClassScope(const Scope* scope)
{
    if (!scope)
    {
        return true;
    }
    for (const Scope* s = scope; s != nullptr; s = s->parent)
    {
        if (s->kind == ScopeKind::Class)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Validates implicit member access on an identifier within an enclosing class.
 *
 * @param[in] node AST identifier node.
 * @param[in] scope Current lexical scope.
 * @param[in] idText Identifier name.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckImplicitMemberAccess(TSNode node, const Scope* scope, std::string_view idText, DiagnosticContext& ctx)
{
    if (!HasEnclosingClassScope(scope))
    {
        return;
    }

    const LocalDefinition* localDef = ResolveInScopeAt(scope, idText, ts_node_start_byte(node));
    if (localDef &&
        (localDef->kind == LocalDefinitionKind::Variable || localDef->kind == LocalDefinitionKind::Parameter))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;

    bool insideMixin = false;
    const std::string_view accessingClass =
        EnclosingClass(node, ctx.request.sourceCode, insideMixin, ctx.request.nodeIndex);
    if (accessingClass.empty())
    {
        return;
    }

    const MemberLookupResult member =
        FindMember(std::string(accessingClass), std::string(idText), table, ctx.request.RequiresAccessorKeyword());
    if (!member.decided)
    {
        return;
    }

    const bool declaredPrivate = member.access == AccessModifier::Private;
    if (declaredPrivate && !ctx.request.TreatsPrivateAsProtected())
    {
        if (insideMixin &&
            (IsSameAccessType(accessingClass, member.declaringClass) || member.declaringClass == accessingClass))
        {
            return;
        }
        if (!IsSameAccessType(accessingClass, member.declaringClass))
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-private-member-access", idText,
                            member.declaringClass);
        }
    }
}

void CheckIdentifierNode(TSNode node, const AstCheckContext& request, DiagnosticContext& ctx)
{
    TSNode parent = parser::GetParent(node);
    if (ShouldSkipIdentifierNode(node, parent))
    {
        return;
    }

    const std::string_view idText = ExtractCleanIdentifier(node, request.sourceCode);
    if (idText == "this" || idText == "super")
    {
        const TSPoint start = ts_node_start_point(node);
        CheckClosureDisallowedAccess(node, FindInnermostScope(request.scopeRoot, start.row, start.column), idText, ctx);
        return;
    }
    if (idText.empty() || IsKeyword(idText) || idText == "value")
    {
        return;
    }

    const TSPoint start = ts_node_start_point(node);
    const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
    if (CheckClosureDisallowedAccess(node, scope, idText, ctx))
    {
        return;
    }

    CheckImplicitMemberAccess(node, scope, idText, ctx);
}

} // namespace angel_lsp::analysis
