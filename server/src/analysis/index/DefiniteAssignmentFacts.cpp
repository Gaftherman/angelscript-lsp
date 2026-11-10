#include "analysis/index/DefiniteAssignmentFacts.h"

#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/** @brief Computes 0-based source range for an AST node. */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
        return {};
    const TSPoint s = ts_node_start_point(node), e = ts_node_end_point(node);
    return {s.row, s.column, e.row, e.column};
}

/** @brief Computes half-open byte range for an AST node. */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

/** @brief Maps grammar statement symbol to definite assignment statement kind. */
static DefiniteAssignmentStmtKind MapStmtKind(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    if (sym == gs.symStatementBlock)
        return DefiniteAssignmentStmtKind::Block;
    if (sym == gs.symIfStatement)
        return DefiniteAssignmentStmtKind::If;
    if (sym == gs.symWhileStatement)
        return DefiniteAssignmentStmtKind::While;
    if (sym == gs.symForStatement)
        return DefiniteAssignmentStmtKind::For;
    if (sym == gs.symDoWhileStatement)
        return DefiniteAssignmentStmtKind::DoWhile;
    if (sym == gs.symSwitchStatement)
        return DefiniteAssignmentStmtKind::Switch;
    if (sym == gs.symCaseClause)
        return DefiniteAssignmentStmtKind::Case;
    if (sym == gs.symExpressionStatement)
        return DefiniteAssignmentStmtKind::Expression;
    if (sym == gs.symVariableDeclaration)
        return DefiniteAssignmentStmtKind::VariableDeclaration;
    if (sym == gs.symReturnStatement)
        return DefiniteAssignmentStmtKind::Return;
    return sym == gs.symBreakStatement ? DefiniteAssignmentStmtKind::Break
                                       : (sym == gs.symContinueStatement ? DefiniteAssignmentStmtKind::Continue
                                                                         : DefiniteAssignmentStmtKind::Other);
}

/** @brief Resolves simple identifier text from an identifier or single-segment scoped identifier node. */
static std::string_view SimpleIdentifierName(TSNode node, std::string_view source) noexcept
{
    if (ts_node_is_null(node))
        return {};
    const auto& gs = parser::GetGrammarSymbols();
    TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symScopedIdentifier && ts_node_named_child_count(node) == 1)
    {
        node = ts_node_named_child(node, 0);
        sym = ts_node_symbol(node);
    }
    return sym == gs.symIdentifier ? parser::NodeText(node, source) : std::string_view{};
}

/** @brief Evaluates whether a loop condition is statically known to be always true. */
static bool IsLoopConditionAlwaysTrue(TSNode condNode, bool isFor, std::string_view source) noexcept
{
    if (ts_node_is_null(condNode))
        return isFor;
    const std::string_view val =
        isFor ? utils::TrimView(parser::NodeText(condNode, source)) : parser::NodeText(condNode, source);
    return val == "true" || (isFor && (val.empty() || val == ";"));
}

/** @brief Extracts reads, assignments, and call sites from an expression sub-tree. */
static void ExtractExpressions(TSNode exprNode, const NodeIndex& index, std::string_view source,
                               DefiniteAssignmentFacts& facts)
{
    if (ts_node_is_null(exprNode))
        return;
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<TSNode> stack{exprNode};
    while (!stack.empty())
    {
        const TSNode node = stack.back();
        stack.pop_back();
        if (ts_node_is_null(node))
            continue;
        const TSSymbol sym = ts_node_symbol(node);
        if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
        {
            if (const std::string_view name = SimpleIdentifierName(node, source); !name.empty())
                facts.reads.push_back({BytesOf(node), RangeOf(node), name});
        }
        else if (sym == gs.symAssignmentExpression)
        {
            const SyntaxOperands op(node, &index);
            const TSNode l = op.Field(gs.fieldLeft), r = op.Field(gs.fieldRight);
            if (const auto* s = facts.assignmentSites.Find(BytesOf(node)))
                facts.assignments.push_back(*s);
            if (parser::NodeText(op.Field(gs.fieldOperator), source) != "=" && !ts_node_is_null(l))
                stack.push_back(l);
            if (!ts_node_is_null(r))
                stack.push_back(r);
        }
        else if (sym == gs.symCallExpression)
        {
            const SyntaxOperands op(node, &index);
            const TSNode func = op.Field(gs.fieldFunction), args = op.Field(gs.fieldArguments);
            DefiniteAssignmentCallFact call{BytesOf(node), RangeOf(node), parser::NodeText(func, source)};
            if (!ts_node_is_null(func) && ts_node_symbol(func) == gs.symMemberExpression)
            {
                const SyntaxOperands m(func, &index);
                const TSNode obj = m.Field(gs.fieldObject);
                call.isMemberCall = true;
                call.memberName = parser::NodeText(m.Field(gs.fieldMember), source);
                call.objectText = parser::NodeText(obj, source);
                call.objectBytes = BytesOf(obj);
                call.objectRange = RangeOf(obj);
                if (!ts_node_is_null(obj))
                    stack.push_back(obj);
            }
            call.argumentBegin = static_cast<uint32_t>(facts.callArguments.size());
            if (!ts_node_is_null(args))
                SyntaxOperands(args, &index)
                    .ForEachNamed(
                        [&](TSNode arg)
                        {
                            if (ts_node_symbol(arg) == gs.symComment)
                                return true;
                            const std::string_view aName = SimpleIdentifierName(arg, source);
                            facts.callArguments.push_back({BytesOf(arg), RangeOf(arg), aName, !aName.empty()});
                            stack.push_back(arg);
                            return true;
                        });
            call.argumentCount = static_cast<uint32_t>(facts.callArguments.size()) - call.argumentBegin;
            facts.calls.push_back(call);
        }
        else if (sym == gs.symMemberExpression)
            stack.push_back(SyntaxOperands(node, &index).Field(gs.fieldObject));
        else if (sym != gs.symLambdaExpression)
            for (uint32_t i = ts_node_named_child_count(node); i > 0; --i)
                if (const TSNode child = ts_node_named_child(node, i - 1); ts_node_symbol(child) != gs.symComment)
                    stack.push_back(child);
    }
}

