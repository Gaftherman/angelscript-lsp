#include "analysis/resolution/ReferenceCountingExprType.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeBinary.h"
#include "analysis/resolution/ExpressionTypeUnary.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace angel_lsp::analysis
{
static std::string ComputeExprTypeUncached(const IndexedExprNode& node, ExprTypeEvalContext& ctx)
{
    switch (node.kind)
    {
    case IndexedExprKind::Identifier:
        return ResolveIdentifierExprType(node, ctx);
    case IndexedExprKind::This:
        return ctx.scope ? std::string(ctx.scope->GetEnclosingClass()) : std::string{};
    case IndexedExprKind::Literal:
        return (node.text == "string") ? std::string(ctx.symbolTable.GetStringType()) : node.text;
    case IndexedExprKind::Null:
        return std::string(parser::keywords::Null);
    case IndexedExprKind::Construct:
    case IndexedExprKind::Cast:
        return node.text;
    case IndexedExprKind::Member:
    {
        const std::string objType = ResolveIndexedExprType(node.left, ctx);
        if (objType.empty())
        {
            return {};
        }
        const std::string normalized = NormalizeType(objType, ctx.symbolTable.GetDefaultArrayType());
        const std::string owner = MemberOwnerType(normalized, ctx.symbolTable.GetDefaultArrayType());
        for (const auto& cls : GetInheritedTypeHierarchy(owner, ctx.symbolTable))
        {
            if (const auto syms = ctx.symbolTable.FindMemberSymbolPtr(cls, node.text))
            {
                for (const auto& m : *syms)
                {
                    if ((m.type == SymbolType::Variable || m.type == SymbolType::Property) &&
                        !m.GetVariable().typeName.empty())
                    {
                        return SubstituteMemberReturnType(m.GetVariable().typeName, normalized, ctx.symbolTable);
                    }
                }
            }
        }
        const auto accessors = FindPropertyAccessors(owner, node.text, ctx.symbolTable, false);
        for (const auto& acc : accessors)
        {
            if (acc.type == SymbolType::Function && acc.name.starts_with("get_"))
            {
                return SubstituteMemberReturnType(acc.GetFunction().returnType, normalized, ctx.symbolTable);
            }
        }
        return {};
    }
    case IndexedExprKind::Call:
        return ResolveCallExprType(node, ctx);
    case IndexedExprKind::Index:
    {
        const std::string objType = ResolveIndexedExprType(node.left, ctx);
        if (objType.empty())
        {
            return {};
        }
        std::vector<std::string> argTypes;
        argTypes.reserve(node.argCount);
        for (uint32_t i = 0; i < node.argCount; ++i)
        {
            argTypes.push_back(ResolveIndexedExprType(ctx.sites.argIds[node.argBegin + i], ctx));
        }
        const bool isConst = IsIndexedExprConst(node.left, ctx);
        const auto match = ResolveMemberOverload({objType, "opIndex", argTypes, isConst}, ctx.symbolTable);
        if (match.hasMatch)
        {
            return SubstituteMemberReturnType(match.symbol.GetFunction().returnType, objType, ctx.symbolTable);
        }
        return {};
    }
    case IndexedExprKind::Binary:
    {
        const std::string lt = ResolveIndexedExprType(node.left, ctx);
        const std::string rt = ResolveIndexedExprType(node.right, ctx);
        if (lt.empty() || rt.empty())
        {
            return {};
        }
        ExpressionTypeContext exprCtx(ctx.scope, ctx.symbolTable, {}, {});
        return EvaluateBinaryResultType(node.text, lt, rt, exprCtx);
    }
    case IndexedExprKind::Assign:
        return ResolveIndexedExprType(node.left, ctx);
    case IndexedExprKind::Unary:
    {
        if (node.text == "!" || node.text == "not")
        {
            return std::string(parser::primitives::Bool);
        }
        const std::string opType = ResolveIndexedExprType(node.left, ctx);
        if (opType.empty())
        {
            return {};
        }
        if (node.text == "-")
        {
            if (IsPrimitiveTypeName(CleanBaseTypeView(opType)))
            {
                return ResolveUnaryMinus(opType);
            }
            const auto match =
                ResolveMemberOverload({opType, "opNeg", {}, IsIndexedExprConst(node.left, ctx)}, ctx.symbolTable);
            return match.hasMatch
                       ? SubstituteMemberReturnType(match.symbol.GetFunction().returnType, opType, ctx.symbolTable)
                       : opType;
        }
        return opType;
    }
    case IndexedExprKind::Postfix:
        return ResolveIndexedExprType(node.left, ctx);
    case IndexedExprKind::Ternary:
    {
        std::string t = ResolveIndexedExprType(node.extra, ctx);
        return t.empty() ? ResolveIndexedExprType(node.right, ctx) : t;
    }
    case IndexedExprKind::InitList:
        return node.text;
    case IndexedExprKind::Unknown:
        return {};
    }
    return {};
}

std::string ResolveIndexedExprType(uint32_t exprId, ExprTypeEvalContext& ctx)
{
    if (exprId >= ctx.sites.exprs.size())
    {
        return {};
    }
    if (ctx.state[exprId] == 2)
    {
        return ctx.memo[exprId];
    }
    if (ctx.state[exprId] == 1)
    {
        return {};
    }
    ctx.state[exprId] = 1;
    ctx.memo[exprId] = ComputeExprTypeUncached(ctx.sites.exprs[exprId], ctx);
    ctx.state[exprId] = 2;
    return ctx.memo[exprId];
}

bool IsIndexedExprConst(uint32_t exprId, const ExprTypeEvalContext& ctx)
{
    if (exprId >= ctx.sites.exprs.size() || !ctx.scope)
    {
        return false;
    }
    const auto& node = ctx.sites.exprs[exprId];
    if (node.kind == IndexedExprKind::Identifier && !node.isScoped)
    {
        if (const auto* def = ResolveInScopeAt(ctx.scope, node.text, node.startByte))
        {
            return def->isConst || IsConstTypeView(def->typeName);
        }
    }
    if (node.kind == IndexedExprKind::This)
    {
        return ctx.scope->isConstMethod;
    }
    return exprId < ctx.memo.size() && IsConstTypeView(ctx.memo[exprId]);
}

bool IsDeclaredTypeSymbol(std::string_view typeName, const SymbolTable& symbolTable)
{
    if (typeName.empty())
    {
        return false;
    }
    if (const auto found = symbolTable.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *found)
        {
            if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface || sym.type == SymbolType::Enum ||
                sym.type == SymbolType::Typedef || sym.type == SymbolType::Funcdef)
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace angel_lsp::analysis
