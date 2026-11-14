#include "analysis/index/ControlFlowFacts.h"

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ReturnSiteFacts.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Converts a node's start and end points into a line/column source range.
 * @param[in] node Node to measure; a null node yields an empty range.
 * @return Zero-based range covering the node's text.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint s = ts_node_start_point(node), e = ts_node_end_point(node);
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

static bool IsLoopKind(ControlFlowRecordKind kind) noexcept
{
    return kind == ControlFlowRecordKind::While || kind == ControlFlowRecordKind::DoWhile ||
           kind == ControlFlowRecordKind::For || kind == ControlFlowRecordKind::Foreach;
}

static ControlFlowConditionFact ReadCondition(TSNode condNode, const NodeIndex& index, std::string_view source)
{
    ControlFlowConditionFact cond;
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(condNode) || ts_node_symbol(condNode) == gs.tokSemicolon)
    {
        return cond;
    }
    if (ts_node_symbol(condNode) == gs.symExpressionStatement)
    {
        condNode = SyntaxOperands(condNode, &index).FirstNamed();
        if (ts_node_is_null(condNode))
        {
            return cond;
        }
    }

    cond.present = true;
    cond.bytes = BytesOf(condNode);
    cond.range = RangeOf(condNode);
    cond.text = utils::TrimView(parser::NodeText(condNode, source));

    TSNode inner = condNode;
    while (!ts_node_is_null(inner) && ts_node_symbol(inner) == gs.symParenthesizedExpression)
    {
        inner = SyntaxOperands(inner, &index).FirstNamed();
    }
    if (ts_node_is_null(inner))
    {
        return cond;
    }

    const TSSymbol sym = ts_node_symbol(inner);
    const TSNode child = ts_node_child_count(inner) > 0 ? ts_node_child(inner, 0) : TSNode{};
    const TSSymbol childSym = !ts_node_is_null(child) ? ts_node_symbol(child) : 0;
    const std::string_view innerText = utils::TrimView(parser::NodeText(inner, source));
    const bool isTrue =
        sym == gs.tokTrue || childSym == gs.tokTrue || (sym == gs.symBooleanLiteral && innerText == "true");
    const bool isFalse =
        sym == gs.tokFalse || childSym == gs.tokFalse || (sym == gs.symBooleanLiteral && innerText == "false");
    const bool isNull = sym == gs.tokNull || sym == gs.symNullLiteral || childSym == gs.tokNull;
    const bool isNumber = sym == gs.symNumberLiteral;

    cond.isConstant = isTrue || isFalse || isNull || isNumber;
    cond.constantValue =
        isTrue || (isNumber && innerText != "0" && innerText != "0.0" && innerText != "0.0f" && innerText != "0x0");
    return cond;
}

static std::pair<uint32_t, uint32_t> CollectSwitchLabels(TSNode node, const NodeIndex& index, std::string_view source,
                                                         ControlFlowFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto labels = index.SwitchLabels(ts_node_start_byte(node));
    const uint32_t begin = static_cast<uint32_t>(facts.labels.size());
    for (const TSNode labelNode : labels)
    {
        const TSSymbol sym = ts_node_symbol(labelNode);
        const std::string_view text = utils::TrimView(parser::NodeText(labelNode, source));
        facts.labels.push_back(ControlFlowCaseLabelFact{
            .bytes = BytesOf(labelNode),
            .range = RangeOf(labelNode),
            .text = text,
            .isUnusable = (sym == gs.symStringLiteral ||
                           (sym == gs.symNumberLiteral && text.find('.') != std::string_view::npos)),
            .isNumberLiteral = (sym == gs.symNumberLiteral),
            .isStringLiteral = (sym == gs.symStringLiteral),
            .isIdentifier = (sym == gs.symIdentifier || sym == gs.symScopedIdentifier),
            .isCall = (sym == gs.symCallExpression)});
    }
    return {begin, static_cast<uint32_t>(facts.labels.size() - begin)};
}

