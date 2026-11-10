#include "analysis/index/CallSiteFacts.h"

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ReferenceReceiverIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <cctype>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

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
 * @brief Trims ASCII whitespace from both ends of a view.
 * @param[in] text View to trim.
 * @return Sub-view of text without leading or trailing whitespace.
 */
static std::string_view Trim(std::string_view text) noexcept
{
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

static void ReadEnclosingContext(uint32_t startByte, const NodeIndex& index, std::string_view source,
                                 CallSiteFact& fact) noexcept
{
    const auto* classSpan = index.FindInnermostClassSpan(startByte);
    if (classSpan && classSpan->nameStartByte < classSpan->nameEndByte && classSpan->nameEndByte <= source.size())
    {
        fact.insideClass = true;
        fact.insideMixin = classSpan->isMixin;
        fact.enclosingClassName =
            source.substr(classSpan->nameStartByte, classSpan->nameEndByte - classSpan->nameStartByte);
    }
    fact.isInsideLambda = index.IsInsideLambdaClosure(startByte);
    const TSNode callable = index.FindEnclosingCallable(startByte);
    const auto& gs = parser::GetGrammarSymbols();
    if (!ts_node_is_null(callable) && ts_node_symbol(callable) == gs.symFuncDeclaration)
    {
        const SyntaxOperands fnSyntax(callable, &index);
        const TSNode nameNode = fnSyntax.Field(gs.fieldName);
        if (!ts_node_is_null(nameNode))
        {
            fact.enclosingFunctionName = parser::NodeText(nameNode, source);
        }
        const TSNode attr = parser::FindChildWithSymbol(callable, gs.symFuncAttributes);
        fact.isEnclosingMethodConst =
            !ts_node_is_null(parser::FindChildWithSymbol(callable, gs.tokConst)) ||
            (!ts_node_is_null(attr) && !ts_node_is_null(parser::FindChildWithSymbol(attr, gs.tokConst)));
    }
}

static void ReadCallee(TSNode node, TSNode calleeNode, const NodeIndex& index, std::string_view source,
                       CallSiteFact& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(calleeNode))
    {
        return;
    }
    fact.calleeBytes = BytesOf(calleeNode);
    fact.calleeRange = RangeOf(calleeNode);
    fact.calleeText = parser::NodeText(calleeNode, source);
    const TSSymbol calleeSym = ts_node_symbol(calleeNode);
    if (calleeSym == gs.symMemberExpression)
    {
        fact.calleeKind = CalleeKind::Member;
        fact.isMemberCall = true;
        const TSNode obj = parser::GetChildByField(calleeNode, gs.fieldObject);
        const TSNode mem = parser::GetChildByField(calleeNode, gs.fieldMember);
        if (!ts_node_is_null(obj))
        {
            fact.objectOperand = BuildBinaryOperandFact(obj, index, source);
            fact.objectValue = ReadValueExpression(obj, &index, source);
            fact.isThisCall = (fact.objectOperand.text == "this");
        }
        if (!ts_node_is_null(mem))
        {
            fact.calleeNameBytes = BytesOf(mem);
            fact.calleeNameRange = RangeOf(mem);
            fact.calleeName = parser::NodeText(mem, source);
        }
    }
    else if (calleeSym == gs.symScopedIdentifier)
    {
        const TSNode tail = ScopedIdentifierTail(calleeNode);
        const TSNode target = !ts_node_is_null(tail) ? tail : calleeNode;
        fact.calleeNameBytes = BytesOf(target);
        fact.calleeNameRange = RangeOf(target);
        fact.calleeName = parser::NodeText(target, source);
        if (utils::ContainsScopeSeparator(fact.calleeText))
        {
            fact.calleeKind = CalleeKind::Scoped;
            fact.scopeQualifier = utils::ParentScopeView(fact.calleeText);
        }
        else
        {
            fact.calleeKind = CalleeKind::Identifier;
        }
    }
    else
    {
        fact.calleeKind = CalleeKind::Identifier;
        fact.calleeNameBytes = fact.calleeBytes;
        fact.calleeNameRange = fact.calleeRange;
        fact.calleeName = fact.calleeText;
    }

    const SyntaxOperands callSyntax(node, &index);
    TSNode tmplNode = callSyntax.FirstNamed(gs.symTemplateTypeList);
    if (ts_node_is_null(tmplNode))
        tmplNode = SyntaxOperands(calleeNode, &index).FirstNamed(gs.symTemplateTypeList);
    if (!ts_node_is_null(tmplNode))
    {
        fact.hasTemplateArguments = true;
        fact.templateArgsBytes = BytesOf(tmplNode);
        fact.templateArgsText = parser::NodeText(tmplNode, source);
    }
}

