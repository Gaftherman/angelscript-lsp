#include "analysis/index/EngineDialectFacts.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Constructs an indexed source span from a Tree-Sitter AST node.
 * @param[in] node AST node to measure.
 * @return Source coordinates and byte bounds of the node, or empty span if null.
 */
static IndexedSourceSpan MakeSourceSpan(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return IndexedSourceSpan{};
    }
    const TSPoint s = ts_node_start_point(node);
    const TSPoint e = ts_node_end_point(node);
    return IndexedSourceSpan{IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)}, s.row, s.column, e.row,
                             e.column};
}

/**
 * @brief Evaluates whether a number literal AST node has integer lexical spelling.
 * @param[in] candidate AST node for the literal operand.
 * @param[in] source Document source text buffer.
 * @return True when candidate is a number literal without floating point exponents or suffixes.
 */
static bool IsIntegerLiteral(TSNode candidate, std::string_view source) noexcept
{
    if (ts_node_is_null(candidate))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(candidate) != gs.symNumberLiteral)
    {
        return false;
    }
    const std::string_view text = parser::NodeText(candidate, source);
    return text.find_first_of(".eEfF") == std::string_view::npos;
}

/**
 * @brief Inspects an initializer list for empty element comma positions.
 * @param[in] node AST node of the initializer list.
 * @param[in] gs Precompiled grammar symbol table.
 * @param[out] facts Fact collection receiving detected empty element spans.
 */
static void ProcessInitializerList(TSNode node, const parser::GrammarSymbols& gs, EngineDialectFacts& facts)
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        TSSymbol prevSym = 0;
        TSPoint prevStart{};
        uint32_t prevStartByte = 0;
        uint32_t prevEndByte = 0;
        do
        {
            const TSNode ch = ts_tree_cursor_current_node(&cursor);
            const TSSymbol currSym = ts_node_symbol(ch);
            if (prevSym == gs.tokComma && (currSym == gs.tokComma || currSym == gs.tokCloseBrace))
            {
                facts.emptyListElements.push_back(IndexedSourceSpan{IndexedByteRange{prevStartByte, prevEndByte},
                                                                    prevStart.row, prevStart.column, prevStart.row,
                                                                    prevStart.column + 1});
            }
            prevSym = currSym;
            prevStart = ts_node_start_point(ch);
            prevStartByte = ts_node_start_byte(ch);
            prevEndByte = ts_node_end_byte(ch);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Extracts variable declarators initialized with character literal strings.
 * @param[in] node Variable declaration AST node.
 * @param[in] gs Precompiled grammar symbol table.
 * @param[in] source Document source text buffer.
 * @param[out] facts Fact collection receiving detected declarator facts.
 */
static void ProcessVariableDeclaration(TSNode node, const parser::GrammarSymbols& gs, std::string_view source,
                                       EngineDialectFacts& facts)
{
    const TSNode typeNode = parser::GetChildByField(node, gs.fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        return;
    }
    const IndexedByteRange typeRange{ts_node_start_byte(typeNode), ts_node_end_byte(typeNode)};
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode declarator = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(declarator) != gs.symVariableDeclarator)
            {
                continue;
            }
            const TSNode value = parser::GetChildByField(declarator, gs.fieldValue);
            if (ts_node_is_null(value) || ts_node_symbol(value) != gs.symStringLiteral)
            {
                continue;
            }
            const uint32_t from = ts_node_start_byte(value);
            if (from < source.size() && source[from] == '\'')
            {
                facts.charLiteralDeclarators.push_back(IndexedCharLiteralDeclarator{typeRange, MakeSourceSpan(value)});
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Collects named argument invocations within a call argument list.
 * @param[in] node AST node of the argument list.
 * @param[in] gs Precompiled grammar symbol table.
 * @param[out] facts Fact collection receiving extracted named arguments.
 */
static void ProcessArgumentList(TSNode node, const parser::GrammarSymbols& gs, EngineDialectFacts& facts)
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode argument = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_named(argument) || ts_node_symbol(argument) != gs.symAssignmentExpression)
            {
                continue;
            }
            const TSNode target = parser::GetChildByField(argument, gs.fieldLeft);
            if (ts_node_is_null(target))
            {
                continue;
            }
            const TSSymbol targetSym = ts_node_symbol(target);
            if (targetSym != gs.symIdentifier && targetSym != gs.symScopedIdentifier)
            {
                continue;
            }
            if (targetSym == gs.symScopedIdentifier && ts_node_named_child_count(target) != 1)
            {
                continue;
            }
            facts.namedArguments.push_back(
                IndexedNamedArgument{MakeSourceSpan(argument), MakeSourceSpan(target),
                                     IndexedByteRange{ts_node_start_byte(target), ts_node_end_byte(target)}});
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Collects assignment expression metadata, target span, and identifier bounds for plain assignments.
 * @param[in] node AST node of the assignment expression.
 * @param[in] gs Precompiled grammar symbol table.
 * @param[out] facts Fact collection receiving extracted assignment facts.
 */
static void ProcessAssignmentExpression(TSNode node, const parser::GrammarSymbols& gs, EngineDialectFacts& facts)
{
    const TSNode op = parser::GetChildByField(node, gs.fieldOperator);
    const TSNode target = parser::GetChildByField(node, gs.fieldLeft);
    if (ts_node_is_null(op) || ts_node_symbol(op) != gs.tokAssign || ts_node_is_null(target) ||
        parser::IsHandleAssignmentTarget(target))
    {
        return;
    }
    IndexedByteRange targetIdRange{ts_node_start_byte(target), ts_node_end_byte(target)};
    const TSSymbol targetSym = ts_node_symbol(target);
    if (targetSym == gs.symMemberExpression)
    {
        const TSNode memberNode = parser::GetChildByField(target, gs.fieldMember);
        if (!ts_node_is_null(memberNode))
        {
            targetIdRange = IndexedByteRange{ts_node_start_byte(memberNode), ts_node_end_byte(memberNode)};
        }
    }
    facts.assignmentExpressions.push_back(
        IndexedAssignmentExpression{MakeSourceSpan(node), MakeSourceSpan(target), targetIdRange});
}

/**
 * @brief Records multiline string literal spans that do not use heredoc syntax.
 * @param[in] node AST node of the string literal.
 * @param[in] source Document source text buffer.
 * @param[out] facts Fact collection receiving extracted multiline string spans.
 */
static void ProcessStringLiteral(TSNode node, std::string_view source, EngineDialectFacts& facts)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    if (end.row > start.row)
    {
        const uint32_t from = ts_node_start_byte(node);
        const bool isHeredoc = from + 3 <= source.size() && source.compare(from, 3, "\"\"\"") == 0;
        if (!isHeredoc)
        {
            facts.multilineStrings.push_back(IndexedSourceSpan{IndexedByteRange{from, ts_node_end_byte(node)},
                                                               start.row, start.column, end.row, end.column});
        }
    }
}

