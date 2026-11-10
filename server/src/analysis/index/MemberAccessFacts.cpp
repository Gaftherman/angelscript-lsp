#include "analysis/index/MemberAccessFacts.h"

#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"

#include <cctype>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

const MemberExpressionFact* MemberAccessFacts::FindMemberExpression(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(memberExpressions, bytes);
}

const MemberAccessIdentifierFact* MemberAccessFacts::FindIdentifier(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(identifiers, bytes);
}

const ImplicitMemberAccessFact* MemberAccessFacts::FindImplicitAccess(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(implicitAccesses, bytes);
}

const LambdaEnclosureFact* MemberAccessFacts::FindLambdaEnclosure(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(lambdaEnclosures, bytes);
}

/** @brief Converts a syntax node to its source range coordinates. */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/** @brief Converts a syntax node to its byte span. */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

/** @brief Extracts trimmed text from an identifier node without memory allocation. */
static std::string_view ExtractCleanIdentifier(TSNode node, std::string_view source) noexcept
{
    std::string_view text = parser::NodeText(node, source);
    while (!text.empty() && isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    return text;
}

/** @brief Checks if a grammar symbol represents a type reference or comment. */
static bool IsTypeRefOrComment(TSSymbol sym) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    return sym == gs.symDatatype || sym == gs.symPrimitiveType || sym == gs.symBaseClassList || sym == gs.symComment;
}

/** @brief Checks whether an identifier node should be skipped during access validation. */
static bool ShouldSkipIdentifierNode(TSNode node, TSNode parent) noexcept
{
    if (ts_node_is_null(parent))
    {
        return true;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symMemberExpression)
    {
        return ts_node_eq(node, parser::GetChildByField(parent, gs.fieldMember));
    }
    if (parser::IsDeclarationSymbol(parentSym))
    {
        return ts_node_eq(node, parser::GetChildByField(parent, gs.fieldName));
    }
    if (IsTypeRefOrComment(parentSym))
    {
        return true;
    }
    return ts_node_symbol(node) == gs.symIdentifier && parentSym == gs.symScopedIdentifier;
}

/** @brief Finds the innermost class enclosing a byte offset, respecting lambda closure barriers. */
static std::string_view EnclosingClass(uint32_t byteOffset, std::string_view source, bool& insideMixin,
                                       const NodeIndex& index) noexcept
{
    insideMixin = false;
    const auto* span = index.FindInnermostClassSpan(byteOffset);
    if (span && span->nameStartByte < span->nameEndByte && span->nameEndByte <= source.size())
    {
        insideMixin = span->isMixin;
        return source.substr(span->nameStartByte, span->nameEndByte - span->nameStartByte);
    }
    return {};
}

/** @brief Finds the outer class enclosing a lambda expression at the given byte offset. */
static std::string_view EnclosingClassOfLambda(uint32_t byteOffset, std::string_view source,
                                               const NodeIndex& index) noexcept
{
    const TSNode classNode = index.FindEnclosingClassOrMixin(byteOffset);
    if (!ts_node_is_null(classNode))
    {
        const TSNode nameNode = parser::GetChildByField(classNode, parser::GetGrammarSymbols().fieldName);
        return ts_node_is_null(nameNode) ? std::string_view{} : parser::NodeText(nameNode, source);
    }
    return {};
}

/** @brief Builds an identifier fact with its enclosing class and lambda closure status. */
static MemberAccessIdentifierFact MakeIdentifierFact(TSNode node, std::string_view name, const NodeIndex& index,
                                                     std::string_view source) noexcept
{
    const uint32_t startByte = ts_node_start_byte(node);
    bool insideMixin = false;
    const std::string_view encClass = EnclosingClass(startByte, source, insideMixin, index);
    return {BytesOf(node),
            RangeOf(node),
            name,
            (name == "this" || name == "super"),
            index.IsInsideLambdaClosure(startByte),
            encClass,
            insideMixin,
            EnclosingClassOfLambda(startByte, source, index)};
}

