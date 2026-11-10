#include "analysis/index/NamespaceUsageFacts.h"

#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint s = ts_node_start_point(node);
    const TSPoint e = ts_node_end_point(node);
    return {s.row, s.column, e.row, e.column};
}

/**
 * @brief Converts a node to its byte span.
 * @param[in] node Syntax node.
 * @return Start and end byte of the node, or a zero range if null.
 */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

static void CollectScopedIdentifiers(const NodeIndex& index, std::string_view source, NamespaceUsageFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode node : index.Nodes(gs.symScopedIdentifier))
    {
        const uint32_t namedCount = ts_node_named_child_count(node);
        if (ts_node_child_count(node) < 2 || namedCount == 0)
        {
            continue;
        }
        const std::string_view text = parser::NodeText(node, source);
        const bool hasGlobal =
            utils::HasLeadingGlobalPrefix(text) ||
            (!ts_node_is_null(ts_node_child(node, 0)) && ts_node_symbol(ts_node_child(node, 0)) == gs.tokColonColon);

        if (namedCount >= 2)
        {
            TSNode firstChild = ts_node_named_child(node, 0);
            TSNode lastChild = ts_node_named_child(node, namedCount - 1);
            if (ts_node_is_null(firstChild) || ts_node_is_null(lastChild))
            {
                continue;
            }
            facts.scopedIdentifiers.push_back(
                ScopedIdentifierFact{.bytes = BytesOf(node),
                                     .range = RangeOf(node),
                                     .qualifiedText = text,
                                     .prefixBytes = BytesOf(firstChild),
                                     .prefixRange = RangeOf(firstChild),
                                     .prefix = utils::TrimView(parser::NodeText(firstChild, source)),
                                     .memberBytes = BytesOf(lastChild),
                                     .memberRange = RangeOf(lastChild),
                                     .memberName = utils::TrimView(parser::NodeText(lastChild, source)),
                                     .hasLeadingGlobal = hasGlobal});
        }
        else if (hasGlobal && namedCount == 1)
        {
            TSNode memberNode = ts_node_named_child(node, 0);
            if (ts_node_is_null(memberNode))
            {
                continue;
            }
            const SourceRange fullRange = RangeOf(node);
            facts.scopedIdentifiers.push_back(
                ScopedIdentifierFact{.bytes = BytesOf(node),
                                     .range = fullRange,
                                     .qualifiedText = text,
                                     .prefixBytes = {ts_node_start_byte(node), ts_node_start_byte(node) + 2},
                                     .prefixRange = {fullRange.startLine, fullRange.startCharacter, fullRange.startLine,
                                                     fullRange.startCharacter + 2},
                                     .prefix = {},
                                     .memberBytes = BytesOf(memberNode),
                                     .memberRange = RangeOf(memberNode),
                                     .memberName = utils::TrimView(parser::NodeText(memberNode, source)),
                                     .hasLeadingGlobal = true});
        }
    }
}

static void CollectScopedTypes(const NodeIndex& index, std::string_view source, NamespaceUsageFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode scopeNode : index.Nodes(gs.symScope))
    {
        TSNode parent = parser::GetParent(scopeNode);
        if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symType || ts_node_named_child_count(parent) < 2)
        {
            continue;
        }
        TSNode firstChild = ts_node_named_child(parent, 0);
        TSNode secondChild = ts_node_named_child(parent, 1);
        if (ts_node_symbol(firstChild) != gs.symScope || ts_node_symbol(secondChild) != gs.symDatatype)
        {
            continue;
        }

        std::string_view scopeText =
            utils::StripGlobalPrefix(utils::StripTrailingScopeSeparator(parser::NodeText(firstChild, source)));
        while (scopeText.ends_with(':'))
        {
            scopeText.remove_suffix(1);
        }
        const std::string_view memberText = parser::NodeText(secondChild, source);
        if (scopeText.empty() || memberText.empty())
        {
            continue;
        }

        TSNode scopeId = ts_node_named_child(firstChild, 0);
        if (!ts_node_is_null(scopeId) && ts_node_named_child_count(scopeId) > 0)
        {
            scopeId = ts_node_named_child(scopeId, ts_node_named_child_count(scopeId) - 1);
        }
        TSNode memberId = ts_node_named_child(secondChild, 0);

        facts.scopedTypes.push_back(
            ScopedTypeFact{.bytes = BytesOf(parent),
                           .range = RangeOf(parent),
                           .qualifiedText = utils::TrimView(parser::NodeText(parent, source)),
                           .prefixBytes = ts_node_is_null(scopeId) ? BytesOf(firstChild) : BytesOf(scopeId),
                           .prefixRange = ts_node_is_null(scopeId) ? RangeOf(firstChild) : RangeOf(scopeId),
                           .prefix = scopeText,
                           .memberBytes = ts_node_is_null(memberId) ? BytesOf(secondChild) : BytesOf(memberId),
                           .memberRange = ts_node_is_null(memberId) ? RangeOf(secondChild) : RangeOf(memberId),
                           .memberName = utils::TrimView(memberText),
                           .hasLeadingGlobal = utils::HasLeadingGlobalPrefix(parser::NodeText(parent, source))});
    }
}

