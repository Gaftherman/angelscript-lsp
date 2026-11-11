#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

bool IsObjectOperandConst(const BinaryOperandFact& objectOperand, const Scope* scope, const SymbolTable& symbolTable)
{
    if (objectOperand.text.empty())
    {
        return false;
    }

    if (scope)
    {
        if (const auto* def = ResolveInScopeAt(scope, objectOperand.text, objectOperand.bytes.startByte))
        {
            if (!def->typeName.empty() && HasConstModifier(def->typeName))
            {
                return true;
            }
        }
    }

    if (const auto syms = symbolTable.FindSymbolsPtr(objectOperand.text))
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

static ObjectTypeInfo ResolveMemberObjectType(const CallSiteFact& site, const Scope* scope,
                                              const DiagnosticContext& ctx)
{
    ObjectTypeInfo info;
    const std::string rawObjType =
        CanonicalizeArrayType(ResolveExpressionType(site.objectOperand.bytes, ExpressionTypeContext(scope, ctx)),
                              ctx.request.GetEffectiveArrayTypeName());
    if (ExpressionTypeCache::IsPoisonType(rawObjType))
    {
        info.objectType = std::string(ExpressionTypeCache::k_poisonType);
        return info;
    }
    info.objectType = CleanBaseType(rawObjType);
    const bool isObjConst = site.isThisCall ? site.isEnclosingMethodConst
                                            : IsObjectOperandConst(site.objectOperand, scope, ctx.request.symbolTable);
    info.qualifiers.isConst = IsConstTypeView(rawObjType) || isObjConst;

    if (rawObjType.find('<') != std::string_view::npos && rawObjType.ends_with('>'))
    {
        const size_t openBracket = rawObjType.find('<');
        std::string_view tmplView(rawObjType.data(), openBracket);
        TrimTypeWhitespace(tmplView);
        if (ctx.request.symbolTable.FindSymbolsPtr(tmplView))
        {
            info.objectType = std::string(tmplView);
            info.templateArgs.push_back(rawObjType.substr(openBracket + 1, rawObjType.size() - openBracket - 2));
        }
    }
    return info;
}

static bool CheckNonCallableVariable(const NonCallableCheckRequest& req, DiagnosticContext& ctx, CalleeResolution& res)
{
    const std::string_view cleanType = CleanBaseTypeView(req.typeName);
    if (!cleanType.empty())
    {
        if (auto funcdefSym = FindFuncdefSymbol(cleanType, ctx.request.symbolTable))
        {
            res.reportedName = req.name;
            res.candidates = {FuncdefToFunctionSymbol(*funcdefSym, req.name)};
            res.candidatesAreFreeFunctions = true;
            res.shouldCheck = true;
            return true;
        }
    }
    auto opCallSyms = FindMethodCandidates(cleanType, "opCall", ctx.request.symbolTable, ctx.request.exprCache);
    if (!opCallSyms.empty())
    {
        res.reportedName = req.name;
        res.candidates = std::move(opCallSyms);
        res.candidatesAreFreeFunctions = false;
        res.shouldCheck = true;
        return true;
    }
    ctx.EmitAtRange(req.calleeRange, diagnostics::codes::CallNoMatchingSignature, req.name);
    res.shouldCheck = false;
    return true;
}

bool CheckMemberNonCallableVariable(const CallSiteFact& site, std::string_view objType, DiagnosticContext& ctx,
                                    CalleeResolution& res)
{
    for (const auto& cls : GetInheritedTypeHierarchy(objType, ctx.request.symbolTable, ctx.request.exprCache))
    {
        if (auto found = ctx.request.symbolTable.FindMemberSymbolPtr(cls, site.calleeName))
        {
            for (const auto& sym : *found)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                    std::holds_alternative<VariableSignature>(sym.signature))
                {
                    return CheckNonCallableVariable(
                        NonCallableCheckRequest{site.calleeName, sym.GetVariable().typeName, site.calleeRange}, ctx,
                        res);
                }
            }
        }
    }
    return false;
}

