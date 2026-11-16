#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ReferenceCountingExprType.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace angel_lsp::analysis
{
bool MatchesMethodArity(const FunctionSignature& sig, size_t argCount) noexcept
{
    const auto arity = InspectFunctionArity(sig);
    return argCount >= arity.requiredParams && (arity.isVariadic || argCount <= arity.maxParams);
}

std::string SubstituteMemberReturnType(std::string_view returnType, std::string_view ownerType,
                                       const SymbolTable& symbolTable)
{
    if (returnType.empty() || ownerType.empty())
    {
        return std::string(returnType);
    }
    const std::string normalized = NormalizeType(ownerType, symbolTable.GetDefaultArrayType());
    if (normalized.find('<') == std::string::npos)
    {
        return std::string(returnType);
    }
    const TemplateBinding binding = BindTemplateArguments(normalized, symbolTable);
    return binding.usable ? SubstituteTemplateParameters(returnType, binding) : std::string(returnType);
}

static void AppendSubstitutedCandidate(const Symbol& sym, MemberCandidateBuffer& buffer)
{
    buffer.originals.push_back(sym);
    Symbol copy = sym;
    for (auto& p : copy.GetFunction().parameters)
    {
        if (!sym.containerName.empty())
        {
            p.typeName = QualifyTypeInContainer(p.typeName, sym.containerName, buffer.symbolTable);
        }
        if (buffer.binding.usable)
        {
            p.typeName = SubstituteTemplateParameters(p.typeName, buffer.binding);
        }
    }
    buffer.substituted.push_back(std::move(copy));
}

static ResolvedOverloadMatch PickZeroArgOverload(const std::vector<Symbol>& originals, bool isReceiverConst)
{
    size_t bestIdx = 0;
    if (!isReceiverConst)
    {
        for (size_t i = 0; i < originals.size(); ++i)
        {
            if (!originals[i].GetFunction().modifiers.isConst)
            {
                bestIdx = i;
                break;
            }
        }
    }
    return {originals[bestIdx], ArgumentConversion{}, true};
}

ResolvedOverloadMatch ResolveMemberOverload(const MemberOverloadQuery& query, const SymbolTable& symbolTable)
{
    if (query.ownerType.empty() || query.methodName.empty())
    {
        return {};
    }
    const std::string normalized = NormalizeType(query.ownerType, symbolTable.GetDefaultArrayType());
    const std::string owner = MemberOwnerType(normalized, symbolTable.GetDefaultArrayType());
    if (owner.empty() || IsPrimitiveTypeName(owner))
    {
        return {};
    }
    const TemplateBinding binding = (normalized.find('<') != std::string::npos)
                                        ? BindTemplateArguments(normalized, symbolTable)
                                        : TemplateBinding{};

    std::vector<Symbol> originals;
    std::vector<Symbol> substituted;
    MemberCandidateBuffer buffer{originals, substituted, binding, symbolTable};
    bool hasConstCandidate = false;
    for (const auto& cls : GetInheritedTypeHierarchy(owner, symbolTable))
    {
        const auto found = symbolTable.FindMemberSymbolPtr(cls, query.methodName);
        if (!found)
        {
            continue;
        }
        for (const auto& sym : *found)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }
            const auto& fn = sym.GetFunction();
            if (fn.modifiers.isDelete || !MatchesMethodArity(fn, query.argTypes.size()))
            {
                continue;
            }
            hasConstCandidate |= fn.modifiers.isConst;
            AppendSubstitutedCandidate(sym, buffer);
        }
    }
    if (originals.empty())
    {
        return {};
    }
    if (query.isReceiverConst && hasConstCandidate)
    {
        std::vector<Symbol> filtOrig;
        std::vector<Symbol> filtSub;
        for (size_t i = 0; i < originals.size(); ++i)
        {
            if (originals[i].GetFunction().modifiers.isConst)
            {
                filtOrig.push_back(std::move(originals[i]));
                filtSub.push_back(std::move(substituted[i]));
            }
        }
        originals = std::move(filtOrig);
        substituted = std::move(filtSub);
    }

    if (query.argTypes.empty())
    {
        return PickZeroArgOverload(originals, query.isReceiverConst);
    }

    const bool allResolved =
        std::none_of(query.argTypes.begin(), query.argTypes.end(), [](std::string_view t) { return t.empty(); });
    if (!allResolved)
    {
        return (originals.size() == 1) ? ResolvedOverloadMatch{originals.front(), ArgumentConversion{}, true}
                                       : ResolvedOverloadMatch{};
    }

    std::vector<const Symbol*> candPtrs;
    candPtrs.reserve(substituted.size());
    for (const auto& s : substituted)
    {
        candPtrs.push_back(&s);
    }
    const OverloadMatchResult match = ResolveBestOverload(candPtrs, query.argTypes, symbolTable);
    const Symbol* chosen = match.bestCandidate;
    if (!chosen && match.isAmbiguous && match.viableCandidates.size() >= 2)
    {
        for (const Symbol* v : match.viableCandidates)
        {
            if (v->GetFunction().modifiers.isConst == query.isReceiverConst)
            {
                chosen = v;
                break;
            }
        }
    }
    if (!chosen)
    {
        return {};
    }
    for (size_t i = 0; i < substituted.size(); ++i)
    {
        if (&substituted[i] == chosen)
        {
            ArgumentConversion conv = !match.bestConversions.empty() ? match.bestConversions[0] : ArgumentConversion{};
            return {originals[i], conv, true};
        }
    }
    return {};
}
ResolvedOverloadMatch ResolveSingleArgOverload(const MemberOverloadQuery& head, std::string_view argType,
                                               const SymbolTable& symbolTable)
{
    const std::string arg(argType);
    MemberOverloadQuery query = head;
    query.argTypes = std::span<const std::string>(&arg, 1);
    return ResolveMemberOverload(query, symbolTable);
}

} // namespace angel_lsp::analysis
