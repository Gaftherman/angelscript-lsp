#include "analysis/resolution/ReferenceCountingReceiver.h"

#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/ReferenceCountingAccess.h"
#include "analysis/resolution/ReferenceCountingScope.h"
#include "analysis/resolution/ReferenceReceiver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
static std::string ResolveObjectTextType(std::string_view oText, const Scope* scope, const SymbolTable& symbolTable,
                                         std::string_view enclosingClass)
{
    if (oText == "this")
    {
        return std::string(enclosingClass);
    }
    const LocalDefinition* oDef = ResolveInScope(scope, oText);
    if (oDef && !oDef->typeName.empty())
    {
        return CleanBaseType(oDef->typeName);
    }
    if (const auto gSyms = symbolTable.FindSymbolsPtr(oText))
    {
        for (const auto& gs : *gSyms)
        {
            if (gs.type == SymbolType::Variable && !gs.GetVariable().typeName.empty())
            {
                return CleanBaseType(gs.GetVariable().typeName);
            }
        }
    }
    return "";
}

static std::string TryResolveReceiverFastPath(const LocalReference& ref, const ReferenceCollectionCriteria& criteria,
                                              const Scope* scope, std::string_view encClass)
{
    if (ref.receiverStartByte < ref.receiverEndByte && ref.receiverEndByte <= criteria.request.sourceCode.size() &&
        !ref.isScopedIdentifier)
    {
        std::string_view oText =
            criteria.request.sourceCode.substr(ref.receiverStartByte, ref.receiverEndByte - ref.receiverStartByte);
        std::string_view enc = (oText == "this") ? encClass : std::string_view{};
        return ResolveObjectTextType(oText, scope, criteria.symbolTable, enc);
    }
    return "";
}

static std::string ResolveReceiverFromScope(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                            const ReferenceCollectionCriteria& criteria)
{
    std::string currentReceiverType;
    for (const auto& candRef : scope->references)
    {
        if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
        {
            if (currentReceiverType.empty())
            {
                std::string_view enc =
                    (candRef.name == "this")
                        ? GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine, criteria.classCache)
                        : std::string_view{};
                currentReceiverType = ResolveObjectTextType(candRef.name, scope, criteria.symbolTable, enc);
            }
            else
            {
                std::string memberType =
                    ResolveReceiverMemberType(currentReceiverType, candRef.name, criteria.symbolTable);
                if (!memberType.empty())
                {
                    currentReceiverType = std::move(memberType);
                }
                else
                {
                    std::string_view enc =
                        (candRef.name == "this")
                            ? GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine, criteria.classCache)
                            : std::string_view{};
                    std::string t = ResolveObjectTextType(candRef.name, scope, criteria.symbolTable, enc);
                    if (!t.empty())
                    {
                        currentReceiverType = std::move(t);
                    }
                }
            }
        }
    }
    return currentReceiverType;
}

bool IsCompatibleMemberAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                              const ReferenceCollectionCriteria& criteria)
{
    std::string rType = ResolveIndexedReferenceReceiver(ref, *scope, criteria.symbolTable);
    if (rType.empty() && fileUri == criteria.request.uri)
    {
        std::string_view encClass =
            GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine, criteria.classCache);
        rType = TryResolveReceiverFastPath(ref, criteria, scope, encClass);
    }
    if (rType.empty())
    {
        rType = ResolveReceiverFromScope(fileUri, ref, scope, criteria);
    }
    return !rType.empty() && criteria.compatibleClasses.contains(rType);
}
} // namespace angel_lsp::analysis
