#include "analysis/index/InitializerListFacts.h"

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Converts a syntax node's points to a diagnostic source range.
 * @param[in] node Syntax node.
 * @return Line and column range of the node, or zero range when null.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
        return {};
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/**
 * @brief Converts a syntax node to its byte span.
 * @param[in] node Syntax node.
 * @return Start and end byte range of the node, or zero range when null.
 */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

/**
 * @brief Tests whether an outer byte span strictly contains an inner byte span.
 * @param[in] outer Enclosing byte range.
 * @param[in] inner Enclosed candidate byte range.
 * @return True when inner is entirely inside outer and strictly smaller.
 */
static constexpr bool StrictlyContains(IndexedByteRange outer, IndexedByteRange inner) noexcept
{
    return outer.startByte <= inner.startByte && inner.endByte <= outer.endByte &&
           (outer.startByte < inner.startByte || inner.endByte < outer.endByte);
}

/**
 * @brief Reads text of child by primary field, or fallback field when null.
 * @param[in] node Parent syntax node.
 * @param[in] primary Primary field identifier.
 * @param[in] fallback Fallback field identifier.
 * @param[in] source Document text buffer.
 * @return Text view of the selected child node.
 */
static std::string_view FieldTextOr(TSNode node, TSFieldId primary, TSFieldId fallback,
                                    std::string_view source) noexcept
{
    TSNode child = parser::GetChildByField(node, primary);
    if (ts_node_is_null(child) && fallback != 0)
        child = parser::GetChildByField(node, fallback);
    return ts_node_is_null(child) ? std::string_view{} : parser::NodeText(child, source);
}

/**
 * @brief Inspects an index expression to determine if its index is a string literal.
 * @param[in] left Index expression syntax node.
 * @return True when the index child is a string literal or concatenated string.
 */
static bool HasStringIndex(TSNode left)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode idx = parser::GetChildByField(left, gs.fieldIndex);
    if (ts_node_is_null(idx))
    {
        uint32_t count = 0;
        parser::ForEachChildNode(left,
                                 [&](TSNode c)
                                 {
                                     if (ts_node_is_named(c) && ++count == 2)
                                         idx = c;
                                 });
    }
    return !ts_node_is_null(idx) &&
           (ts_node_symbol(idx) == gs.symStringLiteral || ts_node_symbol(idx) == gs.symConcatenatedString);
}

/**
 * @brief Classifies an element expression syntax node into an InitializerListElementKind.
 * @param[in] node Element syntax node.
 * @param[in] value Pre-parsed value expression fact of the element.
 * @return Syntactic element kind.
 */
static InitializerListElementKind ClassifyElementKind(TSNode node, const ValueExpressionFact& value) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symInitializerList)
        return InitializerListElementKind::NestedList;
    if (sym == gs.symNumberLiteral || sym == gs.symStringLiteral || sym == gs.symBooleanLiteral ||
        sym == gs.symNullLiteral || sym == gs.symConcatenatedString || value.kind == ValueExpressionKind::Number ||
        value.kind == ValueExpressionKind::String || value.kind == ValueExpressionKind::Boolean ||
        value.kind == ValueExpressionKind::Null)
        return InitializerListElementKind::Literal;
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier || value.kind == ValueExpressionKind::Identifier)
        return InitializerListElementKind::Identifier;
    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression || value.kind == ValueExpressionKind::Call)
        return InitializerListElementKind::Call;
    return InitializerListElementKind::Other;
}

/**
 * @brief Extracts an element expression into an InitializerListElementFact.
 * @param[in] child Element expression syntax node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @return Element fact with spans, value expression fact, and operand fact.
 */
static InitializerListElementFact ReadElementFact(TSNode child, const NodeIndex& index, std::string_view source)
{
    const ValueExpressionFact val = ReadValueExpression(child, &index, source);
    return InitializerListElementFact{.bytes = BytesOf(child),
                                      .range = RangeOf(child),
                                      .text = parser::NodeText(child, source),
                                      .kind = ClassifyElementKind(child, val),
                                      .value = val,
                                      .operand = BuildBinaryOperandFact(child, index, source)};
}

/**
 * @brief Fills call argument owner context for an initializer list argument.
 * @param[in] listNode Initializer list syntax node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @param[in,out] fact Initializer list owner fact sink.
 */
static void ReadCallArgumentOwner(TSNode listNode, const NodeIndex& index, std::string_view source,
                                  InitializerListOwnerFact& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode argList = parser::GetParent(listNode);
    if (ts_node_is_null(argList))
        return;
    const TSNode call = parser::GetParent(argList);
    fact.kind = InitializerListOwnerKind::CallArgument;
    fact.ownerBytes = BytesOf(call);
    fact.ownerRange = RangeOf(call);
    if (!ts_node_is_null(call) && ts_node_symbol(call) == gs.symConstructCallExpression)
    {
        const TSNode callee = SyntaxOperands(call, &index).Callee();
        if (!ts_node_is_null(callee))
            fact.targetType = parser::NodeText(callee, source);
    }
    uint32_t argIdx = 0;
    parser::ForEachChildNode(argList,
                             [&](TSNode c)
                             {
                                 if (ts_node_is_named(c) && ts_node_symbol(c) != gs.symComment)
                                 {
                                     if (c.id == listNode.id)
                                         fact.argumentIndex = argIdx;
                                     ++argIdx;
                                 }
                             });
}

