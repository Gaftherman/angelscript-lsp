#include "analysis/index/HoverFacts.h"
#include "analysis/index/HoverFactsInternal.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/IncludeResolver.h"

#include <optional>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
static std::pair<size_t, size_t> FindLineSpan(std::string_view sourceCode, uint32_t targetLine)
{
    size_t start = 0;
    for (uint32_t current = 0; current < targetLine; ++current)
    {
        const size_t nextBreak = sourceCode.find('\n', start);
        if (nextBreak == std::string_view::npos)
        {
            return {std::string_view::npos, std::string_view::npos};
        }
        start = nextBreak + 1;
    }
    const size_t end = sourceCode.find('\n', start);
    return {start, end == std::string_view::npos ? sourceCode.size() : end};
}

std::optional<IndexedHoverIncludeDirective> FindIncludeDirective(std::string_view sourceCode, uint32_t line,
                                                                 std::optional<uint32_t> column)
{
    if (sourceCode.find('#') == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto [lineStart, lineEnd] = FindLineSpan(sourceCode, line);
    if (lineStart == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string_view lineText = sourceCode.substr(lineStart, lineEnd - lineStart);
    const size_t colParam = column.has_value() ? static_cast<size_t>(*column) : std::string_view::npos;
    const auto info = angel_lsp::utils::IncludeResolver::ParseSingleLineInclude(lineText, colParam);
    if (!info)
    {
        return std::nullopt;
    }
    IndexedHoverIncludeDirective dir{};
    dir.hash = info->hashPos;
    dir.closeQuote = info->closeDelim;
    dir.pathStartByte = static_cast<uint32_t>(lineStart + info->openDelim + 1);
    dir.pathEndByte = static_cast<uint32_t>(lineStart + info->closeDelim);
    return dir;
}

static void PopulateStructuralRanges(TSNode targetNode, TSNode parent, std::string_view sourceCode,
                                     IndexedHoverSite& site)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(parent, gs.fieldObject);
        if (!ts_node_is_null(obj))
        {
            site.receiverRange = {ts_node_start_byte(obj), ts_node_end_byte(obj)};
        }
    }

    if (site.isNamedArgumentLabel)
    {
        TSNode namedArgCall = parser::GetParent(parent);
        if (!ts_node_is_null(namedArgCall))
        {
            site.namedArgCallRange = {ts_node_start_byte(namedArgCall), ts_node_end_byte(namedArgCall)};
        }
    }
    if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symParameter)
    {
        TSNode nameNode = parser::GetChildByField(parent, gs.fieldName);
        site.isParameterName = (!ts_node_is_null(nameNode) && ts_node_eq(nameNode, targetNode)) ? 1 : 0;
    }

    TSNode call = FindEnclosingCallNode(targetNode);
    if (!ts_node_is_null(call))
    {
        site.enclosingCallRange = {ts_node_start_byte(call), ts_node_end_byte(call)};
        TSNode args = parser::GetChildByField(call, gs.fieldArguments);
        if (ts_node_is_null(args))
        {
            args = parser::FindChildWithSymbol(call, gs.symArgumentList);
        }
        if (!ts_node_is_null(args))
        {
            site.argumentListRange = {ts_node_start_byte(args), ts_node_end_byte(args)};
        }
    }

    if (IsDirectInitDeclarator(targetNode))
    {
        site.isDirectInitDeclarator = 1;
        TSNode decl = (ts_node_symbol(targetNode) == gs.symVariableDeclarator) ? targetNode : parent;
        if (!ts_node_is_null(decl) && ts_node_symbol(decl) == gs.symVariableDeclarator)
        {
            site.directInitDeclaratorRange = {ts_node_start_byte(decl), ts_node_end_byte(decl)};
            TSNode args = parser::GetChildByField(decl, gs.fieldArguments);
            if (!ts_node_is_null(args))
            {
                site.argumentListRange = {ts_node_start_byte(args), ts_node_end_byte(args)};
            }
        }
    }

    if (site.kind == HoverNodeKind::Lambda)
    {
        TSNode lam = FindEnclosingLambda(targetNode);
        if (!ts_node_is_null(lam))
        {
            site.enclosingLambdaRange = {ts_node_start_byte(lam), ts_node_end_byte(lam)};
        }
    }

    if (site.kind == HoverNodeKind::Auto)
    {
        TSNode autoDecl = FindEnclosingAutoDeclaration(targetNode);
        if (!ts_node_is_null(autoDecl))
        {
            site.enclosingAutoDeclRange = {ts_node_start_byte(autoDecl), ts_node_end_byte(autoDecl)};
            TSNode typeNode = parser::GetChildByField(autoDecl, gs.fieldVarType);
            if (ts_node_is_null(typeNode))
            {
                typeNode = parser::GetChildByField(autoDecl, gs.fieldType);
            }
            if (!ts_node_is_null(typeNode) && analysis::IsHandleTypeView(parser::GetNodeTextView(typeNode, sourceCode)))
            {
                site.isAutoHandle = 1;
            }
        }
    }

    if (!ts_node_is_null(parent))
    {
        const TSSymbol pSym = ts_node_symbol(parent);
        if (pSym == gs.symScopedIdentifier)
        {
            site.scopeQualifierRange = {ts_node_start_byte(parent), ts_node_end_byte(targetNode)};
            TSNode nextMember = FindNextScopedMemberNode(parent, targetNode);
            if (!ts_node_is_null(nextMember))
            {
                site.nextScopedMemberRange = {ts_node_start_byte(nextMember), ts_node_end_byte(nextMember)};
            }
        }
        else if (pSym == gs.symDatatype)
        {
            TSNode scope = FindDatatypeScopeNode(parent);
            if (!ts_node_is_null(scope))
            {
                site.datatypeScopeRange = {ts_node_start_byte(scope), ts_node_end_byte(scope)};
            }
        }
        else if (pSym == gs.symEnumMember)
        {
            TSNode enumDecl = parser::GetParent(parent);
            if (!ts_node_is_null(enumDecl) && ts_node_symbol(enumDecl) == gs.symEnumDeclaration)
            {
                TSNode nameNode = parser::GetChildByField(enumDecl, gs.fieldName);
                if (!ts_node_is_null(nameNode))
                {
                    site.enclosingEnumNameRange = {ts_node_start_byte(nameNode), ts_node_end_byte(nameNode)};
                }
            }
        }
    }
}

