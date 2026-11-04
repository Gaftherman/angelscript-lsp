#include "analysis/resolution/ReferenceReceiver.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
static std::string QualifyReceiver(std::string_view type, std::string_view owner, const SymbolTable& symbols)
{
    return CleanBaseType(QualifyTypeInContainer(type, owner, symbols));
}

static std::string CandidateType(const Symbol& symbol, uint32_t argCount, const SymbolTable& symbols)
{
    if (argCount == UINT32_MAX && symbol.type == SymbolType::Variable)
    {
        return QualifyReceiver(symbol.GetVariable().typeName, symbol.containerName, symbols);
    }
    if (argCount != UINT32_MAX && symbol.type == SymbolType::Function)
    {
        const auto& signature = symbol.GetFunction();
        const auto required =
            static_cast<size_t>(std::count_if(signature.parameters.begin(), signature.parameters.end(),
                                              [](const auto& parameter) { return parameter.defaultValue.empty(); }));
        if (MatchesArgumentCount(argCount, required, signature.parameters.size()))
        {
            return QualifyReceiver(signature.returnType, symbol.containerName, symbols);
        }
    }
    return {};
}

static std::string CommonCandidateType(std::span<const Symbol> candidates, uint32_t argCount,
                                       const SymbolTable& symbols)
{
    std::string result;
    for (const auto& candidate : candidates)
    {
        auto type = CandidateType(candidate, argCount, symbols);
        if (type.empty())
        {
            continue;
        }
        if (!result.empty() && result != type)
        {
            return {};
        }
        result = std::move(type);
    }
    return result;
}

static std::string ResolveMember(const IndexedReceiverExpression& step, const std::string& receiver,
                                 const SymbolTable& symbols)
{
    for (const auto& owner : GetInheritedTypeHierarchy(receiver, symbols))
    {
        if (const auto candidates = symbols.FindMemberSymbolPtr(owner, step.name))
        {
            if (auto type = CommonCandidateType(*candidates, step.argumentCount, symbols); !type.empty())
            {
                return type;
            }
        }
    }
    if (step.argumentCount != UINT32_MAX)
    {
        return {};
    }
    const auto accessors = FindPropertyAccessors(receiver, step.name, symbols, false);
    return CommonCandidateType(accessors, 0, symbols);
}

static std::string ResolveName(const IndexedReceiverExpression& step, const Scope& scope, const SymbolTable& symbols)
{
    if (step.name == "this")
    {
        return std::string(scope.GetEnclosingClass());
    }
    if (step.argumentCount == UINT32_MAX)
    {
        if (const auto* local = ResolveInScopeAt(&scope, step.name, step.startByte))
        {
            return QualifyReceiver(local->typeName, scope.GetEnclosingNamespace(), symbols);
        }
    }

    if (!scope.GetEnclosingClass().empty())
    {
        if (auto type = ResolveMember(step, std::string(scope.GetEnclosingClass()), symbols); !type.empty())
        {
            return type;
        }
    }
    auto qualifier = scope.GetEnclosingNamespace();
    while (!qualifier.empty() && !utils::HasLeadingGlobalPrefix(step.name))
    {
        if (const auto candidates = symbols.FindMemberSymbolPtr(qualifier, step.name))
        {
            return CommonCandidateType(*candidates, step.argumentCount, symbols);
        }
        qualifier = utils::ParentScopeView(qualifier);
    }
    const auto candidates = symbols.FindSymbolsPtr(utils::StripGlobalPrefix(step.name));
    return candidates ? CommonCandidateType(*candidates, step.argumentCount, symbols) : std::string{};
}

std::string ResolveIndexedReferenceReceiver(const LocalReference& reference, const Scope& scope,
                                            const SymbolTable& symbols)
{
    if (!scope.enclosingContext || !scope.enclosingContext->receivers)
    {
        return {};
    }
    const auto& receivers = *scope.enclosingContext->receivers;
    const auto found = receivers.byReference.find(reference.startByte);
    if (found == receivers.byReference.end())
    {
        return {};
    }

    std::vector<uint32_t> chain;
    for (uint32_t id = found->second; id != UINT32_MAX; id = receivers.expressions[id].base)
    {
        chain.push_back(id);
    }
    auto type = ResolveName(receivers.expressions[chain.back()], scope, symbols);
    for (auto it = chain.rbegin() + 1; it != chain.rend() && !type.empty(); ++it)
    {
        type = ResolveMember(receivers.expressions[*it], type, symbols);
    }
    return type;
}
} // namespace angel_lsp::analysis
