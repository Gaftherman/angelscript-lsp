#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

void TrimString(std::string& s)
{
    while (!s.empty() && isspace(static_cast<unsigned char>(s.front())))
    {
        s.erase(s.begin());
    }
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
    {
        s.pop_back();
    }
}

bool IsObjectNodeConst(TSNode objectNode, const CallValidationContext& valCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(objectNode);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return false;
    }

    const std::string name(parser::NodeText(objectNode, valCtx.request.sourceCode));
    if (valCtx.scope)
    {
        if (const auto* def = ResolveInScopeAt(valCtx.scope, name, ts_node_start_byte(objectNode)))
        {
            if (!def->typeName.empty() && HasConstModifier(def->typeName))
            {
                return true;
            }
        }
    }

    if (const auto syms = valCtx.ctx.request.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& symbol : *syms)
        {
            if ((symbol.type == SymbolType::Variable || symbol.type == SymbolType::Property) &&
                symbol.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

static ObjectTypeInfo ResolveMemberObjectType(TSNode objectNode, const CallValidationContext& valCtx)
{
    ObjectTypeInfo info;
    const std::string rawObjType =
        CanonicalizeArrayType(ResolveExpressionType(objectNode, ExpressionTypeContext(valCtx.scope, valCtx.ctx)),
                              valCtx.ctx.request.GetEffectiveArrayTypeName());
    if (ExpressionTypeCache::IsPoisonType(rawObjType))
    {
        info.objectType = std::string(ExpressionTypeCache::k_poisonType);
        return info;
    }
    info.objectType = CleanBaseType(rawObjType);
    info.qualifiers.isConst = IsConstTypeView(rawObjType) || IsObjectNodeConst(objectNode, valCtx);

    if (rawObjType.find('<') != std::string::npos && rawObjType.ends_with('>'))
    {
        const size_t openBracket = rawObjType.find('<');
        std::string tmplName = rawObjType.substr(0, openBracket);
        TrimString(tmplName);
        if (valCtx.ctx.request.symbolTable.FindSymbolsPtr(tmplName))
        {
            info.objectType = tmplName;
            std::string argStr = rawObjType.substr(openBracket + 1, rawObjType.size() - openBracket - 2);
            info.templateArgs.push_back(std::move(argStr));
        }
    }
    return info;
}

static bool CheckNonCallableVariable(const std::string& name, const std::string& typeName,
                                     const CallValidationContext& valCtx, CalleeResolution& res)
{
    std::string cleanType = CleanBaseType(typeName);
    if (!cleanType.empty())
    {
        if (auto funcdefSym = FindFuncdefSymbol(cleanType, valCtx.ctx.request.symbolTable))
        {
            res.reportedName = name;
            res.candidates = {FuncdefToFunctionSymbol(*funcdefSym, name)};
            res.candidatesAreFreeFunctions = true;
            res.shouldCheck = true;
            return true;
        }
    }
    auto opCallSyms =
        FindMethodCandidates(cleanType, "opCall", valCtx.ctx.request.symbolTable, valCtx.ctx.request.exprCache);
    if (!opCallSyms.empty())
    {
        res.reportedName = name;
        res.candidates = std::move(opCallSyms);
        res.candidatesAreFreeFunctions = false;
        res.shouldCheck = true;
        return true;
    }
    valCtx.ctx.EmitAtRange(GetCalleeDiagnosticRange(valCtx.callee), diagnostics::codes::CallNoMatchingSignature, name);
    res.shouldCheck = false;
    return true;
}

bool CheckMemberNonCallableVariable(const std::string& objType, const std::string& memberName,
                                    const CallValidationContext& valCtx, CalleeResolution& res)
{
    for (const auto& cls :
         GetInheritedTypeHierarchy(objType, valCtx.ctx.request.symbolTable, valCtx.ctx.request.exprCache))
    {
        if (auto found = valCtx.ctx.request.symbolTable.FindMemberSymbolPtr(cls, memberName))
        {
            for (const auto& sym : *found)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                    std::holds_alternative<VariableSignature>(sym.signature))
                {
                    return CheckNonCallableVariable(memberName, sym.GetVariable().typeName, valCtx, res);
                }
            }
        }
    }
    return false;
}