static void ClassifyHoverTargetKind(TSNode targetNode, TSNode parent, std::string_view nodeText, IndexedHoverSite& site)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(targetNode);

    const bool isQuoted = nodeText.size() >= 2 && ((nodeText.front() == '"' && nodeText.back() == '"') ||
                                                   (nodeText.front() == '\'' && nodeText.back() == '\''));
    if ((sym == gs.symStringLiteral || sym == gs.symBinaryExpression) && isQuoted)
    {
        site.kind = sym == gs.symBinaryExpression ? HoverNodeKind::ConcatenatedString : HoverNodeKind::StringLiteral;
        TSNode topPlus = FindTopmostBinaryPlus(targetNode);
        if (!ts_node_is_null(topPlus) && ts_node_symbol(topPlus) == gs.symBinaryExpression)
        {
            site.topmostBinaryPlusRange = {ts_node_start_byte(topPlus), ts_node_end_byte(topPlus)};
        }
    }
    else if (nodeText == "auto" || nodeText == "auto@")
    {
        site.kind = HoverNodeKind::Auto;
        if (nodeText == "auto@")
        {
            site.isAutoHandle = 1;
        }
    }
    else if (nodeText == "this")
    {
        site.kind = HoverNodeKind::This;
    }
    else if (nodeText == "function")
    {
        site.kind = HoverNodeKind::Lambda;
    }
    else if (analysis::IsPrimitiveTypeName(nodeText) || nodeText == "?")
    {
        site.kind = HoverNodeKind::PrimitiveOrType;
    }
    else if (IsNamedArgumentLabelAtCursor(targetNode, parent))
    {
        site.kind = HoverNodeKind::NamedArgument;
    }
    else if (IsMemberChildOfExpression(targetNode, parent))
    {
        site.kind = HoverNodeKind::MemberAccess;
    }
    else if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symEnumMember)
    {
        site.kind = HoverNodeKind::EnumMember;
    }
    else if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symParameter)
    {
        site.kind = HoverNodeKind::Parameter;
    }
    else if (IsScopeQualifier(targetNode))
    {
        site.kind = HoverNodeKind::ScopedMember;
    }
    else if (IsQualifiedMemberNode(targetNode))
    {
        site.kind = HoverNodeKind::ScopedMember;
    }
    else
    {
        site.kind = HoverNodeKind::Identifier;
    }
}

std::optional<IndexedHoverSite> FindHoverSite(const NodeIndex& index, std::string_view sourceCode, uint32_t line,
                                              uint32_t column)
{
    TSNode root = index.Root();
    if (ts_node_is_null(root) || sourceCode.empty())
    {
        return std::nullopt;
    }

    TSPoint pt{line, column};
    TSNode targetNode = ExtractHoverNode(root, sourceCode, pt);
    if (ts_node_is_null(targetNode))
    {
        return std::nullopt;
    }

    IndexedHoverSite site{};
    site.targetStartByte = ts_node_start_byte(targetNode);
    site.targetEndByte = ts_node_end_byte(targetNode);
    TSPoint sp = ts_node_start_point(targetNode);
    TSPoint ep = ts_node_end_point(targetNode);
    site.startLine = sp.row;
    site.startColumn = sp.column;
    site.endLine = ep.row;
    site.endColumn = ep.column;

    TSNode parent = parser::GetParent(targetNode);
    if (!ts_node_is_null(parent))
    {
        site.parentStartByte = ts_node_start_byte(parent);
        site.parentEndByte = ts_node_end_byte(parent);
    }

    const std::string_view nodeText =
        sourceCode.substr(site.targetStartByte, site.targetEndByte - site.targetStartByte);
    ClassifyHoverTargetKind(targetNode, parent, nodeText, site);
    site.isNamedArgumentLabel = IsNamedArgumentLabelAtCursor(targetNode, parent) ? 1 : 0;
    site.isMemberOfExpression = IsMemberChildOfExpression(targetNode, parent) ? 1 : 0;
    site.isScopeQualifier = IsScopeQualifier(targetNode) ? 1 : 0;
    site.isQualifiedMember = IsQualifiedMemberNode(targetNode) ? 1 : 0;
    PopulateStructuralRanges(targetNode, parent, sourceCode, site);

    site.contextPreference = CheckContextPreference(targetNode);
    const auto gsq = parser::GetGlobalScopeQualifier(targetNode, sourceCode);
    site.isGlobalScopeQualified = gsq.isGlobal ? 1 : 0;
    site.isDirectRootGlobal = (gsq.isGlobal && gsq.isDirectRoot) ? 1 : 0;

    return site;
}
} // namespace angel_lsp::analysis
