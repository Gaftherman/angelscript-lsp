#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/IsolationSiteFacts.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <string_view>

namespace angel_lsp::analysis
{

static bool IsSymbolShared(const Symbol& s)
{
    switch (s.type)
    {
    case SymbolType::Class:
        return s.GetClass().modifiers.isShared;
    case SymbolType::Interface:
        return s.GetInterface().modifiers.isShared;
    case SymbolType::Enum:
        return s.GetEnum().modifiers.isShared;
    case SymbolType::Funcdef:
        return s.GetFuncdef().modifiers.isShared;
    default:
        return true;
    }
}

static bool IsCoreOrRegisteredType(std::string_view typeName, const SemanticAnalysisRequest& request)
{
    return typeName.empty() || IsCorePrimitive(typeName) || typeName == request.GetStringTypeName() ||
           typeName == request.GetArrayTypeName() || request.IsRegisteredSymbol(typeName);
}

static bool IsAllowedCallee(std::string_view calleeName, std::string_view enclosingClass,
                            const SemanticAnalysisRequest& request)
{
    if (!enclosingClass.empty())
    {
        if (request.symbolTable.FindMemberSymbolPtr(enclosingClass, calleeName) != nullptr)
        {
            return true;
        }
    }
    return request.IsRegisteredSymbol(calleeName);
}

void CheckSharedDatatype(const IsolationTypeSiteFact& fact, DiagnosticContext& ctx)
{
    if (!fact.inSharedContext)
    {
        return;
    }

    const std::string_view typeName = fact.cleanName;
    if (IsCoreOrRegisteredType(typeName, ctx.request))
    {
        return;
    }

    auto syms = ctx.request.symbolTable.FindSymbolsPtr(typeName);
    if (!syms)
    {
        return;
    }

    for (const auto& s : *syms)
    {
        if (s.type != SymbolType::Class && s.type != SymbolType::Interface && s.type != SymbolType::Enum &&
            s.type != SymbolType::Funcdef)
        {
            continue;
        }

        if (!IsSymbolShared(s) && !IsFromPredefinedStub(s, ctx))
        {
            ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
            ctx.EmitAtRange(fact.range, "as-err-shared-cannot-access-non-shared", typeName);
            break;
        }
    }
}

void CheckSharedCallExpression(const IsolationCallSiteFact& fact, DiagnosticContext& ctx)
{
    if (!fact.inSharedContext || !fact.isDirectOrScoped)
    {
        return;
    }

    const std::string_view calleeName = fact.calleeText;
    if (calleeName.empty() || IsAllowedCallee(calleeName, fact.enclosingClass, ctx.request))
    {
        return;
    }

    if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(calleeName))
    {
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Function && s.containerName.empty())
            {
                if (!s.GetFunction().modifiers.isShared && !IsFromPredefinedStub(s, ctx))
                {
                    ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
                    ctx.EmitAtRange(fact.calleeRange, "as-err-shared-cannot-access-non-shared", calleeName);
                    break;
                }
            }
        }
    }
}

} // namespace angel_lsp::analysis
