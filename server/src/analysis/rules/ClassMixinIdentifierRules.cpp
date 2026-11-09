#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/MixinSourceIndex.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/types/TypeSanitization.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Checks if a name is a built-in symbol, keyword, or primitive type.
 */
static bool IsBuiltinOrKeyword(std::string_view name, std::string_view effectiveStringTypeName)
{
    if (name.empty() || name == "this" || name == "super" || name == "cast" || name == "null" || name == "true" ||
        name == "false" || name == effectiveStringTypeName)
    {
        return true;
    }
    return IsKeyword(name) || IsPrimitiveTypeName(name);
}

static bool HasSelfOrHostPropertyAccessor(std::string_view getView, std::string_view setView,
                                          const FunctionCheckState& state)
{
    return state.mctx.mixinSelfMembers.contains(getView) || state.mctx.mixinSelfMembers.contains(setView) ||
           state.mctx.hostMembers.contains(getView) || state.mctx.hostMembers.contains(setView);
}

static bool HasGlobalPropertyAccessor(std::string_view name, std::string_view getView, std::string_view setView,
                                      const DiagnosticContext& ctx)
{
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    if (ruleIndex.globalAccessorPropertyNames.contains(name) ||
        ruleIndex.keywordGlobalAccessorPropertyNames.contains(name))
    {
        return true;
    }
    const auto& table = ctx.request.symbolTable;
    return table.FindSymbolsPtr(getView) != nullptr || table.FindSymbolsPtr(setView) != nullptr;
}

static bool HasNamespacePropertyAccessor(const AccessorLookupContext& actx, std::string_view ns,
                                         const DiagnosticContext& ctx)
{
    if (ns.empty())
    {
        return false;
    }
    if (ctx.request.GetRuleIndex().NamespaceMembers(ns).accessorPropertyNames.contains(actx.name))
    {
        return true;
    }
    const auto& table = ctx.request.symbolTable;
    const auto hasAccessor = [&](std::string_view view)
    {
        if (auto ptr = table.FindMemberSymbolPtr(ns, view))
        {
            for (const auto& sym : *ptr)
            {
                if (sym.containerKind == ContainerKind::Namespace)
                {
                    return true;
                }
            }
        }
        return false;
    };
    return hasAccessor(actx.getView) || hasAccessor(actx.setView);
}

/**
 * @brief Checks if a name matches getter/setter property accessors in mixin, host, global, or namespaces.
 */
static bool HasPropertyAccessorMatch(std::string_view name, const FunctionCheckState& state,
                                     const DiagnosticContext& ctx)
{
    if (name.size() > 120)
    {
        return false;
    }
    char buf[128];
    std::memcpy(buf, "get_", 4);
    std::memcpy(buf + 4, name.data(), name.size());
    const std::string_view getView(buf, 4 + name.size());

    std::memcpy(buf, "set_", 4);
    const std::string_view setView(buf, 4 + name.size());

    if (HasSelfOrHostPropertyAccessor(getView, setView, state) ||
        HasGlobalPropertyAccessor(name, getView, setView, ctx))
    {
        return true;
    }

    const AccessorLookupContext actx{name, getView, setView};
    if (state.mctx.mixinSym && HasNamespacePropertyAccessor(actx, state.mctx.mixinSym->containerName, ctx))
    {
        return true;
    }
    if (HasScopeQualifier(state.mctx.hostClassName) &&
        HasNamespacePropertyAccessor(actx, utils::ParentScopeView(state.mctx.hostClassName), ctx))
    {
        return true;
    }
    for (const auto& uNs : analysis::GetUsingNamespaces(ctx.request))
    {
        if (HasNamespacePropertyAccessor(actx, uNs, ctx))
        {
            return true;
        }
    }
    return false;
}

static bool HasNamespaceMember(std::string_view name, std::string_view ns, const DiagnosticContext& ctx)
{
    if (ns.empty())
    {
        return false;
    }
    if (ctx.request.GetRuleIndex().NamespaceMembers(ns).allMemberNames.contains(name))
    {
        return true;
    }
    if (auto ptr = ctx.request.symbolTable.FindMemberSymbolPtr(ns, name))
    {
        for (const auto& sym : *ptr)
        {
            if (sym.containerKind == ContainerKind::Namespace)
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsEngineOrGlobalIdentifier(std::string_view name, const DiagnosticContext& ctx)
{
    if (ctx.request.engineRules)
    {
        const auto& delProps = ctx.request.engineRules->unusedRules.baseClassDelegateProperties;
        if (std::find(delProps.begin(), delProps.end(), name) != delProps.end())
        {
            return true;
        }
    }
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    return ruleIndex.globalNames.contains(name) || ruleIndex.enumMemberNames.contains(name) ||
           ctx.request.symbolTable.FindSymbolsPtr(name) != nullptr;
}

static bool IsValidInEnclosingOrUsingNamespaces(std::string_view name, const FunctionCheckState& state,
                                                const DiagnosticContext& ctx)
{
    if (state.mctx.mixinSym && HasNamespaceMember(name, state.mctx.mixinSym->containerName, ctx))
    {
        return true;
    }
    if (HasScopeQualifier(state.mctx.hostClassName) &&
        HasNamespaceMember(name, utils::ParentScopeView(state.mctx.hostClassName), ctx))
    {
        return true;
    }
    for (const auto& uNs : analysis::GetUsingNamespaces(ctx.request))
    {
        if (HasNamespaceMember(name, uNs, ctx))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if an identifier resolves in local, mixin, host, or global scopes.
 */
bool IsValidMixinIdentifier(std::string_view name, const FunctionCheckState& state, const DiagnosticContext& ctx)
{
    if (IsBuiltinOrKeyword(name, ctx.request.GetEffectiveStringTypeName()))
    {
        return true;
    }
    if (state.localNames.contains(name) || state.mctx.mixinSelfMembers.contains(name) ||
        state.mctx.hostMembers.contains(name) || HasPropertyAccessorMatch(name, state, ctx))
    {
        return true;
    }
    if (IsEngineOrGlobalIdentifier(name, ctx))
    {
        return true;
    }
    return IsValidInEnclosingOrUsingNamespaces(name, state, ctx);
}

} // namespace angel_lsp::analysis::rules