static void AddCallable(TSNode node, const NodeIndex& index, std::string_view source, ControlFlowFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    const bool isLambda = (ts_node_symbol(node) == gs.symLambdaExpression);
    const TSNode body = syntax.Field(gs.fieldBody);
    const TSNode retType = isLambda ? TSNode{} : syntax.Field(gs.fieldReturnType);
    const TSNode name = isLambda ? TSNode{} : syntax.Field(gs.fieldName);
    const std::string_view fnName = !ts_node_is_null(name) ? parser::NodeText(name, source) : "";
    const std::string_view retStr = !ts_node_is_null(retType) ? utils::TrimView(parser::NodeText(retType, source)) : "";
    const bool implicitVoid = !isLambda && ts_node_is_null(retType);

    ControlFlowRecord rec{.kind = isLambda ? ControlFlowRecordKind::Lambda : ControlFlowRecordKind::Function,
                          .bytes = BytesOf(node),
                          .range = RangeOf(node),
                          .enclosingReturnType = retStr,
                          .enclosingFunctionName = fnName,
                          .enclosingImplicitVoid = implicitVoid};
    ControlFlowFunctionFact fnFact{.bytes = rec.bytes,
                                   .range = rec.range,
                                   .name = fnName,
                                   .nameRange = RangeOf(name),
                                   .returnType = retStr,
                                   .returnTypeRange = RangeOf(retType),
                                   .isLambda = isLambda,
                                   .implicitVoid = implicitVoid};
    if (!ts_node_is_null(body))
    {
        fnFact.bodyBytes = BytesOf(body);
        fnFact.bodyRange = RangeOf(body);
        fnFact.hasSyntaxError = ts_node_has_error(body) || index.NodeHasSyntaxError(body);
        fnFact.definitelyReturns = index.CallableBodyDefinitelyReturns(ts_node_start_byte(body));
    }
    facts.functions.push_back(fnFact);
    facts.records.push_back(rec);
}

static void CollectStatements(const NodeIndex& index, std::string_view source, ControlFlowFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode ifNode : index.Nodes(gs.symIfStatement))
    {
        const SyntaxOperands syntax(ifNode, &index);
        ControlFlowRecord rec{.kind = ControlFlowRecordKind::If, .bytes = BytesOf(ifNode), .range = RangeOf(ifNode)};
        rec.condition = ReadCondition(syntax.FieldOrNamed(gs.fieldCondition, 0), index, source);
        const TSNode c = syntax.Field(gs.fieldConsequence);
        const TSNode a = syntax.Field(gs.fieldAlternative);
        if (!ts_node_is_null(c) && ts_node_symbol(c) == gs.tokSemicolon)
        {
            rec.branch.hasEmptyConsequence = true;
            rec.branch.consequenceRange = RangeOf(c);
        }
        if (!ts_node_is_null(a) && ts_node_symbol(a) == gs.tokSemicolon)
        {
            rec.branch.hasEmptyAlternative = true;
            rec.branch.alternativeRange = RangeOf(a);
        }
        facts.records.push_back(rec);
    }
    for (const TSNode sw : index.Nodes(gs.symSwitchStatement))
    {
        ControlFlowRecord rec{.kind = ControlFlowRecordKind::Switch, .bytes = BytesOf(sw), .range = RangeOf(sw)};
        rec.condition = ReadCondition(SyntaxOperands(sw, &index).FieldOrNamed(gs.fieldCondition, 0), index, source);
        const auto [begin, count] = CollectSwitchLabels(sw, index, source, facts);
        rec.labelBegin = begin;
        rec.labelCount = count;
        facts.records.push_back(rec);
    }
    const std::pair<TSSymbol, ControlFlowRecordKind> loops[] = {
        {gs.symWhileStatement, ControlFlowRecordKind::While},
        {gs.symDoWhileStatement, ControlFlowRecordKind::DoWhile},
        {gs.symForStatement, ControlFlowRecordKind::For},
        {gs.symForeachStatement, ControlFlowRecordKind::Foreach}};
    for (const auto& [sym, kind] : loops)
    {
        for (const TSNode lp : index.Nodes(sym))
        {
            ControlFlowRecord rec{.kind = kind, .bytes = BytesOf(lp), .range = RangeOf(lp)};
            if (kind != ControlFlowRecordKind::Foreach)
            {
                rec.condition =
                    ReadCondition(SyntaxOperands(lp, &index).FieldOrNamed(gs.fieldCondition, 0), index, source);
            }
            facts.records.push_back(rec);
        }
    }
    const std::pair<TSSymbol, ControlFlowRecordKind> simple[] = {
        {gs.symStatementBlock, ControlFlowRecordKind::Block},
        {gs.symCaseClause, ControlFlowRecordKind::Case},
        {gs.symBreakStatement, ControlFlowRecordKind::Break},
        {gs.symContinueStatement, ControlFlowRecordKind::Continue},
        {gs.symTryStatement, ControlFlowRecordKind::Try}};
    for (const auto& [sym, kind] : simple)
    {
        for (const TSNode n : index.Nodes(sym))
        {
            facts.records.push_back(ControlFlowRecord{.kind = kind, .bytes = BytesOf(n), .range = RangeOf(n)});
        }
    }
    for (const TSNode ret : index.Nodes(gs.symReturnStatement))
    {
        ControlFlowRecord rec{.kind = ControlFlowRecordKind::Return, .bytes = BytesOf(ret), .range = RangeOf(ret)};
        const uint32_t count = ts_node_named_child_count(ret);
        rec.returnFact.hasValue = (count > 0);
        rec.returnFact.isBare = (count == 0);
        rec.returnFact.returnsNullLiteral =
            (count == 1 && ts_node_symbol(ts_node_named_child(ret, 0)) == gs.symNullLiteral);
        if (count > 0)
        {
            const TSNode val = SyntaxOperands(ret, &index).FieldOrNamed(gs.fieldValue);
            if (!ts_node_is_null(val))
            {
                rec.returnFact.valueOperand = BuildBinaryOperandFact(val, index, source);
            }
        }
        facts.records.push_back(rec);
    }
}

