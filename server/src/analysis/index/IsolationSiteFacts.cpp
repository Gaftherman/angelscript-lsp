#include "analysis/index/IsolationSiteFacts.h"

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <tuple>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Syntax node.
 * @return Line and column span of the node, or a zero range if null.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
        return {};
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

static std::string_view CleanTypeName(std::string_view raw) noexcept
{
    std::string_view sv = utils::TrimView(raw);
    TrimTypeWhitespace(sv);
    sv = StripTrailingDecorationsView(sv);
    TrimTypeWhitespace(sv);
    return sv;
}

/**
 * @brief Checks whether a declaration node has a direct shared modifier token.
 * @param[in] node Declaration syntax node.
 * @param[in] gs Grammar symbol table.
 * @return True when a child token of the node is the shared modifier.
 */
static bool NodeHasSharedToken(TSNode node, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(node))
        return false;
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return false;
    }
    bool found = false;
    do
    {
        const TSNode child = ts_tree_cursor_current_node(&cursor);
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.tokShared || ((sym == gs.symDeclarationModifier || sym == gs.symSharedExternalModifier) &&
                                    !ts_node_is_null(parser::FindChildWithSymbol(child, gs.tokShared))))
        {
            found = true;
            break;
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));
    ts_tree_cursor_delete(&cursor);
    return found;
}

/**
 * @brief Checks whether any descendant of a node has the given grammar symbol.
 * @param[in] root Subtree root to scan.
 * @param[in] targetSym Grammar symbol to look for.
 * @return True when a descendant of root carries targetSym.
 */
