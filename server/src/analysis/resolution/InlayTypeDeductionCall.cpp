#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/InlayTypeDeductionInternal.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves receiver class type name for a method call expression.
 * @param[in] objNode Object expression AST node.
 * @param[in] exprNode Full call expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] rootScope Root scope of document, or nullptr.
 * @return Receiver type name or empty string.
 */
static std::string ResolveMemberCallReceiverType(TSNode objNode, TSNode exprNode, const ExpressionTypeContext& ctx,
                                                 const Scope* rootScope)
{
    std::string objText = parser::GetNodeText(objNode, ctx.sourceCode);
    if (objText == "this")
    {
        auto containers = GetEnclosingContainers(exprNode, ctx.sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == ContainerKind::Class)
            {
                return c.name;
            }
        }
    }
    else if (rootScope)
    {
        TSPoint objPoint = ts_node_start_point(objNode);
        const Scope* objScope = FindInnermostScope(rootScope, objPoint.row, objPoint.column);
        if (objScope)
        {
            const LocalDefinition* def = ResolveInScope(objScope, objText);
            if (def && !def->typeName.empty())
            {
                return CleanBaseType(def->typeName);
            }
        }
    }
    return objText;
}

/**
 * @brief Deduces return type for a member method call expression.
 * @param[in] funcNode AST member_expression node.
 * @param[in] exprNode Call expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @param[in] rootScope Root scope of document, or nullptr.
 * @return Return type string or empty string.
 */
static std::string DeduceMemberCallReturnType(TSNode funcNode, TSNode exprNode, const ExpressionTypeContext& ctx,
                                              const Scope* rootScope)
{
    TSNode objNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }
    std::string receiverType = ResolveMemberCallReceiverType(objNode, exprNode, ctx, rootScope);
    if (receiverType.empty())
    {
        return "";
    }
    std::string memText = parser::GetNodeText(memNode, ctx.sourceCode);
    auto hierarchy = GetInheritedTypeHierarchy(receiverType, ctx.symbolTable);
    if (hierarchy.empty())
    {
        hierarchy.push_back(receiverType);
    }
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedName = utils::JoinScopedName(typeName, memText);
        auto found = ctx.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : found)
        {
            if (sym.type == SymbolType::Function)
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    return "";
}

/**
 * @brief Deduces return type for a free or unqualified function call expression.
 * @param[in] funcNode Callee function identifier AST node.
 * @param[in] exprNode Call expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @return Return type string or class name, or empty string.
 */
static std::string DeduceFreeCallReturnType(TSNode funcNode, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    std::string fName = parser::GetNodeText(funcNode, ctx.sourceCode);
    auto candidates = FindSymbolsInScope(fName, exprNode, ctx.sourceCode, ctx.symbolTable);
    if (candidates.empty())
    {
        candidates = ctx.symbolTable.FindSymbols(fName);
    }
    for (const auto& sym : candidates)
    {
        if (sym.type == SymbolType::Function)
        {
            return sym.GetFunction().returnType;
        }
        if (sym.type == SymbolType::Funcdef)
        {
            return sym.GetFuncdef().returnType;
        }
        if (sym.type == SymbolType::Class)
        {
            return sym.name;
        }
    }
    return "";
}

std::string DeduceCallExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx, const Scope* rootScope)
{
    TSNode funcNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldFunction);
    if (ts_node_is_null(funcNode))
    {
        uint32_t childCount = ts_node_child_count(exprNode);
        if (childCount > 0)
        {
            funcNode = ts_node_child(exprNode, 0);
        }
    }
    if (ts_node_is_null(funcNode))
    {
        return "";
    }
    if (ts_node_symbol(funcNode) == parser::GetGrammarSymbols().symMemberExpression)
    {
        return DeduceMemberCallReturnType(funcNode, exprNode, ctx, rootScope);
    }
    return DeduceFreeCallReturnType(funcNode, exprNode, ctx);
}

std::string DeduceMemberExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx, const Scope* rootScope)
{
    TSNode objNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }
    std::string objText = parser::GetNodeText(objNode, ctx.sourceCode);
    std::string memText = parser::GetNodeText(memNode, ctx.sourceCode);
    std::string receiverType;
    if (rootScope)
    {
        TSPoint objPoint = ts_node_start_point(objNode);
        const Scope* objScope = FindInnermostScope(rootScope, objPoint.row, objPoint.column);
        if (objScope)
        {
            const LocalDefinition* def = ResolveInScope(objScope, objText);
            if (def && !def->typeName.empty())
            {
                receiverType = CleanBaseType(def->typeName);
            }
        }
    }
    if (receiverType.empty())
    {
        return "";
    }
    auto hierarchy = GetInheritedTypeHierarchy(receiverType, ctx.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedName = utils::JoinScopedName(typeName, memText);
        auto found = ctx.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : found)
        {
            if (sym.type == SymbolType::Variable)
            {
                return sym.GetVariable().typeName;
            }
            if (sym.type == SymbolType::Function)
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    return "";
}

} // namespace angel_lsp::analysis
