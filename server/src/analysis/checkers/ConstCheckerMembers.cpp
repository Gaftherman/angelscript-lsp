#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"

#include <array>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Identifies enclosing class and whether the enclosing method or accessor is const.
 * @param[in] node AST node inside the method.
 * @param[in] sourceCode Source text.
 * @return EnclosingMethodInfo containing class membership and const qualification.
 */
EnclosingMethodInfo FindEnclosingMethod(TSNode node, std::string_view sourceCode)
{
    EnclosingMethodInfo info;
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> funcSyms = {gs.symFuncDeclaration, gs.symAccessor};
    TSNode funcNode = parser::FindAncestorWithAnySymbol(node, funcSyms);
    if (ts_node_is_null(funcNode))
    {
        return info;
    }

    TSNode classNode = parser::FindAncestorWithSymbol(funcNode, gs.symClassDeclaration);
    if (!ts_node_is_null(classNode))
    {
        info.inClass = true;
        TSNode nameNode = parser::GetChildByField(classNode, gs.fieldName);
        if (!ts_node_is_null(nameNode))
        {
            info.className = parser::NodeText(nameNode, sourceCode);
        }
    }

    if (!info.inClass)
    {
        return info;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            const TSSymbol sym = ts_node_symbol(child);
            if (sym == gs.tokConst ||
                (sym == gs.symFuncAttributes && !ts_node_is_null(parser::FindChildWithSymbol(child, gs.tokConst))))
            {
                info.isConstMethod = true;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return info;
}

/**
 * @brief Finds a member variable or property in a class's inheritance hierarchy.
 * @param[in] className Name of the class to search.
 * @param[in] memberName Member name to look for.
 * @param[in] table Workspace symbol table.
 * @return Pointer to VariableSignature if found, nullptr otherwise.
 */
static const VariableSignature* FindClassMemberVariable(const std::string& className, const std::string& memberName,
                                                        const SymbolTable& table, ExpressionTypeCache* cache = nullptr)
{
    if (className.empty() || memberName.empty())
    {
        return nullptr;
    }
    for (const auto& owner : GetInheritedTypeHierarchy(className, table, cache))
    {
        if (const auto candidates = table.FindMemberSymbolPtr(owner, memberName))
        {
            for (const auto& sym : *candidates)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    return &sym.GetVariable();
                }
            }
        }
    }
    return nullptr;
}

/**
 * @brief Checks if an identifier or scoped identifier represents a member of 'this'.
 * @param[in] target AST node representing the identifier.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Expression type context containing symbols and source.
 * @param[out] outMemberName Resolved member name if matched.
 * @return True if target represents a member of 'this', false otherwise.
 */
static bool IsIdentifierMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo,
                                     const ExpressionTypeContext& ctx, std::string& outMemberName)
{
    const std::string text(parser::NodeText(target, ctx.sourceCode));
    const auto [prefix, baseName] = utils::SplitScopedName(text);
    if (!prefix.empty() && prefix != encInfo.className)
    {
        return false;
    }

    const std::string name(baseName);
    if (ctx.scope)
    {
        const Scope* owner = nullptr;
        if (const LocalDefinition* def = ResolveInScope(ctx.scope, name, &owner))
        {
            if (def->kind == LocalDefinitionKind::Field || (owner && owner->kind == ScopeKind::Class))
            {
                outMemberName = name;
                return true;
            }
            return false;
        }
    }
    if (FindClassMemberVariable(encInfo.className, name, ctx.symbolTable, ctx.cache))
    {
        outMemberName = name;
        return true;
    }
    return false;
}

/**
 * @brief Checks if a member expression represents a mutation of 'this'.
 * @param[in] target AST node representing the member expression.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Expression type context containing symbols and source.
 * @param[out] outMemberName Resolved member name if matched.
 * @return True if target represents a member of 'this', false otherwise.
 */
static bool IsMemberExprOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ExpressionTypeContext& ctx,
                               std::string& outMemberName)
{
    TSNode obj = parser::GetChildByField(target, parser::GetGrammarSymbols().fieldObject);
    TSNode member = parser::GetChildByField(target, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(obj) || ts_node_is_null(member))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol objSym = ts_node_symbol(obj);
    if (objSym == gs.symThisExpression ||
        (objSym == gs.symIdentifier && parser::NodeText(obj, ctx.sourceCode) == parser::keywords::This))
    {
        outMemberName = parser::NodeText(member, ctx.sourceCode);
        return true;
    }

    std::string parentMember;
    if (IsMemberOfThis(obj, encInfo, ctx, parentMember))
    {
        bool hasHandleType = false;
        if (const auto* var = FindClassMemberVariable(encInfo.className, parentMember, ctx.symbolTable, ctx.cache))
        {
            hasHandleType = var->modifiers.isHandle;
        }
        else if (ctx.scope)
        {
            if (const auto* def = ResolveInScope(ctx.scope, parentMember))
            {
                hasHandleType = def->isHandleType;
            }
        }
        if (!hasHandleType)
        {
            outMemberName = parser::NodeText(member, ctx.sourceCode);
            return true;
        }
    }
    return false;
}

/**
 * @brief Determines if an assignment target refers to a member of 'this' in a const method.
 * @param[in] target Target AST node of the assignment.
 * @param[in] encInfo Enclosing method information.
 * @param[in] ctx Expression type context containing symbols and source.
 * @param[out] outMemberName Name of the modified member.
 * @return True if target modifies 'this' or a member of 'this' in a const context.
 */
bool IsMemberOfThis(TSNode target, const EnclosingMethodInfo& encInfo, const ExpressionTypeContext& ctx,
                    std::string& outMemberName)
{
    if (!encInfo.inClass || !encInfo.isConstMethod || ts_node_is_null(target))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targetSym = ts_node_symbol(target);
    if (targetSym == gs.symThisExpression ||
        (targetSym == gs.symIdentifier && parser::NodeText(target, ctx.sourceCode) == parser::keywords::This))
    {
        outMemberName = parser::keywords::This;
        return true;
    }

    if (targetSym == gs.symIdentifier || targetSym == gs.symScopedIdentifier)
    {
        return IsIdentifierMemberOfThis(target, encInfo, ctx, outMemberName);
    }

    if (targetSym == gs.symMemberExpression)
    {
        return IsMemberExprOfThis(target, encInfo, ctx, outMemberName);
    }

    return false;
}

} // namespace angel_lsp::analysis