static CallSiteFact ReadCallSite(TSNode node, const NodeIndex& index, std::string_view source, CallSiteFacts& facts)
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
    ReadCallee(node, calleeNode, index, source, fact);
    ReadEnclosingContext(fact.bytes.startByte, index, source, fact);

    TSNode argsNode = syntax.Field(gs.fieldArguments);
    if (ts_node_is_null(argsNode))
        argsNode = syntax.FirstNamed(gs.symArgumentList);
    if (!ts_node_is_null(argsNode))
    {
        fact.hasArgumentList = true;
        fact.argumentListBytes = BytesOf(argsNode);
        fact.argumentListRange = RangeOf(argsNode);
        fact.argumentBegin = static_cast<uint32_t>(facts.arguments.size());
        ExtractCallArguments(argsNode, index, source, facts);
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
            fact.isDelegateConstruction = (soleArg.isThisMemberAccess && soleArg.thisMemberName == fact.calleeName);
        }
    }
    return fact;
}

static void CollectCallSites(const NodeIndex& index, std::string_view source, CallSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symCallExpression))
    {
        facts.callSites.push_back(ReadCallSite(node, index, source, facts));
    }
    for (const TSNode node : index.Nodes(gs.symConstructCallExpression))
    {
        facts.callSites.push_back(ReadCallSite(node, index, source, facts));
    }
}

static void CollectDirectInits(const NodeIndex& index, std::string_view source, CallSiteFacts& facts)
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
        const std::string_view rawType = Trim(parser::NodeText(varTypeNode, source));
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
                initFact.declaredType = rawType;
                const size_t openBracket = rawType.find('<');
                if (openBracket != std::string_view::npos && rawType.ends_with('>'))
                {
                    initFact.baseTypeName = Trim(rawType.substr(0, openBracket));
                    initFact.templateArgsText = rawType.substr(openBracket);
                }
                else
                {
                    initFact.baseTypeName = rawType;
                }
                const TSNode nameNode = declSyntax.Field(gs.fieldName);
                if (!ts_node_is_null(nameNode))
                {
                    initFact.declaratorNameBytes = BytesOf(nameNode);
                    initFact.declaratorNameRange = RangeOf(nameNode);
                    initFact.declaratorName = parser::NodeText(nameNode, source);
                }
                initFact.argumentListBytes = BytesOf(argsNode);
                initFact.argumentListRange = RangeOf(argsNode);
                initFact.argumentBegin = static_cast<uint32_t>(facts.arguments.size());
                ExtractCallArguments(argsNode, index, source, facts);
                initFact.argumentCount = static_cast<uint32_t>(facts.arguments.size()) - initFact.argumentBegin;
                facts.directInits.push_back(initFact);
                return true;
            });
    }
}

CallSiteFacts BuildCallSiteFacts(const NodeIndex& index, std::string_view source)
{
    CallSiteFacts facts;
    CollectCallSites(index, source, facts);
    CollectDirectInits(index, source, facts);
    SortBySpan(facts.callSites);
    SortBySpan(facts.directInits);
    return facts;
}

} // namespace angel_lsp::analysis