CalleeResolution ResolveMemberCallee(const CallValidationContext& valCtx)
{
    CalleeResolution res;
    TSNode objectNode = parser::GetChildByField(valCtx.callee, parser::GetGrammarSymbols().fieldObject);
    TSNode memberNode = parser::GetChildByField(valCtx.callee, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
    {
        res.shouldCheck = false;
        return res;
    }

    const auto objInfo = ResolveMemberObjectType(objectNode, valCtx);
    if (objInfo.objectType.empty() || ExpressionTypeCache::IsPoisonType(objInfo.objectType) ||
        !HierarchyIsFullyVisible(objInfo.objectType, valCtx.ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return res;
    }

    res.reportedName = parser::NodeText(memberNode, valCtx.request.sourceCode);
    if (IsConstructorOrDestructorName(res.reportedName, objInfo.objectType))
    {
        const TSPoint ctorStart = ts_node_start_point(memberNode);
        const TSPoint ctorEnd = ts_node_end_point(memberNode);
        valCtx.ctx.EmitAtRange({ctorStart.row, ctorStart.column, ctorEnd.row, ctorEnd.column},
                               "as-err-constructor-not-callable", res.reportedName, objInfo.objectType);
        res.shouldCheck = false;
        return res;
    }

    res.isReceiverConst = objInfo.qualifiers.isConst;
    res.candidates = FindMethodCandidates(objInfo.objectType, res.reportedName, valCtx.ctx.request.symbolTable,
                                          valCtx.ctx.request.exprCache);
    FilterMethodCandidatesConstness(res.candidates, res.isReceiverConst);
    ApplyTemplateSubstitutions(res.candidates, objInfo.objectType, objInfo.templateArgs,
                               valCtx.ctx.request.symbolTable);
    if (res.candidates.empty())
    {
        if (auto callableSym = TryResolveMemberCallableFuncdef(
                objInfo.objectType, res.reportedName, valCtx.ctx.request.symbolTable, valCtx.ctx.request.exprCache))
        {
            res.candidates.push_back(std::move(*callableSym));
        }
        else if (CheckMemberNonCallableVariable(objInfo.objectType, res.reportedName, valCtx, res))
        {
            return res;
        }
    }
    return res;
}

bool CheckLocalNonCallable(const std::string& shortName, const CallValidationContext& valCtx, CalleeResolution& res)
{
    if (valCtx.scope)
    {
        if (const LocalDefinition* shadow =
                ResolveInScopeAt(valCtx.scope, shortName, ts_node_start_byte(valCtx.callee)))
        {
            if (shadow->kind == LocalDefinitionKind::Variable || shadow->kind == LocalDefinitionKind::Parameter)
            {
                return CheckNonCallableVariable(shortName, shadow->typeName, valCtx, res);
            }
        }
    }
    return false;
}

bool CheckScopedNonCallableVariable(std::string_view written, const std::string& shortName,
                                    const CallValidationContext& valCtx, CalleeResolution& res)
{
    if (const auto found = valCtx.ctx.request.symbolTable.FindSymbolsPtr(written))
    {
        for (const auto& sym : *found)
        {
            if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                std::holds_alternative<VariableSignature>(sym.signature))
            {
                return CheckNonCallableVariable(shortName, sym.GetVariable().typeName, valCtx, res);
            }
        }
    }
    return false;
}

bool CheckGlobalNonCallableVariable(std::string_view written, const std::string& shortName,
                                    const CallValidationContext& valCtx, CalleeResolution& res)
{
    const auto reachableScopes = CollectReachableScopes(valCtx.callNode, valCtx.request.sourceCode);
    for (const auto& scopeName : reachableScopes)
    {
        const auto found = scopeName.empty() ? valCtx.ctx.request.symbolTable.FindSymbolsPtr(written)
                                             : valCtx.ctx.request.symbolTable.FindMemberSymbolPtr(scopeName, shortName);
        if (!found)
        {
            continue;
        }
        bool hasVar = false;
        bool hasFunc = false;
        std::string varType;
        for (const auto& sym : *found)
        {
            if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                std::holds_alternative<VariableSignature>(sym.signature))
            {
                hasVar = true;
                varType = sym.GetVariable().typeName;
            }
            if (sym.type == SymbolType::Function)
            {
                hasFunc = true;
            }
        }
        if (hasVar && !hasFunc)
        {
            return CheckNonCallableVariable(shortName, varType, valCtx, res);
        }
    }
    return false;
}

} // namespace angel_lsp::analysis
