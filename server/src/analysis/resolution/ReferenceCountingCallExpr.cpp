#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ReferenceCountingExprType.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
static std::string FindFirstVarPropertyType(const std::vector<Symbol>& symbols, const SymbolTable& symbolTable)
{
    for (const auto& s : symbols)
    {
        if ((s.type == SymbolType::Variable || s.type == SymbolType::Property) && !s.GetVariable().typeName.empty())
        {
            return QualifyTypeInContainer(s.GetVariable().typeName, s.containerName, symbolTable);
        }
    }
    return {};
}

std::string ResolveIdentifierExprType(const IndexedExprNode& node, ExprTypeEvalContext& ctx)
{
    const std::string_view cleanName = utils::StripGlobalPrefix(node.text);
    const bool isExplicitGlobal = utils::HasLeadingGlobalPrefix(node.text);
    if (!node.isScoped && ctx.scope)
    {
        if (const auto* def = ResolveInScopeAt(ctx.scope, cleanName, node.startByte))
        {
            if (!def->typeName.empty())
            {
                return QualifyTypeInContainer(def->typeName, ctx.scope->GetEnclosingNamespace(), ctx.symbolTable);
            }
        }
    }
    if (!node.isScoped && ctx.scope && !ctx.scope->GetEnclosingClass().empty())
    {
        for (const auto& cls : GetInheritedTypeHierarchy(ctx.scope->GetEnclosingClass(), ctx.symbolTable))
        {
            if (const auto members = ctx.symbolTable.FindMemberSymbolPtr(cls, cleanName))
            {
                std::string resolved = FindFirstVarPropertyType(*members, ctx.symbolTable);
                if (!resolved.empty())
                {
                    return resolved;
                }
            }
        }
        const auto accessors = FindPropertyAccessors(ctx.scope->GetEnclosingClass(), cleanName, ctx.symbolTable, false);
        for (const auto& acc : accessors)
        {
            if (acc.type == SymbolType::Function && acc.name.starts_with("get_"))
            {
                return QualifyTypeInContainer(acc.GetFunction().returnType, acc.containerName, ctx.symbolTable);
            }
        }
    }
    if (!isExplicitGlobal && !utils::ContainsScopeSeparator(cleanName) && ctx.scope)
    {
        for (std::string_view ns = ctx.scope->GetEnclosingNamespace(); !ns.empty(); ns = utils::ParentScopeView(ns))
        {
            if (const auto found = ctx.symbolTable.FindMemberSymbolPtr(ns, cleanName))
            {
                std::string resolved = FindFirstVarPropertyType(*found, ctx.symbolTable);
                if (!resolved.empty())
                {
                    return resolved;
                }
            }
        }
    }
    if (const auto found = ctx.symbolTable.FindSymbolsPtr(cleanName))
    {
        return FindFirstVarPropertyType(*found, ctx.symbolTable);
    }
    return {};
}

std::string ResolveCallExprType(const IndexedExprNode& node, ExprTypeEvalContext& ctx)
{
    std::vector<std::string> argTypes;
    argTypes.reserve(node.argCount);
    for (uint32_t i = 0; i < node.argCount; ++i)
    {
        argTypes.push_back(ResolveIndexedExprType(ctx.sites.argIds[node.argBegin + i], ctx));
    }
    if (node.left >= ctx.sites.exprs.size())
    {
        return {};
    }
    const auto& callee = ctx.sites.exprs[node.left];
    if (callee.kind == IndexedExprKind::Member)
    {
        const std::string objType = ResolveIndexedExprType(callee.left, ctx);
        if (objType.empty())
        {
            return {};
        }
        const bool isConst = IsIndexedExprConst(callee.left, ctx);
        const auto match = ResolveMemberOverload({objType, callee.text, argTypes, isConst}, ctx.symbolTable);
        if (match.hasMatch)
        {
            return SubstituteMemberReturnType(match.symbol.GetFunction().returnType, objType, ctx.symbolTable);
        }
        return {};
    }
    if (callee.kind == IndexedExprKind::Identifier)
    {
        const std::string_view cleanName = utils::StripGlobalPrefix(callee.text);
        if (IsDeclaredTypeSymbol(cleanName, ctx.symbolTable))
        {
            return std::string(cleanName);
        }
        const std::string varType = ResolveIdentifierExprType(callee, ctx);
        if (!varType.empty())
        {
            if (const auto fd = FindFuncdefSymbol(CleanBaseTypeView(varType), ctx.symbolTable))
            {
                return fd->GetFuncdef().returnType;
            }
            const auto match = ResolveMemberOverload({varType, "opCall", argTypes, false}, ctx.symbolTable);
            if (match.hasMatch)
            {
                return SubstituteMemberReturnType(match.symbol.GetFunction().returnType, varType, ctx.symbolTable);
            }
        }
        if (const auto found = ctx.symbolTable.FindSymbolsPtr(cleanName))
        {
            std::vector<Symbol> cands;
            for (const auto& s : *found)
            {
                if (s.type == SymbolType::Function && MatchesMethodArity(s.GetFunction(), argTypes.size()))
                {
                    cands.push_back(s);
                }
            }
            if (cands.size() == 1)
            {
                return cands.front().GetFunction().returnType;
            }
            if (!cands.empty())
            {
                const auto match = ResolveBestOverload(cands, argTypes, ctx.symbolTable);
                if (match.bestCandidate)
                {
                    return match.bestCandidate->GetFunction().returnType;
                }
            }
        }
    }
    return {};
}
} // namespace angel_lsp::analysis