static void CollectDeclarations(const NodeIndex& index, std::string_view source, NamespaceUsageFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode node : index.Nodes(gs.symNamespaceDeclaration))
    {
        const SyntaxOperands op(node, &index);
        TSNode nameNode = op.FieldOrNamed(gs.fieldName);
        TSNode bodyNode = op.FieldOrNamed(gs.fieldBody);
        facts.namespaceDeclarations.push_back(
            NamespaceDeclarationFact{.bytes = BytesOf(node),
                                     .range = RangeOf(node),
                                     .nameBytes = BytesOf(nameNode),
                                     .nameRange = RangeOf(nameNode),
                                     .name = utils::TrimView(parser::NodeText(nameNode, source)),
                                     .bodyBytes = BytesOf(bodyNode),
                                     .bodyRange = RangeOf(bodyNode)});
    }
    for (TSNode node : index.Nodes(gs.symUsingDeclaration))
    {
        TSNode nameNode = SyntaxOperands(node, &index).FieldOrNamed(gs.fieldName);
        facts.usingDeclarations.push_back(
            UsingDeclarationFact{.bytes = BytesOf(node),
                                 .range = RangeOf(node),
                                 .nameBytes = BytesOf(nameNode),
                                 .nameRange = RangeOf(nameNode),
                                 .namespaceName = utils::TrimView(parser::NodeText(nameNode, source))});
    }
}

static void CollectImportDeclarations(const NodeIndex& index, std::string_view source, NamespaceUsageFacts& facts)
{
    const auto checkNode = [&](TSNode node, bool isError)
    {
        const std::string_view text = parser::NodeText(node, source);
        const size_t importPos = text.find("import ");
        if (importPos == std::string_view::npos)
            return;
        const size_t fromPos = text.find("from", importPos);
        const bool hasBody = fromPos != std::string_view::npos && text.find('{', fromPos) != std::string_view::npos;
        if (!isError || hasBody)
        {
            facts.importDeclarations.push_back(ImportDeclarationFact{.bytes = BytesOf(node),
                                                                     .range = RangeOf(node),
                                                                     .text = text,
                                                                     .hasBodyBlock = hasBody,
                                                                     .isSyntaxError = isError});
        }
    };
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode node : index.Nodes(gs.symImportDeclaration))
        checkNode(node, false);
    for (TSNode node : index.Nodes(gs.symError))
        checkNode(node, true);
}

static void CollectAllContainers(const NodeIndex& index, std::string_view source,
                                 std::vector<EnclosingContainerFact>& containers)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto pushContainer = [&](TSNode node, ContainerKind kind, bool isMixin)
    {
        TSNode nameNode = SyntaxOperands(node, &index).FieldOrNamed(gs.fieldName);
        containers.push_back(EnclosingContainerFact{.bytes = BytesOf(node),
                                                    .range = RangeOf(node),
                                                    .name = utils::TrimView(parser::NodeText(nameNode, source)),
                                                    .kind = kind,
                                                    .isMixin = isMixin});
    };
    for (TSNode node : index.Nodes(gs.symNamespaceDeclaration))
        pushContainer(node, ContainerKind::Namespace, false);
    for (TSNode node : index.Nodes(gs.symClassDeclaration))
        pushContainer(node, ContainerKind::Class, !ts_node_is_null(parser::FindChildWithSymbol(node, gs.tokMixin)));
    for (TSNode node : index.Nodes(gs.symMixinDeclaration))
        pushContainer(node, ContainerKind::Class, true);
    for (TSNode node : index.Nodes(gs.symInterfaceDeclaration))
        pushContainer(node, ContainerKind::Interface, false);
}

static void CollectIdentifierUseSites(const NodeIndex& index, std::string_view source,
                                      const std::vector<EnclosingContainerFact>& allContainers,
                                      NamespaceUsageFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode node : index.Nodes(gs.symIdentifier))
    {
        const uint32_t startByte = ts_node_start_byte(node);
        const uint32_t endByte = ts_node_end_byte(node);
        const uint32_t containerBegin = static_cast<uint32_t>(facts.enclosingContainers.size());

        bool inMixin = false;
        std::vector<EnclosingContainerFact> matched;
        for (const auto& c : allContainers)
        {
            if (c.bytes.startByte <= startByte && endByte <= c.bytes.endByte)
            {
                matched.push_back(c);
                if (c.isMixin)
                    inMixin = true;
            }
        }
        std::ranges::sort(matched, [](const EnclosingContainerFact& a, const EnclosingContainerFact& b)
                          { return (a.bytes.endByte - a.bytes.startByte) < (b.bytes.endByte - b.bytes.startByte); });

        for (const auto& c : matched)
            facts.enclosingContainers.push_back(c);

        facts.identifierUseSites.push_back(IdentifierUseSiteFact{
            .bytes = BytesOf(node),
            .range = RangeOf(node),
            .name = utils::TrimView(parser::NodeText(node, source)),
            .isInsideError = index.IsInsideError(startByte),
            .isInsideMixin = inMixin,
            .containerBegin = containerBegin,
            .containerCount = static_cast<uint32_t>(facts.enclosingContainers.size() - containerBegin)});
    }
}

NamespaceUsageFacts BuildNamespaceUsageFacts(const NodeIndex& index, std::string_view source)
{
    NamespaceUsageFacts facts;
    CollectScopedIdentifiers(index, source, facts);
    CollectScopedTypes(index, source, facts);
    CollectDeclarations(index, source, facts);
    CollectImportDeclarations(index, source, facts);

    std::vector<EnclosingContainerFact> allContainers;
    CollectAllContainers(index, source, allContainers);
    CollectIdentifierUseSites(index, source, allContainers, facts);

    SortBySpan(facts.scopedIdentifiers);
    SortBySpan(facts.scopedTypes);
    SortBySpan(facts.namespaceDeclarations);
    SortBySpan(facts.usingDeclarations);
    SortBySpan(facts.importDeclarations);
    SortBySpan(facts.identifierUseSites);
    return facts;
}

} // namespace angel_lsp::analysis
