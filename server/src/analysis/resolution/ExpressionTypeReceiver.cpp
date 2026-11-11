#include "analysis/resolution/ExpressionTypeResolver.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static std::string ExtractMemberSymbolOwner(const Symbol& sym, std::string_view arrayTypeName)
{
    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
        std::holds_alternative<VariableSignature>(sym.signature))
    {
        const auto& var = sym.GetVariable();
        if (!var.typeName.empty())
        {
            return MemberOwnerType(var.typeName, arrayTypeName);
        }
    }
    else if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        const auto& fn = sym.GetFunction();
        if (!fn.returnType.empty() && fn.returnType != "void")
        {
            return MemberOwnerType(fn.returnType, arrayTypeName);
        }
    }
    return "";
}

static std::string ResolveMemberInHierarchy(const MemberHierarchyQuery& query, const SymbolTable& symbolTable)
{
    auto hier = GetInheritedTypeHierarchy(std::string(query.hostClass), symbolTable, query.cache);
    ExpandMixinHostHierarchies(query.hostClass, symbolTable, hier, query.cache);
    for (const auto& cls : hier)
    {
        auto memberSyms = symbolTable.FindMemberSymbols(cls, query.memberName);
        for (const auto& sym : memberSyms)
        {
            if (auto owner = ExtractMemberSymbolOwner(sym, query.arrayTypeName); !owner.empty())
            {
                return owner;
            }
        }
        auto accessors = FindPropertyAccessors(cls, std::string(query.memberName), symbolTable, false);
        if (!accessors.empty())
        {
            std::string pType = PropertyTypeFromAccessors(accessors);
            if (!pType.empty())
            {
                return MemberOwnerType(pType, query.arrayTypeName);
            }
        }
    }
    return "";
}

static std::string ResolveThisReceiver(IndexedByteRange objRange, std::string_view sourceCode,
                                       std::string_view effectiveHostClass, const NodeIndex* nodeIndex = nullptr)
{
    if (!effectiveHostClass.empty())
    {
        return std::string(effectiveHostClass);
    }
    if (nodeIndex && (objRange.startByte != 0 || objRange.endByte != 0))
    {
        auto containers = GetEnclosingContainers(objRange, *nodeIndex, sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
            {
                return c.qualifiedName.empty() ? c.name : c.qualifiedName;
            }
        }
    }
    return "";
}

static std::string ResolveReceiverInEnclosingContainers(IndexedByteRange objRange, std::string_view sourceCode,
                                                        const MemberHierarchyQuery& query,
                                                        const SymbolTable& symbolTable)
{
    if (query.nodeIndex && (objRange.startByte != 0 || objRange.endByte != 0))
    {
        auto containers = GetEnclosingContainers(objRange, *query.nodeIndex, sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
            {
                std::string enclosing = c.qualifiedName.empty() ? c.name : c.qualifiedName;
                MemberHierarchyQuery subQuery{enclosing, query.memberName, query.arrayTypeName, query.cache,
                                              query.nodeIndex};
                return ResolveMemberInHierarchy(subQuery, symbolTable);
            }
        }
    }
    return "";
}

static std::string ResolveReceiverInGlobalOrShort(std::string_view objText, const SymbolTable& symbolTable,
                                                  std::string_view arrayTypeName)
{
    auto globSyms = symbolTable.FindSymbols(objText);
    for (const auto& sym : globSyms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            std::holds_alternative<VariableSignature>(sym.signature))
        {
            const auto& var = sym.GetVariable();
            if (!var.typeName.empty())
            {
                return MemberOwnerType(var.typeName, arrayTypeName);
            }
        }
        else if (sym.type == SymbolType::Class || sym.type == SymbolType::Namespace)
        {
            return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
        }
    }

    auto shortMatches = symbolTable.FindTypeSymbolsByShortName(objText);
    for (const auto& sym : shortMatches)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Namespace)
        {
            return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
        }
    }
    return "";
}

static std::string GetEffectiveHostClass(std::string_view virtualHostClass, std::string_view fileUri)
{
    if (!virtualHostClass.empty())
    {
        return std::string(virtualHostClass);
    }
    if (fileUri.starts_with("angelscript-virtual:") || fileUri.starts_with("angelscript-virtual://"))
    {
        return SymbolTable::ExtractVirtualHostClass(fileUri);
    }
    return "";
}

