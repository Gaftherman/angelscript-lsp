#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <vector>

namespace angel_lsp::analysis
{

static bool CheckVirtualPropertySymbol(const std::string& typeName, const std::string& propName,
                                       const SymbolTable& table, PropertyAccessInfo& info)
{
    if (auto propSyms = table.FindMemberSymbolPtr(typeName, propName))
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

static void CheckAccessorMethods(const std::string& typeName, const std::string& propName, const SymbolTable& table,
                                 PropertyAccessInfo& info)
{
    if (!info.hasGet)
    {
        if (auto getSyms = table.FindMemberSymbolPtr(typeName, "get_" + propName); getSyms && !getSyms->empty())
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
        if (auto setSyms = table.FindMemberSymbolPtr(typeName, "set_" + propName); setSyms && !setSyms->empty())
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

static PropertyAccessInfo InspectBareOrScopedProperty(TSNode exprNode, const Scope* scope, const DiagnosticContext& ctx)
{
    PropertyAccessInfo info;
    std::string text = std::string(TrimString(parser::NodeText(exprNode, ctx.request.sourceCode)));
    const auto [scopeNameView, propNameView] = utils::SplitScopedName(text);
    if (!scopeNameView.empty())
    {
        std::string scopeName = std::string(TrimString(scopeNameView));
        info.propName = std::string(TrimString(propNameView));
        auto accessors = FindPropertyAccessors(scopeName, info.propName, ctx.request.symbolTable, false);
        PopulateAccessorInfo(accessors, info);
        return info;
    }

    if (scope != nullptr)
    {
        if (const auto* def = ResolveInScopeAt(scope, text, ts_node_start_byte(exprNode)))
        {
            return info;
        }
    }

    info.propName = text;
    auto accessors = FindGlobalPropertyAccessors(info.propName, ctx.request.symbolTable, false);
    PopulateAccessorInfo(accessors, info);
    if (info.isProperty)
    {
        return info;
    }

    for (const auto& cont : GetEnclosingContainers(exprNode, ctx.request.sourceCode, ctx.request.nodeIndex))
    {
        const std::string cName = cont.qualifiedName.empty() ? cont.name : cont.qualifiedName;
        auto contAcc = FindPropertyAccessors(cName, info.propName, ctx.request.symbolTable, false);
        PopulateAccessorInfo(contAcc, info);
        if (info.isProperty)
        {
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
    }
    return info;
}

PropertyAccessInfo InspectPropertyAccess(TSNode exprNode, const Scope* scope, const DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(exprNode);
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return InspectBareOrScopedProperty(exprNode, scope, ctx);
    }

    PropertyAccessInfo info;
    if (sym != gs.symMemberExpression)
    {
        return info;
    }

    const auto& grammar = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(exprNode, ctx.request.nodeIndex);
    TSNode objNode = syntax.Field(grammar.fieldObject);
    TSNode memNode = syntax.Field(grammar.fieldMember);
    if (ts_node_is_null(objNode))
    {
        objNode = syntax.FirstNamed();
        const TSNode fallbackMember = syntax.NamedAt(1);
        if (!ts_node_is_null(fallbackMember))
        {
            memNode = fallbackMember;
        }
    }
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return info;
    }

    info.propName = parser::NodeText(memNode, ctx.request.sourceCode);
    info.receiverType = ResolveExpressionType(objNode, ExpressionTypeContext(scope, ctx));
    const std::string cleanObj = CleanBaseType(info.receiverType);
    if (cleanObj.empty())
    {
        return info;
    }

    const auto hierarchy = GetInheritedTypeHierarchy(cleanObj, ctx.request.symbolTable);
    for (const auto& typeName : hierarchy)
    {
        CheckVirtualPropertySymbol(typeName, info.propName, ctx.request.symbolTable, info);
        CheckAccessorMethods(typeName, info.propName, ctx.request.symbolTable, info);
        if (info.isProperty && info.hasGet && info.hasSet)
        {
            return info;
        }
    }
    return info;
}

void CheckAssignmentPropertyAccess(TSNode node, const Scope* scope, DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(node, ctx.request.nodeIndex);
    const auto& gs = parser::GetGrammarSymbols();
    TSNode left = syntax.Field(gs.fieldLeft);
    if (ts_node_is_null(left))
    {
        return;
    }
    TSNode opNode = syntax.Field(gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);

    const TSSymbol leftSym = ts_node_symbol(left);
    if (leftSym == gs.symIndexExpression)
    {
        TSNode arrayNode = SyntaxOperands(left, ctx.request.nodeIndex).FieldOrNamed(gs.fieldObject);
        PropertyAccessInfo pInfo = InspectPropertyAccess(arrayNode, scope, ctx);
        if (pInfo.isProperty && pInfo.isIndexed)
        {
            if (opSym != gs.tokAssign)
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-compound-assign-on-indexed-prop",
                                    pInfo.propName);
            }
            if (!pInfo.hasSet && pInfo.hasGet)
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(left), "as-err-read-only-property", pInfo.propName);
            }
        }
    }
    else
    {
        PropertyAccessInfo pInfo = InspectPropertyAccess(left, scope, ctx);
        if (pInfo.isProperty)
        {
            if (!pInfo.hasSet && pInfo.hasGet)
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(left), "as-err-read-only-property", pInfo.propName);
            }
            else if (opSym != gs.tokAssign && !IsHandleTypeView(pInfo.receiverType))
            {
                ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-compound-assign-on-value-prop",
                                    pInfo.propName);
            }
        }
    }
}

} // namespace angel_lsp::analysis