/** @brief Collects local variable declarators within a variable declaration statement. */
static void CollectDeclarators(TSNode declNode, const NodeIndex& index, std::string_view source,
                               DefiniteAssignmentFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands decl(declNode, &index);
    TSNode typeNode = decl.Field(gs.fieldVarType);
    typeNode = ts_node_is_null(typeNode) ? decl.Field(gs.fieldType) : typeNode;
    bool isPrimitive = false;
    if (!ts_node_is_null(typeNode))
    {
        const TypeExtractionResult t = ExtractTypeInfoFromAST(typeNode, source, &index);
        isPrimitive = IsPrimitiveTypeName(t.baseTypeName) && !t.isArray && !t.isHandle && t.templateName.empty();
    }
    decl.ForEachNamed(
        [&](TSNode child)
        {
            if (ts_node_symbol(child) != gs.symVariableDeclarator)
                return true;
            const SyntaxOperands d(child, &index);
            const TSNode nameNode = d.Field(gs.fieldName);
            TSNode initNode = d.Field(gs.fieldValue);
            initNode = ts_node_is_null(initNode) ? d.FirstNamed(gs.symInitializerList) : initNode;
            const bool hasInit = !ts_node_is_null(initNode);
            facts.declarators.push_back(
                {BytesOf(child), RangeOf(child), RangeOf(!ts_node_is_null(nameNode) ? nameNode : child),
                 parser::NodeText(nameNode, source), hasInit, isPrimitive, (isPrimitive && !hasInit)});
            if (hasInit)
                ExtractExpressions(initNode, index, source, facts);
            return true;
        });
}

/** @brief Constructs and populates a single statement record from an AST node. */
static DefiniteAssignmentStmtRecord BuildRecord(TSNode node, const NodeIndex& index, std::string_view source,
                                                DefiniteAssignmentFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands s(node, &index);
    DefiniteAssignmentStmtRecord rec{
        .kind = MapStmtKind(ts_node_symbol(node), gs), .bytes = BytesOf(node), .range = RangeOf(node)};
    rec.readBegin = static_cast<uint32_t>(facts.reads.size());
    rec.assignBegin = static_cast<uint32_t>(facts.assignments.size());
    rec.callBegin = static_cast<uint32_t>(facts.calls.size());
    switch (rec.kind)
    {
    case DefiniteAssignmentStmtKind::If:
    case DefiniteAssignmentStmtKind::Switch:
        ExtractExpressions(s.FieldOrNamed(gs.fieldCondition, 0), index, source, facts);
        break;
    case DefiniteAssignmentStmtKind::While:
        rec.conditionAlwaysTrue = IsLoopConditionAlwaysTrue(s.FieldOrNamed(gs.fieldCondition, 0), false, source);
        ExtractExpressions(s.FieldOrNamed(gs.fieldCondition, 0), index, source, facts);
        break;
    case DefiniteAssignmentStmtKind::For:
        if (const TSNode init = s.Field(gs.fieldInit);
            !ts_node_is_null(init) && ts_node_symbol(init) != gs.symVariableDeclaration)
            ExtractExpressions(init, index, source, facts);
        rec.conditionAlwaysTrue = IsLoopConditionAlwaysTrue(s.Field(gs.fieldCondition), true, source);
        ExtractExpressions(s.Field(gs.fieldCondition), index, source, facts);
        ExtractExpressions(s.Field(gs.fieldUpdate), index, source, facts);
        break;
    case DefiniteAssignmentStmtKind::DoWhile:
        ExtractExpressions(s.FieldOrNamed(gs.fieldCondition, 1), index, source, facts);
        break;
    case DefiniteAssignmentStmtKind::Case:
        rec.isDefaultCase =
            (ts_node_named_child_count(node) == 0 || parser::NodeText(node, source).starts_with("default"));
        if (!rec.isDefaultCase)
            ExtractExpressions(s.FirstNamed(), index, source, facts);
        break;
    case DefiniteAssignmentStmtKind::Expression:
        ExtractExpressions(s.FirstNamed(), index, source, facts);
        break;
    case DefiniteAssignmentStmtKind::VariableDeclaration:
        rec.declaratorBegin = static_cast<uint32_t>(facts.declarators.size());
        CollectDeclarators(node, index, source, facts);
        rec.declaratorCount = static_cast<uint32_t>(facts.declarators.size()) - rec.declaratorBegin;
        break;
    case DefiniteAssignmentStmtKind::Return:
        ExtractExpressions(s.FieldOrNamed(gs.fieldValue, 0), index, source, facts);
        break;
    default:
        break;
    }
    rec.readCount = static_cast<uint32_t>(facts.reads.size()) - rec.readBegin;
    rec.assignCount = static_cast<uint32_t>(facts.assignments.size()) - rec.assignBegin;
    rec.callCount = static_cast<uint32_t>(facts.calls.size()) - rec.callBegin;
    return rec;
}

