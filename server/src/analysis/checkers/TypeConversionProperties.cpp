#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Looks for a declared virtual property named like info.propName on one type.
 * @param[in] typeName Qualified name of the type searched.
 * @param[in] table Symbol table searched for members.
 * @param[in,out] info Receives the accessor flags when a virtual property matches; its propName names the property.
 * @return True when a virtual property was found.
 */
static bool CheckVirtualPropertySymbol(std::string_view typeName, const SymbolTable& table, PropertyAccessInfo& info)
{
    if (auto propSyms = table.FindMemberSymbolPtr(typeName, info.propName))
    {
        for (const auto& s : *propSyms)
        {
            if (s.type == SymbolType::Property && std::holds_alternative<VariableSignature>(s.signature))
            {
                const auto& vs = s.GetVariable();
                if (vs.isVirtualProperty)
                {
                    info.isProperty = true;
                    if (vs.hasGet)
                    {
                        info.hasGet = true;
                    }
                    if (vs.hasSet)
                    {
                        info.hasSet = true;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Looks for get_ and set_ methods named like info.propName on one type.
 * @param[in] typeName Qualified name of the type searched.
 * @param[in] table Symbol table searched for members.
 * @param[in,out] info Receives the accessor flags; its propName names the property.
 */
static void CheckAccessorMethods(std::string_view typeName, const SymbolTable& table, PropertyAccessInfo& info)
{
    if (!info.hasGet)
    {
        if (auto getSyms = table.FindMemberSymbolPtr(typeName, "get_" + info.propName); getSyms && !getSyms->empty())
        {
            info.isProperty = true;
            info.hasGet = true;
            for (const auto& gs : *getSyms)
            {
                if (gs.type == SymbolType::Function && !gs.GetFunction().parameters.empty())
                {
                    info.isIndexed = true;
                }
            }
        }
    }
    if (!info.hasSet)
    {
        if (auto setSyms = table.FindMemberSymbolPtr(typeName, "set_" + info.propName); setSyms && !setSyms->empty())
        {
            info.isProperty = true;
            info.hasSet = true;
            for (const auto& ss : *setSyms)
            {
                if (ss.type == SymbolType::Function && ss.GetFunction().parameters.size() > 1)
                {
                    info.isIndexed = true;
                }
            }
        }
    }
}

/**
 * @brief Folds accessor functions into the property flags.
 * @param[in] accessors Functions found for one property name.
 * @param[in,out] info Receives isProperty, hasGet, hasSet and isIndexed.
 */
static void PopulateAccessorInfo(const std::vector<Symbol>& accessors, PropertyAccessInfo& info)
{
    for (const auto& sym : accessors)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto& fn = sym.GetFunction();
            info.isProperty = true;
            if (sym.name.starts_with("get_"))
            {
                info.hasGet = true;
                if (!fn.parameters.empty())
                {
                    info.isIndexed = true;
                }
            }
            else if (sym.name.starts_with("set_"))
            {
                info.hasSet = true;
                if (fn.parameters.size() > 1)
                {
                    info.isIndexed = true;
                }
            }
        }
    }
}

/**
 * @brief Inspects an identifier or scoped identifier that may name a virtual property.
 * @param[in] access Indexed name fact.
 * @param[in] scope Enclosing lexical scope; a local of the same name hides the property.
 * @param[in] ctx Diagnostic context.
 * @return Accessor flags; none are set for a local or an unknown name.
 */
static PropertyAccessInfo InspectBareOrScopedProperty(const PropertyAccessFact& access, const Scope* scope,
                                                      const DiagnosticContext& ctx)
{
    PropertyAccessInfo info;
    const auto [scopeName, propName] = utils::SplitScopedName(access.text);
    if (!scopeName.empty())
    {
        info.propName = TrimString(propName);
        auto accessors = FindPropertyAccessors(TrimString(scopeName), info.propName, ctx.request.symbolTable, false);
        PopulateAccessorInfo(accessors, info);
        return info;
    }

    if (scope != nullptr && ResolveInScopeAt(scope, access.text, access.bytes.startByte))
    {
        return info;
    }

    info.propName = access.text;
    auto accessors = FindGlobalPropertyAccessors(info.propName, ctx.request.symbolTable, false);
    PopulateAccessorInfo(accessors, info);
    if (info.isProperty || ctx.request.nodeIndex == nullptr)
    {
        return info;
    }

    const auto containers = ctx.request.nodeIndex->CollectLookupContainers(access.bytes.startByte, access.bytes.endByte,
                                                                           access.nodeSymbol, ctx.request.sourceCode);
    for (const auto& cont : containers)
    {
        const auto& cName = cont.qualifiedName.empty() ? cont.name : cont.qualifiedName;
        auto contAcc = FindPropertyAccessors(cName, info.propName, ctx.request.symbolTable, false);
        PopulateAccessorInfo(contAcc, info);
        if (!info.isProperty)
        {
            continue;
        }
        if (info.hasGet && !info.hasSet)
        {
            for (const auto& ancestor :
                 GetInheritedTypeHierarchy(cName, ctx.request.symbolTable, ctx.request.exprCache))
            {
                auto ancestorAcc = FindPropertyAccessors(ancestor, info.propName, ctx.request.symbolTable, false);
                for (const auto& sym : ancestorAcc)
                {
                    if (sym.name.starts_with("set_"))
                    {
                        info.hasSet = true;
                        break;
                    }
                }
                if (info.hasSet)
                {
                    break;
                }
            }
        }
        return info;
    }
    return info;
}

PropertyAccessInfo InspectPropertyAccess(const PropertyAccessFact& access, const Scope* scope,
                                         const DiagnosticContext& ctx)
{
    if (access.kind == PropertyAccessKind::Name)
    {
        return InspectBareOrScopedProperty(access, scope, ctx);
    }

    PropertyAccessInfo info;
    if (access.kind != PropertyAccessKind::Member)
    {
        return info;
    }

    info.propName = access.text;
    info.receiverType = ResolveExpressionType(access.receiver, ExpressionTypeContext(scope, ctx));
    const auto cleanObj = CleanBaseType(info.receiverType);
    if (cleanObj.empty())
    {
        return info;
    }

    for (const auto& typeName : GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable))
    {
        CheckVirtualPropertySymbol(typeName, ctx.request.symbolTable, info);
        CheckAccessorMethods(typeName, ctx.request.symbolTable, info);
        if (info.isProperty && info.hasGet && info.hasSet)
        {
            return info;
        }
    }
    return info;
}

void CheckAssignmentPropertyAccess(const AssignmentSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    const PropertyAccessInfo pInfo = InspectPropertyAccess(site.leftProperty, scope, ctx);
    if (!pInfo.isProperty)
    {
        return;
    }

    if (site.leftIsIndex)
    {
        if (!pInfo.isIndexed)
        {
            return;
        }
        if (!site.opIsAssign)
        {
            ctx.EmitAtRange(site.range, "as-err-compound-assign-on-indexed-prop", pInfo.propName);
        }
        if (!pInfo.hasSet && pInfo.hasGet)
        {
            ctx.EmitAtRange(site.left.range, "as-err-read-only-property", pInfo.propName);
        }
        return;
    }

    if (!pInfo.hasSet && pInfo.hasGet)
    {
        ctx.EmitAtRange(site.left.range, "as-err-read-only-property", pInfo.propName);
    }
    else if (!site.opIsAssign && !IsHandleTypeView(pInfo.receiverType))
    {
        ctx.EmitAtRange(site.range, "as-err-compound-assign-on-value-prop", pInfo.propName);
    }
}

} // namespace angel_lsp::analysis
