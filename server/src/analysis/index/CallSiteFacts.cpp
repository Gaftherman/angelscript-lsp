#include "analysis/index/CallSiteFacts.h"

#include "analysis/index/ReferenceReceiverIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

const CallSiteFact* CallSiteFacts::FindCallSite(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(callSites, bytes);
}

const DirectInitSiteFact* CallSiteFacts::FindDirectInit(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(directInits, bytes);
}

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Syntax node.
 * @return Line and column span of the node, or a zero range if null.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
        return {};
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

/**
 * @brief Computes a node's byte span with leading and trailing ASCII whitespace trimmed.
 * @param[in] node Syntax node.
 * @param[in] source Document text backing node bytes.
 * @return Trimmed byte span, or empty when node is null or whitespace-only.
 */
static IndexedByteRange TrimmedBytesOf(TSNode node, std::string_view source) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    while (start < end && start < source.size() && std::isspace(static_cast<unsigned char>(source[start])))
    {
        ++start;
    }
    while (end > start && end <= source.size() && std::isspace(static_cast<unsigned char>(source[end - 1])))
    {
        --end;
    }
    return {start, end};
}

static void ReadEnclosingContext(uint32_t startByte, const NodeIndex& index, std::string_view source,
                                 CallSiteFact& fact) noexcept
{
    const auto* classSpan = index.FindInnermostClassSpan(startByte);
    if (classSpan && classSpan->nameStartByte < classSpan->nameEndByte && classSpan->nameEndByte <= source.size())
    {
        fact.insideClass = true;
        fact.insideMixin = classSpan->isMixin;
        fact.enclosingClassNameBytes = {classSpan->nameStartByte, classSpan->nameEndByte};
    }
    const TSNode callable = index.FindEnclosingCallable(startByte);
    const auto& gs = parser::GetGrammarSymbols();
    if (!ts_node_is_null(callable) && ts_node_symbol(callable) == gs.symFuncDeclaration)
    {
        const SyntaxOperands fnSyntax(callable, &index);
        const TSNode nameNode = fnSyntax.Field(gs.fieldName);
        if (!ts_node_is_null(nameNode))
        {
            fact.enclosingFunctionNameBytes = BytesOf(nameNode);
        }
        const TSNode attr = parser::FindChildWithSymbol(callable, gs.symFuncAttributes);
        fact.isEnclosingMethodConst =
            !ts_node_is_null(parser::FindChildWithSymbol(callable, gs.tokConst)) ||
            (!ts_node_is_null(attr) && !ts_node_is_null(parser::FindChildWithSymbol(attr, gs.tokConst)));
    }
}

static void ReadCallee(TSNode calleeNode, std::string_view source, CallSiteFact& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(calleeNode))
    {
        return;
    }
    fact.calleeBytes = BytesOf(calleeNode);
    fact.calleeRange = RangeOf(calleeNode);
    const std::string_view calleeText = parser::NodeText(calleeNode, source);
    const TSSymbol calleeSym = ts_node_symbol(calleeNode);
    if (calleeSym == gs.symMemberExpression)
    {
        fact.calleeKind = CalleeKind::Member;
        fact.isMemberCall = true;
        const TSNode obj = parser::GetChildByField(calleeNode, gs.fieldObject);
        const TSNode mem = parser::GetChildByField(calleeNode, gs.fieldMember);
        if (!ts_node_is_null(obj))
        {
            fact.objectBytes = BytesOf(obj);
            fact.isThisCall = (parser::NodeText(obj, source) == parser::keywords::This);
        }
        if (!ts_node_is_null(mem))
        {
            fact.calleeNameBytes = BytesOf(mem);
            fact.calleeNameRange = RangeOf(mem);
        }
    }
    else if (calleeSym == gs.symScopedIdentifier)
    {
        const TSNode tail = ScopedIdentifierTail(calleeNode);
        const TSNode target = !ts_node_is_null(tail) ? tail : calleeNode;
        fact.calleeNameBytes = BytesOf(target);
        fact.calleeNameRange = RangeOf(target);
        fact.calleeKind = utils::ContainsScopeSeparator(calleeText) ? CalleeKind::Scoped : CalleeKind::Identifier;
    }
    else
    {
        fact.calleeKind = CalleeKind::Identifier;
        fact.calleeNameBytes = fact.calleeBytes;
        fact.calleeNameRange = fact.calleeRange;
    }
}