/** @brief Discovers and populates records for all recognized statements in the document. */
static void CollectStatements(const NodeIndex& index, std::string_view source, DefiniteAssignmentFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol symbols[] = {gs.symStatementBlock,  gs.symIfStatement,         gs.symWhileStatement,
                                gs.symForStatement,    gs.symDoWhileStatement,    gs.symSwitchStatement,
                                gs.symCaseClause,      gs.symExpressionStatement, gs.symVariableDeclaration,
                                gs.symReturnStatement, gs.symBreakStatement,      gs.symContinueStatement};
    for (const TSSymbol sym : symbols)
        for (const TSNode node : index.Nodes(sym))
            facts.records.push_back(BuildRecord(node, index, source, facts));
}

/** @brief Links parent-child hierarchy indices across statement and block records. */
static void LinkHierarchy(DefiniteAssignmentFacts& facts)
{
    std::vector<uint32_t> stack;
    std::vector<std::vector<uint32_t>> children(facts.records.size());
    for (uint32_t i = 0; i < facts.records.size(); ++i)
    {
        while (!stack.empty() && facts.records[stack.back()].bytes.endByte <= facts.records[i].bytes.startByte)
            stack.pop_back();
        if (!stack.empty())
        {
            facts.records[i].parentIndex = stack.back();
            children[stack.back()].push_back(i);
        }
        stack.push_back(i);
    }
    for (uint32_t i = 0; i < facts.records.size(); ++i)
    {
        facts.records[i].childBegin = static_cast<uint32_t>(facts.childIndices.size());
        facts.records[i].childCount = static_cast<uint32_t>(children[i].size());
        facts.childIndices.insert(facts.childIndices.end(), children[i].begin(), children[i].end());
    }
}

/** @brief Collects callable definitions (functions and lambdas) and their body blocks. */
static void CollectCallables(const NodeIndex& index, DefiniteAssignmentFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSSymbol sym : {gs.symFuncDeclaration, gs.symLambdaExpression})
        for (const TSNode callable : index.Nodes(sym))
        {
            const SyntaxOperands s(callable, &index);
            TSNode body = s.Field(gs.fieldBody);
            if (ts_node_is_null(body) || ts_node_symbol(body) != gs.symStatementBlock)
                body = s.FirstNamed(gs.symStatementBlock);
            if (!ts_node_is_null(body))
                facts.functions.push_back({BytesOf(callable), RangeOf(callable), BytesOf(body), RangeOf(body),
                                           sym == gs.symLambdaExpression});
        }
}

DefiniteAssignmentFacts BuildDefiniteAssignmentFacts(const NodeIndex& index, std::string_view source)
{
    DefiniteAssignmentFacts facts;
    facts.assignmentSites = BuildAssignmentSiteFacts(index, source);
    CollectCallables(index, facts);
    CollectStatements(index, source, facts);
    SortBySpan(facts.records);
    SortBySpan(facts.functions);
    SortBySpan(facts.declarators);
    SortBySpan(facts.assignments);
    SortBySpan(facts.calls);
    LinkHierarchy(facts);
    for (auto& fn : facts.functions)
        if (const auto* rec = facts.FindRecord(fn.bodyBytes))
        {
            fn.rootRecordIndex = static_cast<uint32_t>(rec - facts.records.data());
            fn.recordBegin = fn.rootRecordIndex;
            uint32_t count = 0;
            while (fn.recordBegin + count < facts.records.size() &&
                   facts.records[fn.recordBegin + count].bytes.endByte <= fn.bodyBytes.endByte)
                ++count;
            fn.recordCount = count;
        }
    return facts;
}

} // namespace angel_lsp::analysis