/**
 * @brief Fills assignment owner context for an initializer list right-hand side.
 * @param[in] listNode Initializer list syntax node.
 * @param[in] parent Assignment expression syntax node.
 * @param[in,out] fact Initializer list owner fact sink.
 */
static void ReadAssignmentOwner(TSNode listNode, TSNode parent, InitializerListOwnerFact& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode op = parser::GetChildByField(parent, gs.fieldOperator);
    const TSNode right = parser::GetChildByField(parent, gs.fieldRight);
    if (ts_node_is_null(op) || ts_node_symbol(op) != gs.tokAssign || right.id != listNode.id)
        return;
    fact.kind = InitializerListOwnerKind::Assignment;
    const TSNode target = parser::GetChildByField(parent, gs.fieldLeft);
    if (ts_node_is_null(target))
        return;
    fact.targetBytes = BytesOf(target);
    if (ts_node_symbol(target) == gs.symIndexExpression)
    {
        fact.isSubscriptAssignment = true;
        fact.hasStringIndex = HasStringIndex(target);
        TSNode obj = parser::GetChildByField(target, gs.fieldObject);
        if (ts_node_is_null(obj))
            parser::ForEachChildNode(target,
                                     [&](TSNode c)
                                     {
                                         if (ts_node_is_null(obj) && ts_node_is_named(c))
                                             obj = c;
                                     });
        if (!ts_node_is_null(obj))
            fact.subscriptObjectBytes = BytesOf(obj);
    }
}

/**
 * @brief Resolves the enclosing declaration, statement, or call context of an initializer list.
 * @param[in] listNode Initializer list syntax node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text.
 * @return Owning context fact.
 */
static InitializerListOwnerFact ReadOwnerFact(TSNode listNode, const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    InitializerListOwnerFact fact;
    const TSNode parent = parser::GetParent(listNode);
    if (ts_node_is_null(parent))
        return fact;
    fact.ownerBytes = BytesOf(parent);
    fact.ownerRange = RangeOf(parent);
    const TSSymbol sym = ts_node_symbol(parent);
    if (sym == gs.symInitializerList)
        fact.kind = InitializerListOwnerKind::ParentList;
    else if (sym == gs.symTypedInitializerList)
    {
        fact.kind = InitializerListOwnerKind::TypedList;
        fact.targetType = FieldTextOr(parent, gs.fieldType, 0, source);
    }
    else if (sym == gs.symVariableDeclarator)
    {
        const TSNode decl = parser::GetParent(parent);
        fact.kind = InitializerListOwnerKind::VariableDeclaration;
        fact.ownerBytes = BytesOf(decl);
        fact.ownerRange = RangeOf(decl);
        fact.targetType = FieldTextOr(decl, gs.fieldVarType, gs.fieldType, source);
    }
    else if (sym == gs.symAssignmentExpression)
        ReadAssignmentOwner(listNode, parent, fact);
    else if (sym == gs.symReturnStatement)
    {
        fact.kind = InitializerListOwnerKind::ReturnStatement;
        const TSNode func = index.EnclosingDeclaration(parent);
        if (!ts_node_is_null(func) && ts_node_symbol(func) == gs.symFuncDeclaration)
            fact.targetType = FieldTextOr(func, gs.fieldReturnType, gs.fieldType, source);
    }
    else if (sym == gs.symArgumentList)
        ReadCallArgumentOwner(listNode, index, source, fact);
    return fact;
}

InitializerListFacts BuildInitializerListFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    InitializerListFacts facts;
    for (const TSNode listNode : index.Nodes(gs.symInitializerList))
    {
        InitializerListFact listFact{.bytes = BytesOf(listNode),
                                     .range = RangeOf(listNode),
                                     .elementBegin = static_cast<uint32_t>(facts.elements.size()),
                                     .owner = ReadOwnerFact(listNode, index, source)};
        uint32_t seps = 0;
        uint32_t written = 0;
        parser::ForEachChildNode(listNode,
                                 [&](TSNode child)
                                 {
                                     const TSSymbol sym = ts_node_symbol(child);
                                     if (sym == gs.tokComma)
                                         ++seps;
                                     else if (ts_node_is_named(child) && sym != gs.symComment)
                                     {
                                         ++written;
                                         facts.elements.push_back(ReadElementFact(child, index, source));
                                     }
                                 });
        listFact.elementCount = static_cast<uint32_t>(facts.elements.size()) - listFact.elementBegin;
        listFact.valueCount = (seps > 0) ? (seps + 1) : (written > 0 ? 1 : 0);
        facts.lists.push_back(listFact);
    }

    SortBySpan(facts.lists);

    std::vector<uint32_t> stack;
    for (uint32_t i = 0; i < facts.lists.size(); ++i)
    {
        while (!stack.empty() && !StrictlyContains(facts.lists[stack.back()].bytes, facts.lists[i].bytes))
            stack.pop_back();
        if (!stack.empty())
        {
            facts.lists[i].parentListIndex = stack.back();
            if (facts.lists[i].owner.kind == InitializerListOwnerKind::None)
                facts.lists[i].owner.kind = InitializerListOwnerKind::ParentList;
        }
        stack.push_back(i);
    }

    for (auto& elem : facts.elements)
    {
        if (elem.kind == InitializerListElementKind::NestedList)
        {
            if (const auto* nested = facts.Find(elem.bytes))
                elem.nestedListIndex = static_cast<uint32_t>(nested - facts.lists.data());
        }
    }
    return facts;
}

} // namespace angel_lsp::analysis