static void LinkHierarchyAndDepth(ControlFlowFacts& facts)
{
    std::vector<uint32_t> stack;
    for (uint32_t i = 0; i < facts.records.size(); ++i)
    {
        auto& rec = facts.records[i];
        while (!stack.empty() && facts.records[stack.back()].bytes.endByte <= rec.bytes.startByte)
        {
            stack.pop_back();
        }
        if (!stack.empty())
        {
            const uint32_t parentIdx = stack.back();
            rec.parentIndex = parentIdx;
            const auto& parentRec = facts.records[parentIdx];
            rec.loopDepth = parentRec.loopDepth + (IsLoopKind(parentRec.kind) ? 1u : 0u);
            rec.switchDepth = parentRec.switchDepth + (parentRec.kind == ControlFlowRecordKind::Switch ? 1u : 0u);
            rec.enclosingReturnType = parentRec.enclosingReturnType;
            rec.enclosingFunctionName = parentRec.enclosingFunctionName;
            rec.enclosingImplicitVoid = parentRec.enclosingImplicitVoid;
        }
        stack.push_back(i);
    }

    std::vector<std::vector<uint32_t>> children(facts.records.size());
    for (uint32_t i = 0; i < facts.records.size(); ++i)
    {
        if (facts.records[i].parentIndex != UINT32_MAX)
        {
            children[facts.records[i].parentIndex].push_back(i);
        }
    }
    for (uint32_t i = 0; i < facts.records.size(); ++i)
    {
        facts.records[i].childBegin = static_cast<uint32_t>(facts.childIndices.size());
        facts.records[i].childCount = static_cast<uint32_t>(children[i].size());
        facts.childIndices.insert(facts.childIndices.end(), children[i].begin(), children[i].end());
    }
}

ControlFlowFacts BuildControlFlowFacts(const NodeIndex& index, std::string_view source)
{
    ControlFlowFacts facts;
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode func : index.Nodes(gs.symFuncDeclaration))
    {
        AddCallable(func, index, source, facts);
    }
    for (const TSNode lambda : index.Nodes(gs.symLambdaExpression))
    {
        AddCallable(lambda, index, source, facts);
    }
    CollectStatements(index, source, facts);
    SortBySpan(facts.records);
    SortBySpan(facts.functions);
    SortBySpan(facts.labels);
    LinkHierarchyAndDepth(facts);

    for (auto& fnFact : facts.functions)
    {
        if (const auto* rec = facts.FindRecord(fnFact.bytes))
        {
            fnFact.recordIndex = static_cast<uint32_t>(rec - facts.records.data());
        }
    }

    facts.returnSites = BuildReturnSiteFacts(index, source);
    return facts;
}

} // namespace angel_lsp::analysis