static bool ContainsSymbolDescendant(TSNode root, TSSymbol targetSym) noexcept
{
    if (ts_node_is_null(root))
        return false;
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool found = false;
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        for (uint32_t d = 1; d > 0 && d < 16;)
        {
            if (ts_node_symbol(ts_tree_cursor_current_node(&cursor)) == targetSym)
            {
                found = true;
                break;
            }
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                ++d;
                continue;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
                continue;
            while (d > 0 && !ts_tree_cursor_goto_next_sibling(&cursor))
            {
                ts_tree_cursor_goto_parent(&cursor);
                --d;
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
    return found;
}

/**
 * @brief Checks that a shared modifier is not followed by an error node holding a primitive type.
 * @param[in] parent Modifier node that owns the shared token.
 * @param[in] gs Grammar symbol table.
 * @return False when the next significant sibling is an error node containing a primitive type; true otherwise.
 */
static bool IsValidEnumSharedModifier(TSNode parent, const parser::GrammarSymbols& gs) noexcept
{
    TSNode next = ts_node_next_sibling(parent);
    while (!ts_node_is_null(next))
    {
        const TSSymbol sym = ts_node_symbol(next);
        if (sym != gs.symSharedExternalModifier && sym != gs.symDeclarationModifier && sym != gs.symComment)
            break;
        next = ts_node_next_sibling(next);
    }
    return !(!ts_node_is_null(next) && ts_node_is_error(next) && ContainsSymbolDescendant(next, gs.symPrimitiveType));
}

static bool IsValidSharedNode(TSNode sharedNode, const parser::GrammarSymbols& gs, TSNode& outDecl) noexcept
{
    const TSNode parent = parser::GetParent(sharedNode);
    if (ts_node_is_null(parent))
        return true;
    const TSSymbol pSym = ts_node_symbol(parent);
    outDecl = (pSym == gs.symDeclarationModifier || pSym == gs.symSharedExternalModifier) ? parser::GetParent(parent)
                                                                                          : parent;
    if (ts_node_is_null(outDecl) || ts_node_is_error(outDecl))
        return false;
    const TSSymbol sym = ts_node_symbol(outDecl);
    const bool isAllowed = (sym == gs.symClassDeclaration || sym == gs.symInterfaceDeclaration ||
                            sym == gs.symFuncdefDeclaration || sym == gs.symFuncDeclaration);
    return isAllowed || (sym == gs.symEnumDeclaration && IsValidEnumSharedModifier(parent, gs));
}

static void InspectIdentifier(TSNode node, const parser::GrammarSymbols& gs, IsolationIdentifierSiteFact& fact) noexcept
{
    parser::ForEachAncestor(
        parser::GetParent(node),
        [&](TSNode cur) noexcept -> bool
        {
            const TSSymbol sym = ts_node_symbol(cur);
            if (sym == gs.symCallExpression)
            {
                const TSNode fn = parser::GetCallCallee(cur);
                if (!ts_node_is_null(fn) && (fn.id == node.id || parser::GetParent(node).id == fn.id))
                {
                    fact.isSpecialContext = fact.isCallee = true;
                    return false;
                }
            }
            if (sym == gs.symDatatype || sym == gs.symType || sym == gs.symBaseClassList)
            {
                fact.isSpecialContext = true;
                return false;
            }
            if (sym == gs.symVariableDeclarator)
            {
                const TSNode val = parser::GetChildByField(cur, gs.fieldValue);
                if (ts_node_is_null(val) || ts_node_start_byte(node) < ts_node_start_byte(val))
                {
                    fact.isSpecialContext = fact.isDeclarationName = true;
                    return false;
                }
            }
            if (sym == gs.symFuncDeclaration || sym == gs.symClassDeclaration || sym == gs.symParameter ||
                sym == gs.symEnumMember)
            {
                const TSNode n = parser::GetChildByField(cur, gs.fieldName);
                if (!ts_node_is_null(n) && (n.id == node.id || ts_node_start_byte(node) == ts_node_start_byte(n)))
                {
                    fact.isSpecialContext = fact.isDeclarationName = true;
                    return false;
                }
            }
            if (sym == gs.symMemberExpression)
            {
                const TSNode prop = parser::GetChildByField(cur, gs.fieldMember);
                if (!ts_node_is_null(prop) && prop.id == node.id)
                {
                    fact.isSpecialContext = fact.isMember = true;
                    return false;
                }
            }
            return sym != gs.symStatementBlock && sym != gs.symFuncDeclaration && sym != gs.symClassDeclaration;
        });
}

template <typename Fact>
static std::pair<uint32_t, uint32_t> SliceWithin(const std::vector<Fact>& facts, IndexedByteRange span) noexcept
{
    const auto first = std::lower_bound(facts.begin(), facts.end(), span.startByte,
                                        [](const Fact& f, uint32_t p) { return f.bytes.startByte < p; });
    const auto last = std::lower_bound(first, facts.end(), span.endByte,
                                       [](const Fact& f, uint32_t p) { return f.bytes.startByte < p; });
    return {static_cast<uint32_t>(first - facts.begin()), static_cast<uint32_t>(last - first)};
}

static std::pair<std::string_view, bool> EnclosingContext(const NodeIndex& index, uint32_t offset,
                                                          std::string_view source) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode cls = index.FindEnclosingClassOrMixin(offset);
    std::string_view className{};
    bool shared = false;
    if (!ts_node_is_null(cls))
    {
        const TSNode nameNode = SyntaxOperands(cls, &index).Field(gs.fieldName);
        if (!ts_node_is_null(nameNode))
            className = parser::NodeText(nameNode, source);
        shared = NodeHasSharedToken(cls, gs);
    }
    if (!shared)
    {
        const TSNode fn = index.FindEnclosingCallable(offset);
        if (!ts_node_is_null(fn))
            shared = NodeHasSharedToken(fn, gs);
    }
    return {className, shared};
}

static void CollectSharedAndDecls(const NodeIndex& index, std::string_view source, IsolationSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.tokShared))
    {
        if (index.IsInsideComment(ts_node_start_byte(node)))
            continue;
        TSNode declNode{};
        const bool valid = IsValidSharedNode(node, gs, declNode);
        facts.sharedModifiers.push_back({BytesOf(node), RangeOf(node), BytesOf(declNode), RangeOf(declNode),
                                         ts_node_is_null(declNode) ? uint16_t{0} : ts_node_symbol(declNode), valid});
    }
    for (const TSNode node : index.Nodes(gs.symClassDeclaration))
    {
        const TSNode name = SyntaxOperands(node, &index).Field(gs.fieldName);
        facts.classes.push_back({BytesOf(node), RangeOf(node), RangeOf(name),
                                 ts_node_is_null(name) ? std::string_view{} : parser::NodeText(name, source),
                                 NodeHasSharedToken(node, gs), 0, 0, 0, 0});
    }
    for (const TSNode node : index.Nodes(gs.symFuncDeclaration))
    {
        const TSNode name = SyntaxOperands(node, &index).Field(gs.fieldName);
        const bool shared = NodeHasSharedToken(node, gs);
        const uint32_t start = ts_node_start_byte(node);
        const std::string_view cls = EnclosingContext(index, start, source).first;
        const TSNode classNode = index.FindEnclosingClassOrMixin(start);
        const bool classShared = !ts_node_is_null(classNode) && NodeHasSharedToken(classNode, gs);
        facts.functions.push_back({BytesOf(node), RangeOf(node), RangeOf(name),
                                   ts_node_is_null(name) ? std::string_view{} : parser::NodeText(name, source), cls,
                                   shared, classShared, shared || classShared, 0, 0, 0, 0, 0, 0});
    }
}

