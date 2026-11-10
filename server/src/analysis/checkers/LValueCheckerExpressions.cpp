#include "analysis/checkers/LValueChecker.h"
#include "analysis/checkers/LValueCheckerCommon.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if an identifier or symbol name denotes an assignable variable or property.
 * @param[in] name Identifier name to inspect.
 * @param[in] scope Current lexical scope.
 * @param[in] table Workspace symbol table.
 * @param[in] refByte Byte offset of the reference node in source code.
 * @return True if identifier can be assigned to; false otherwise.
 */
static bool IsIdentifierAssignable(std::string_view name, const Scope* scope, const SymbolTable& table,
                                   uint32_t refByte = UINT32_MAX)
{
    if (IsPrimitiveTypeName(name))
    {
        return false;
    }

    if (scope)
    {
        const LocalDefinition* localDef = ResolveInScopeAt(scope, name, refByte);
        if (localDef)
        {
            return localDef->kind != LocalDefinitionKind::Function && localDef->kind != LocalDefinitionKind::Method;
        }
    }

    auto symbols = table.FindSymbolsPtr(name);
    if (symbols && !symbols->empty())
    {
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

/**
 * @brief Checks if a member expression denotes an assignable property, variable, or setter method.
 * @param[in] node Member expression AST node.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context containing symbol table and expression cache.
 * @return True if member expression can be assigned to; false otherwise.
 */
static bool IsAssignableMember(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    TSNode objNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return true;
    }
    std::string memName(parser::NodeText(memNode, ctx.request.sourceCode));
    if (memName.empty())
    {
        return true;
    }
    std::string objType = ResolveExpressionType(objNode, ExpressionTypeContext(scope, ctx));
    std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty())
    {
        return true;
    }
    for (const auto& typeName : GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable, ctx.request.exprCache))
    {
        if (auto found = ctx.request.symbolTable.FindMemberSymbolPtr(typeName, memName))
        {
            bool hasAssignable = false;
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    hasAssignable = true;
                    break;
                }
            }
            if (!hasAssignable)
            {
                bool hasSetter = false;
                for (const auto& ancestor :
                     GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable, ctx.request.exprCache))
                {
                    auto setters = ctx.request.symbolTable.FindMemberSymbolPtr(ancestor, "set_" + memName);
                    if (setters && !setters->empty())
                    {
                        hasSetter = true;
                        break;
                    }
                }
                if (!hasSetter)
                {
                    return false;
                }
            }
        }
    }
    return true;
}

/**
 * @brief Checks if both branches of a ternary expression are assignable l-values of matching type.
 * @param[in] node Ternary expression AST node.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context containing symbol table and expression cache.
 * @return True if ternary expression can be assigned to; false otherwise.
 */
static bool IsTernaryAssignable(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    TSNode consequence = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldConsequence);
    TSNode alternative = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldAlternative);
    if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
    {
        if (ts_node_named_child_count(node) >= 3)
        {
            consequence = ts_node_named_child(node, 1);
            alternative = ts_node_named_child(node, 2);
        }
    }
    if (!LValueNodeValidator::IsAssignableLValueNode(consequence, scope, ctx) ||
        !LValueNodeValidator::IsAssignableLValueNode(alternative, scope, ctx))
    {
        return false;
    }
    const std::string type1 = ResolveExpressionType(consequence, ExpressionTypeContext(scope, ctx));
    const std::string type2 = ResolveExpressionType(alternative, ExpressionTypeContext(scope, ctx));
    if (!type1.empty() && !type2.empty() && CleanBaseType(type1) != CleanBaseType(type2))
    {
        return false;
    }
    return true;
}

/**
 * @brief Checks if a unary expression is an assignable handle dereference expression.
 * @param[in] node Unary expression AST node.
 * @param[in] scope Current lexical scope.
 * @param[in] ctx Diagnostic context containing symbol table and expression cache.
 * @return True if unary operand is an assignable l-value; false otherwise.
 */
static bool IsUnaryAssignable(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    if (!ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokAt)
    {
        TSNode operand = parser::GetChildByField(node, gs.fieldOperand);
        return LValueNodeValidator::IsAssignableLValueNode(operand, scope, ctx);
    }
    return false;
}

bool LValueNodeValidator::IsAssignableLValueNode(TSNode rawNode, const Scope* scope, const DiagnosticContext& ctx)
{
    if (ts_node_is_null(rawNode))
    {
        return false;
    }

    TSNode node = parser::UnwrapParentheses(rawNode);
    if (ts_node_is_null(node))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symTernaryExpression)
    {
        return IsTernaryAssignable(node, scope, ctx);
    }
    if (sym == gs.symUnaryExpression)
    {
        return IsUnaryAssignable(node, scope, ctx);
    }
    if (sym == gs.symMemberExpression)
    {
        return IsAssignableMember(node, scope, ctx);
    }
    if (sym == gs.symIndexExpression)
    {
        return true;
    }
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier || sym == gs.tokFunction)
    {
        return IsIdentifierAssignable(parser::NodeText(node, ctx.request.sourceCode), scope, ctx.request.symbolTable,
                                      ts_node_start_byte(node));
    }
    return false;
}

} // namespace angel_lsp::analysis
