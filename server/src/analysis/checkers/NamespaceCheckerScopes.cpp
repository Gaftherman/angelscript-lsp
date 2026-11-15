#include "analysis/checkers/NamespaceCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/SymbolTable.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

bool HasContainerOrAccessorMember(std::string_view name, const rules::ContainerMembers& members, bool reqKeyword)
{
    const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
    return accSet.contains(name) || members.allMemberNames.contains(name);
}

/**
 * @brief Checks whether any namespace in the RuleIndex is a sub-namespace of prefix.
 */
static bool HasNamespaceWithPrefix(const rules::RuleIndex& ruleIndex, std::string_view prefix)
{
    for (const auto& [nsName, _] : ruleIndex.byNamespace)
    {
        if (utils::HasScopePrefix(nsName, prefix))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if an enum symbol declares the given enumerator name.
 */
static bool EnumHasMember(const Symbol& sym, std::string_view member)
{
    if (sym.type != SymbolType::Enum || !std::holds_alternative<EnumSignature>(sym.signature))
    {
        return false;
    }
    const auto& members = sym.GetEnum().members;
    return std::any_of(members.begin(), members.end(),
                       [member](const EnumMemberInformation& entry) { return entry.name == member; });
}

/**
 * @brief Checks if SymbolTable has a non-call-reference member stored directly under container.
 */
static bool HasNonReferenceMemberSymbol(const SymbolTable& table, std::string_view container, std::string_view member)
{
    const auto found = table.FindMemberSymbolPtr(container, member);
    if (!found)
    {
        return false;
    }
    return std::any_of(found->begin(), found->end(),
                       [](const Symbol& sym) { return sym.type != SymbolType::CallReference; });
}

/**
 * @brief Builds candidate full qualifier paths for a written qualifier at byteOffset.
 */
static std::vector<std::string> BuildQualifierCandidates(const QualifiedLookupRequest& req)
{
    std::vector<std::string> candidates;
    candidates.emplace_back(req.qualifier);
    if (req.hasLeadingGlobal || !req.nodeIndex)
    {
        return candidates;
    }

    for (const auto& container : req.nodeIndex->CollectEnclosingContainers(req.startByte, req.sourceCode))
    {
        if (container.kind != ContainerKind::Namespace)
        {
            continue;
        }
        std::string_view path = container.qualifiedName.empty() ? std::string_view(container.name)
                                                                : std::string_view(container.qualifiedName);
        for (; !path.empty(); path = utils::ParentScopeView(path))
        {
            candidates.push_back(utils::JoinScopedName(path, req.qualifier));
        }
    }
    for (const auto& imported : req.nodeIndex->CollectUsingNamespaces(req.sourceCode, req.startByte))
    {
        candidates.push_back(utils::JoinScopedName(imported, req.qualifier));
    }
    return candidates;
}

/**
 * @brief Checks if a byte offset lies inside a mixin class or interface declaration.
 */
static bool IsOffsetInsideMixin(uint32_t byteOffset, const ScopedIdentifierCheckContext& sctx, const SymbolTable& table)
{
    if (!sctx.nodeIndex)
    {
        return false;
    }
    for (const auto& container : sctx.nodeIndex->CollectEnclosingContainers(byteOffset, sctx.sourceCode))
    {
        if (container.kind != ContainerKind::Class)
        {
            continue;
        }
        const auto symbols = table.FindSymbolsPtr(container.qualifiedName);
        if (symbols && std::any_of(symbols->begin(), symbols->end(), [](const Symbol& sym)
                                   { return sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin; }))
        {
            return true;
        }
    }
    return false;
}

QualifiedLookupStatus ResolveQualifiedMember(const QualifiedLookupRequest& req, const DiagnosticContext& ctx)
{
    if (req.qualifier.empty())
    {
        return QualifiedLookupStatus::Resolved;
    }
    if (ctx.request.IsRegisteredSymbol(req.qualifier))
    {
        return QualifiedLookupStatus::Resolved;
    }
    if (!req.member.empty() && ctx.request.IsRegisteredSymbol(utils::JoinScopedName(req.qualifier, req.member)))
    {
        return QualifiedLookupStatus::Resolved;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    const bool reqKeyword = ctx.request.RequiresAccessorKeyword();
    bool anyMatched = false;

    for (const std::string& candQual : BuildQualifierCandidates(req))
    {
        const auto qualSyms = table.FindSymbolsPtr(candQual);
        if (qualSyms)
        {
            for (const auto& sym : *qualSyms)
            {
                if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                    sym.type == SymbolType::Typedef)
                {
                    return QualifiedLookupStatus::Resolved;
                }
                if (sym.type == SymbolType::Enum)
                {
                    anyMatched = true;
                    if (!req.checkMember || EnumHasMember(sym, req.member))
                    {
                        return QualifiedLookupStatus::Resolved;
                    }
                }
            }
        }

        const auto nsIt = ruleIndex.byNamespace.find(candQual);
        const bool hasNsSymbol = qualSyms && std::any_of(qualSyms->begin(), qualSyms->end(), [](const Symbol& s)
                                                         { return s.type == SymbolType::Namespace; });
        const bool hasNsPrefix =
            nsIt == ruleIndex.byNamespace.end() && !hasNsSymbol && HasNamespaceWithPrefix(ruleIndex, candQual);
        if (nsIt != ruleIndex.byNamespace.end() || hasNsSymbol || hasNsPrefix)
        {
            anyMatched = true;
            if (!req.checkMember)
            {
                return QualifiedLookupStatus::Resolved;
            }
            if (nsIt != ruleIndex.byNamespace.end() &&
                HasContainerOrAccessorMember(req.member, nsIt->second, reqKeyword))
            {
                return QualifiedLookupStatus::Resolved;
            }
            if (HasNonReferenceMemberSymbol(table, candQual, req.member))
            {
                return QualifiedLookupStatus::Resolved;
            }
        }
    }

    return anyMatched ? QualifiedLookupStatus::UndefinedMember : QualifiedLookupStatus::UnknownQualifier;
}

void CheckScopedIdentifier(const ScopedIdentifierFact& fact, const ScopedIdentifierCheckContext& sctx,
                           DiagnosticContext& ctx)
{
    if (fact.isInsideError || fact.usage == ScopedIdentifierUsage::NamespaceDeclaration ||
        fact.usage == ScopedIdentifierUsage::CallCallee)
    {
        return;
    }

    const bool checkMember = (fact.usage == ScopedIdentifierUsage::Expression);
    const std::string compactFull = utils::CompactScopedName(fact.qualifiedText);
    const std::string compactQualifier = utils::CompactScopedName(fact.qualifier);
    const std::string_view cleanFull = utils::StripGlobalPrefix(compactFull);
    const std::string_view targetQualifier = checkMember ? std::string_view(compactQualifier) : cleanFull;
    if (targetQualifier.empty())
    {
        return;
    }

    const QualifiedLookupRequest req{.qualifier = targetQualifier,
                                     .member = checkMember ? fact.memberName : std::string_view{},
                                     .startByte = fact.bytes.startByte,
                                     .hasLeadingGlobal = fact.hasLeadingGlobal,
                                     .checkMember = checkMember,
                                     .sourceCode = sctx.sourceCode,
                                     .nodeIndex = sctx.nodeIndex};
    const QualifiedLookupStatus status = ResolveQualifiedMember(req, ctx);

    if (status == QualifiedLookupStatus::UnknownQualifier)
    {
        const SourceRange& range = checkMember ? fact.qualifierRange : fact.range;
        ctx.EmitAtRange(range, "as-err-undefined-namespace", targetQualifier, DiagnosticSeverity::Error);
    }
    else if (status == QualifiedLookupStatus::UndefinedMember &&
             !IsOffsetInsideMixin(fact.bytes.startByte, sctx, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(fact.range, "as-err-undefined-identifier", cleanFull, DiagnosticSeverity::Error);
    }
}

void CheckImportBody(const ImportDeclarationFact& fact, DiagnosticContext& ctx)
{
    if (fact.hasBodyBlock)
    {
        ctx.EmitAtRange(fact.range, "as-err-import-has-body", "import", DiagnosticSeverity::Error);
    }
}

} // namespace angel_lsp::analysis