static void CollectExpressions(const NodeIndex& index, std::string_view source, IsolationSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symDatatype))
    {
        const auto [cls, sh] = EnclosingContext(index, ts_node_start_byte(node), source);
        const auto raw = parser::NodeText(node, source);
        facts.types.push_back({BytesOf(node), RangeOf(node), raw, CleanTypeName(raw), cls, sh});
    }
    const auto addCall = [&](TSNode node)
    {
        const TSNode fn = parser::GetCallCallee(node);
        const TSSymbol sym = ts_node_is_null(fn) ? 0 : ts_node_symbol(fn);
        const auto [cls, sh] = EnclosingContext(index, ts_node_start_byte(node), source);
        facts.calls.push_back({BytesOf(node), RangeOf(node), BytesOf(fn), RangeOf(fn),
                               ts_node_is_null(fn) ? std::string_view{} : utils::TrimView(parser::NodeText(fn, source)),
                               cls, sym == gs.symIdentifier || sym == gs.symScopedIdentifier, sh});
    };
    for (const TSNode node : index.Nodes(gs.symCallExpression))
        addCall(node);
    for (const TSNode node : index.Nodes(gs.symConstructCallExpression))
        addCall(node);
    for (const TSNode node : index.Nodes(gs.symIdentifier))
    {
        const uint32_t start = ts_node_start_byte(node);
        if (index.IsInsideComment(start))
            continue;
        const auto [cls, sh] = EnclosingContext(index, start, source);
        IsolationIdentifierSiteFact fact{BytesOf(node), RangeOf(node), utils::TrimView(parser::NodeText(node, source)),
                                         cls,           false,         false,
                                         false,         false,         sh};
        InspectIdentifier(node, gs, fact);
        facts.identifiers.push_back(fact);
    }
}

IsolationSiteFacts BuildIsolationSiteFacts(const NodeIndex& index, std::string_view source)
{
    IsolationSiteFacts facts;
    CollectSharedAndDecls(index, source, facts);
    CollectExpressions(index, source, facts);

    SortBySpan(facts.sharedModifiers);
    SortBySpan(facts.classes);
    SortBySpan(facts.functions);
    SortBySpan(facts.types);
    SortBySpan(facts.calls);
    SortBySpan(facts.identifiers);

    for (auto& cls : facts.classes)
    {
        std::tie(cls.functionBegin, cls.functionCount) = SliceWithin(facts.functions, cls.bytes);
        std::tie(cls.typeBegin, cls.typeCount) = SliceWithin(facts.types, cls.bytes);
    }
    for (auto& fn : facts.functions)
    {
        std::tie(fn.identifierBegin, fn.identifierCount) = SliceWithin(facts.identifiers, fn.bytes);
        std::tie(fn.callBegin, fn.callCount) = SliceWithin(facts.calls, fn.bytes);
        std::tie(fn.typeBegin, fn.typeCount) = SliceWithin(facts.types, fn.bytes);
    }
    return facts;
}

} // namespace angel_lsp::analysis