EngineDialectFacts BuildEngineDialectFacts(const NodeIndex& index, std::string_view source)
{
    EngineDialectFacts facts;
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 7> symbols{gs.symForeachStatement, gs.symInitializerList, gs.symVariableDeclaration,
                                          gs.symBinaryExpression, gs.symArgumentList,    gs.symAssignmentExpression,
                                          gs.symStringLiteral};

    index.ForEachNodeOrdered(symbols,
                             [&](TSNode node)
                             {
                                 const TSSymbol sym = ts_node_symbol(node);
                                 if (sym == gs.symForeachStatement)
                                 {
                                     facts.foreachStatements.push_back(MakeSourceSpan(node));
                                 }
                                 else if (sym == gs.symInitializerList)
                                 {
                                     ProcessInitializerList(node, gs, facts);
                                 }
                                 else if (sym == gs.symVariableDeclaration)
                                 {
                                     ProcessVariableDeclaration(node, gs, source, facts);
                                 }
                                 else if (sym == gs.symBinaryExpression)
                                 {
                                     const TSNode op = parser::GetChildByField(node, gs.fieldOperator);
                                     if (!ts_node_is_null(op) && ts_node_symbol(op) == gs.tokSlash)
                                     {
                                         const TSNode l = parser::GetChildByField(node, gs.fieldLeft);
                                         const TSNode r = parser::GetChildByField(node, gs.fieldRight);
                                         if (IsIntegerLiteral(l, source) && IsIntegerLiteral(r, source))
                                         {
                                             facts.integerDivisions.push_back(MakeSourceSpan(node));
                                         }
                                     }
                                 }
                                 else if (sym == gs.symArgumentList)
                                 {
                                     ProcessArgumentList(node, gs, facts);
                                 }
                                 else if (sym == gs.symAssignmentExpression)
                                 {
                                     ProcessAssignmentExpression(node, gs, facts);
                                 }
                                 else if (sym == gs.symStringLiteral)
                                 {
                                     ProcessStringLiteral(node, source, facts);
                                 }
                             });

    return facts;
}

} // namespace angel_lsp::analysis