static std::string ResolveReceiverScope(const Scope* scope, std::string_view objText, uint32_t refByte,
                                        const ExpressionTypeContext* exprCtx, std::string_view arrayTypeName)
{
    if (scope)
    {
        const LocalDefinition* objDef = ResolveInScopeAt(scope, objText, refByte);
        if (objDef)
        {
            if (!objDef->typeName.empty() && CleanBaseTypeView(objDef->typeName) != "auto")
            {
                return MemberOwnerType(objDef->typeName, arrayTypeName);
            }
            if (exprCtx)
            {
                std::string deduced = ResolveLocalDefinitionType(*objDef, *exprCtx);
                if (!deduced.empty())
                {
                    return MemberOwnerType(deduced, arrayTypeName);
                }
            }
            if (!objDef->typeName.empty())
            {
                return MemberOwnerType(objDef->typeName, arrayTypeName);
            }
        }
    }
    return "";
}

static std::string ResolveMemberOrContainerReceiver(IndexedByteRange objRange, std::string_view sourceCode,
                                                    const MemberHierarchyQuery& query, const SymbolTable& symbolTable)
{
    if (!query.hostClass.empty())
    {
        if (auto res = ResolveMemberInHierarchy(query, symbolTable); !res.empty())
        {
            return res;
        }
        return "";
    }
    return ResolveReceiverInEnclosingContainers(objRange, sourceCode, query, symbolTable);
}

static std::string ResolveReceiverTypeUncached(IndexedByteRange objRange, std::string_view sourceCode,
                                               const SymbolTable& symbolTable, const ReceiverTypeContext& ctx)
{
    if (objRange.startByte >= sourceCode.size() || objRange.endByte > sourceCode.size() ||
        objRange.startByte >= objRange.endByte)
    {
        return "";
    }
    std::string_view objText = sourceCode.substr(objRange.startByte, objRange.endByte - objRange.startByte);
    TrimTypeWhitespace(objText);
    if (objText.empty())
    {
        return "";
    }

    std::string effectiveHostClass = GetEffectiveHostClass(ctx.virtualHostClass, ctx.fileUri);
    if (effectiveHostClass.empty())
    {
        effectiveHostClass =
            InferHostClassFromEnclosingMixin(objRange, ctx.nodeIndex, sourceCode, symbolTable);
    }
    const std::string_view arrayTypeName =
        ctx.arrayTypeName.empty() ? symbolTable.GetDefaultArrayType() : ctx.arrayTypeName;

    if (objText == "this")
    {
        return ResolveThisReceiver(objRange, sourceCode, effectiveHostClass, ctx.nodeIndex);
    }

    ExpressionTypeContext exprCtx(ctx.scope, symbolTable, sourceCode, ctx.fileUri);
    exprCtx.cache = ctx.cache;
    exprCtx.nodeIndex = ctx.nodeIndex;
    if (auto scopeRes = ResolveReceiverScope(ctx.scope, objText, objRange.startByte, &exprCtx, arrayTypeName);
        !scopeRes.empty())
    {
        return scopeRes;
    }

    MemberHierarchyQuery query{effectiveHostClass, objText, arrayTypeName, ctx.cache, ctx.nodeIndex};
    if (auto res = ResolveMemberOrContainerReceiver(objRange, sourceCode, query, symbolTable); !res.empty())
    {
        return res;
    }

    if (auto delRes = ResolveDelegatePropertyReceiver(
            {objRange, std::string(objText), effectiveHostClass, sourceCode, arrayTypeName, ctx.nodeIndex},
            symbolTable, ctx);
        !delRes.empty())
    {
        return delRes;
    }

    std::string exprType = ResolveExpressionTypeAt(objRange, exprCtx);
    if (!exprType.empty() && exprType != "void" && exprType != "unknown")
    {
        return MemberOwnerType(exprType, arrayTypeName);
    }

    return ResolveReceiverInGlobalOrShort(objText, symbolTable, arrayTypeName);
}

std::string ResolveReceiverType(IndexedByteRange objRange, std::string_view sourceCode, const SymbolTable& symbolTable,
                                const ReceiverTypeContext& ctx)
{
    if (objRange.startByte >= objRange.endByte || objRange.endByte > sourceCode.size())
    {
        return "";
    }

    const uint64_t cacheKey =
        (static_cast<uint64_t>(objRange.startByte) << 32) | static_cast<uint64_t>(objRange.endByte);
    if (ctx.cache)
    {
        auto it = ctx.cache->receiverTypes.find(cacheKey);
        if (it != ctx.cache->receiverTypes.end())
        {
            return it->second;
        }
    }

    std::string result = ResolveReceiverTypeUncached(objRange, sourceCode, symbolTable, ctx);
    if (ctx.cache)
    {
        ctx.cache->receiverTypes.emplace(cacheKey, result);
    }
    return result;
}

} // namespace angel_lsp::analysis