CalleeResolution ResolveMemberCallee(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    CalleeResolution res;
    if (site.objectOperand.bytes.Empty() || site.calleeName.empty())
    {
        res.shouldCheck = false;
        return res;
    }

    const auto objInfo = ResolveMemberObjectType(site, scope, ctx);
    if (objInfo.objectType.empty() || ExpressionTypeCache::IsPoisonType(objInfo.objectType) ||
        !HierarchyIsFullyVisible(objInfo.objectType, ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return res;
    }

    res.reportedName = site.calleeName;
    if (IsConstructorOrDestructorName(res.reportedName, objInfo.objectType))
    {
        ctx.EmitAtRange(site.calleeNameRange, "as-err-constructor-not-callable", res.reportedName, objInfo.objectType);
        res.shouldCheck = false;
        return res;
    }

    res.isReceiverConst = objInfo.qualifiers.isConst;
    res.candidates =
        FindMethodCandidates(objInfo.objectType, res.reportedName, ctx.request.symbolTable, ctx.request.exprCache);
    FilterMethodCandidatesConstness(res.candidates, res.isReceiverConst);
    ApplyTemplateSubstitutions(res.candidates, objInfo.objectType, objInfo.templateArgs, ctx.request.symbolTable);
    if (res.candidates.empty())
    {
        if (auto callableSym = TryResolveMemberCallableFuncdef(objInfo.objectType, res.reportedName,
                                                               ctx.request.symbolTable, ctx.request.exprCache))
        {
            res.candidates.push_back(std::move(*callableSym));
        }
        else if (CheckMemberNonCallableVariable(site, objInfo.objectType, ctx, res))
        {
            return res;
        }
    }
    return res;
}

bool CheckLocalNonCallable(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx, CalleeResolution& res)
{
    if (scope)
    {
        if (const LocalDefinition* shadow = ResolveInScopeAt(scope, site.calleeName, site.calleeNameBytes.startByte))
        {
            if (shadow->kind == LocalDefinitionKind::Variable || shadow->kind == LocalDefinitionKind::Parameter)
            {
                return CheckNonCallableVariable(
                    NonCallableCheckRequest{site.calleeName, shadow->typeName, site.calleeRange}, ctx, res);
            }
        }
    }
    return false;
}

bool CheckScopedNonCallableVariable(const CallSiteFact& site, DiagnosticContext& ctx, CalleeResolution& res)
{
    if (const auto found = ctx.request.symbolTable.FindSymbolsPtr(site.calleeText))
    {
        for (const auto& sym : *found)
        {
            if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                std::holds_alternative<VariableSignature>(sym.signature))
            {
                return CheckNonCallableVariable(
                    NonCallableCheckRequest{site.calleeName, sym.GetVariable().typeName, site.calleeRange}, ctx, res);
            }
        }
    }
    return false;
}

bool CheckGlobalNonCallableVariable(const CallSiteFact& site, DiagnosticContext& ctx, CalleeResolution& res)
{
    const auto reachableScopes =
        CollectReachableScopes(site.bytes.startByte, ctx.request.sourceCode, ctx.request.nodeIndex);
    for (const auto& scopeName : reachableScopes)
    {
        const auto found = scopeName.empty() ? ctx.request.symbolTable.FindSymbolsPtr(site.calleeText)
                                             : ctx.request.symbolTable.FindMemberSymbolPtr(scopeName, site.calleeName);
        if (!found)
        {
            continue;
        }
        bool hasVar = false;
        bool hasFunc = false;
        std::string_view varType;
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
            return CheckNonCallableVariable(NonCallableCheckRequest{site.calleeName, varType, site.calleeRange}, ctx,
                                            res);
        }
    }
    return false;
}

} // namespace angel_lsp::analysis