static CallSiteFact ReadCallSite(TSNode node, const NodeIndex& index, CallSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    CallSiteFact fact;
    fact.bytes = BytesOf(node);
    fact.range = RangeOf(node);
    fact.isConstructCall = (ts_node_symbol(node) == gs.symConstructCallExpression);

    TSNode calleeNode = fact.isConstructCall ? syntax.Field(gs.fieldType) : syntax.Callee();
    if (ts_node_is_null(calleeNode) && ts_node_named_child_count(node) > 0)
        calleeNode = ts_node_named_child(node, 0);
    ReadCallee(calleeNode, facts.source, fact);
    ReadEnclosingContext(fact.bytes.startByte, index, facts.source, fact);

    TSNode argsNode = syntax.Field(gs.fieldArguments);
    if (ts_node_is_null(argsNode))
        argsNode = syntax.FirstNamed(gs.symArgumentList);
    if (!ts_node_is_null(argsNode))
    {
        fact.hasArgumentList = true;
        fact.argumentBegin = static_cast<uint32_t>(facts.arguments.size());
        ExtractCallArguments(argsNode, index, facts);
        fact.argumentCount = static_cast<uint32_t>(facts.arguments.size()) - fact.argumentBegin;

        bool sawNamed = false;
        for (uint32_t i = 0; i < fact.argumentCount; ++i)
        {
            auto& arg = facts.arguments[fact.argumentBegin + i];
            if (arg.isNamed)
            {
                sawNamed = true;
                fact.hasNamedArguments = true;
            }
            else if (sawNamed)
            {
                arg.isPositionalAfterNamed = true;
                fact.hasPositionalAfterNamed = true;
            }
        }
        if (fact.argumentCount == 1)
        {
            const auto& soleArg = facts.arguments[fact.argumentBegin];
            fact.isDelegateConstruction =
                (soleArg.isThisMemberAccess && facts.ThisMemberName(soleArg) == facts.TextOf(fact.calleeNameBytes));
        }
    }
    return fact;
}

static void CollectCallSites(const NodeIndex& index, CallSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symCallExpression))
    {
        facts.callSites.push_back(ReadCallSite(node, index, facts));
    }
    for (const TSNode node : index.Nodes(gs.symConstructCallExpression))
    {
        facts.callSites.push_back(ReadCallSite(node, index, facts));
    }
}

static void CollectDirectInits(const NodeIndex& index, CallSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode varDecl : index.Nodes(gs.symVariableDeclaration))
    {
        const SyntaxOperands varSyntax(varDecl, &index);
        TSNode varTypeNode = varSyntax.Field(gs.fieldVarType);
        if (ts_node_is_null(varTypeNode))
        {
            varTypeNode = varSyntax.Field(gs.fieldType);
        }
        if (ts_node_is_null(varTypeNode))
        {
            continue;
        }
        const IndexedByteRange rawTypeBytes = TrimmedBytesOf(varTypeNode, facts.source);
        varSyntax.ForEachNamed(
            [&](TSNode declarator)
            {
                if (ts_node_symbol(declarator) != gs.symVariableDeclarator)
                {
                    return true;
                }
                const SyntaxOperands declSyntax(declarator, &index);
                const TSNode argsNode = declSyntax.Field(gs.fieldArguments);
                if (ts_node_is_null(argsNode))
                {
                    return true;
                }
                DirectInitSiteFact initFact;
                initFact.bytes = BytesOf(declarator);
                initFact.range = RangeOf(declarator);
                initFact.declaredTypeBytes = rawTypeBytes;
                initFact.argumentListRange = RangeOf(argsNode);
                initFact.argumentBegin = static_cast<uint32_t>(facts.arguments.size());
                ExtractCallArguments(argsNode, index, facts);
                initFact.argumentCount = static_cast<uint32_t>(facts.arguments.size()) - initFact.argumentBegin;
                facts.directInits.push_back(initFact);
                return true;
            });
    }
}

CallSiteFacts BuildCallSiteFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    CallSiteFacts facts;
    facts.source = source;
    const auto callNodes = index.Nodes(gs.symCallExpression);
    const auto constructNodes = index.Nodes(gs.symConstructCallExpression);
    const auto varDeclNodes = index.Nodes(gs.symVariableDeclaration);
    const auto argListNodes = index.Nodes(gs.symArgumentList);
    facts.callSites.reserve(callNodes.size() + constructNodes.size());
    facts.directInits.reserve(std::min(varDeclNodes.size(), argListNodes.size()));
    facts.arguments.reserve(argListNodes.size());
    CollectCallSites(index, facts);
    CollectDirectInits(index, facts);
    SortBySpan(facts.callSites);
    SortBySpan(facts.directInits);
    return facts;
}

} // namespace angel_lsp::analysis