/** @brief Collects all member expressions from the syntax index. */
static void CollectMemberExpressions(const NodeIndex& index, std::string_view source, MemberAccessFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode node : index.Nodes(gs.symMemberExpression))
    {
        const SyntaxOperands syntax(node, &index);
        const TSNode objectNode = syntax.Field(gs.fieldObject);
        const TSNode memberNode = syntax.Field(gs.fieldMember);
        if (ts_node_is_null(objectNode) || ts_node_is_null(memberNode))
        {
            continue;
        }

        const std::string_view memberName = parser::NodeText(memberNode, source);
        if (memberName.empty())
        {
            continue;
        }

        MemberExpressionFact fact{BytesOf(node),
                                  RangeOf(node),
                                  BytesOf(objectNode),
                                  RangeOf(objectNode),
                                  parser::NodeText(objectNode, source),
                                  BytesOf(memberNode),
                                  RangeOf(memberNode),
                                  memberName,
                                  {},
                                  false,
                                  (parser::NodeText(objectNode, source) == "this")};
        fact.enclosingClassName = EnclosingClass(fact.bytes.startByte, source, fact.insideMixin, index);
        facts.memberExpressions.push_back(fact);
    }
}

/** @brief Collects all lambda expression closure boundaries from the syntax index. */
static void CollectLambdaEnclosures(const NodeIndex& index, std::string_view source, MemberAccessFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode node : index.Nodes(gs.symLambdaExpression))
    {
        facts.lambdaEnclosures.push_back(
            {BytesOf(node), RangeOf(node), EnclosingClassOfLambda(ts_node_start_byte(node), source, index), 0, 0});
    }
}

/** @brief Collects identifier nodes and implicit member access candidates from the syntax index. */
static void CollectIdentifiers(const NodeIndex& index, std::string_view source, MemberAccessFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode thisNode : index.Nodes(gs.symThisExpression))
    {
        facts.identifiers.push_back(MakeIdentifierFact(thisNode, "this", index, source));
    }

    const TSSymbol kIdSymbols[] = {gs.symIdentifier, gs.symScopedIdentifier};
    for (const TSSymbol sym : kIdSymbols)
    {
        for (TSNode node : index.Nodes(sym))
        {
            if (ShouldSkipIdentifierNode(node, parser::GetParent(node)))
            {
                continue;
            }

            const std::string_view idText = ExtractCleanIdentifier(node, source);
            const bool isThisOrSuper = (idText == "this" || idText == "super");
            if (!isThisOrSuper && (idText.empty() || parser::keywords::IsKeyword(idText) || idText == "value"))
            {
                continue;
            }

            const MemberAccessIdentifierFact fact = MakeIdentifierFact(node, idText, index, source);
            facts.identifiers.push_back(fact);

            if (!fact.enclosingClassName.empty() && !fact.isThisOrSuper)
            {
                facts.implicitAccesses.push_back(
                    {fact.bytes, fact.range, fact.name, fact.enclosingClassName, fact.insideMixin});
            }
        }
    }
}

MemberAccessFacts BuildMemberAccessFacts(const NodeIndex& index, std::string_view source)
{
    MemberAccessFacts facts;
    CollectMemberExpressions(index, source, facts);
    CollectLambdaEnclosures(index, source, facts);
    CollectIdentifiers(index, source, facts);

    SortBySpan(facts.memberExpressions);
    SortBySpan(facts.identifiers);
    SortBySpan(facts.implicitAccesses);
    SortBySpan(facts.lambdaEnclosures);

    for (auto& lambda : facts.lambdaEnclosures)
    {
        lambda.identifierBegin = static_cast<uint32_t>(facts.enclosedIdentifiers.size());
        for (const auto& idFact : facts.identifiers)
        {
            if (idFact.bytes.startByte >= lambda.bytes.startByte && idFact.bytes.endByte <= lambda.bytes.endByte)
            {
                facts.enclosedIdentifiers.push_back(idFact.bytes);
            }
        }
        lambda.identifierCount = static_cast<uint32_t>(facts.enclosedIdentifiers.size() - lambda.identifierBegin);
    }

    return facts;
}

} // namespace angel_lsp::analysis
