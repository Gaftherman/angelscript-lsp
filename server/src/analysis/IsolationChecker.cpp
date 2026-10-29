#include "analysis/IsolationChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
void EmitAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code, const std::string& arg)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code, arg);
}

std::string TrimWhitespace(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string CleanSharedTypeName(std::string_view raw)
{
    std::string text(raw);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '@' || text.back() == '&'))
    {
        text.pop_back();
    }
    return text;
}

bool IsSymbolShared(const Symbol& s)
{
    switch (s.type)
    {
    case SymbolType::Class:
        return s.GetClass().modifiers.isShared;
    case SymbolType::Interface:
        return s.GetInterface().modifiers.isShared;
    case SymbolType::Enum:
        return s.GetEnum().modifiers.isShared;
    case SymbolType::Funcdef:
        return s.GetFuncdef().modifiers.isShared;
    default:
        return true;
    }
}

bool IsCoreOrRegisteredType(std::string_view typeName, const SemanticAnalysisRequest& request)
{
    return typeName.empty() || IsCorePrimitive(typeName) || typeName == request.GetStringTypeName() ||
           typeName == request.GetArrayTypeName() || request.IsRegisteredSymbol(std::string(typeName));
}

/**
 * @brief Innermost containing scope, falling back to `root` rather than nullptr.
 *
 * Named apart from analysis::FindInnermostScope on purpose: that one reports "no scope"
 * when the point lies outside the root, this one hands back the root. The shared-isolation
 * walk needs a scope for every node it visits, so the fallback is the behaviour it wants.
 */
const Scope* FindEnclosingScopeOrRoot(const Scope* root, uint32_t line, uint32_t character)
{
    if (!root)
    {
        return nullptr;
    }

    const Scope* current = root;
    while (true)
    {
        const Scope* narrower = nullptr;
        for (const auto& child : current->children)
        {
            if (child->startLine < line || (child->startLine == line && child->startCharacter <= character))
            {
                if (child->endLine > line || (child->endLine == line && child->endCharacter >= character))
                {
                    narrower = child.get();
                    break;
                }
            }
        }

        if (!narrower)
        {
            return current;
        }
        current = narrower;
    }
}

bool IsLocalVariableOrParameter(const Scope* startScope, std::string_view name)
{
    const Scope* cur = startScope;
    while (cur)
    {
        for (const auto& def : cur->definitions)
        {
            if (def.name == name)
            {
                const Scope* check = cur;
                while (check)
                {
                    if (check->isFunctionScope)
                    {
                        return true;
                    }
                    check = check->parent;
                }
                return false;
            }
        }
        cur = cur->parent;
    }
    return false;
}

bool NodeHasModifierToken(TSNode node, std::string_view modifierName, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return false;
    }
    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        const uint32_t start = ts_node_start_byte(child);
        const uint32_t end = ts_node_end_byte(child);
        if (start < end && end <= sourceCode.size())
        {
            if (sourceCode.substr(start, end - start) == modifierName)
            {
                ts_tree_cursor_delete(&cursor);
                return true;
            }
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);
    return false;
}

bool IsAllowedSharedEntity(std::string_view afterShared)
{
    if (afterShared.starts_with("class ") || afterShared.starts_with("class\t") ||
        afterShared.starts_with("interface ") || afterShared.starts_with("interface\t") ||
        afterShared.starts_with("enum ") || afterShared.starts_with("enum\t") || afterShared.starts_with("funcdef ") ||
        afterShared.starts_with("funcdef\t") || afterShared.starts_with("external "))
    {
        return true;
    }

    size_t parenPos = afterShared.find('(');
    size_t semiPos = afterShared.find(';');
    size_t eqPos = afterShared.find('=');

    return parenPos != std::string_view::npos && (semiPos == std::string_view::npos || parenPos < semiPos) &&
           (eqPos == std::string_view::npos || parenPos < eqPos);
}

void CheckSharedLine(std::string_view line, uint32_t lineNum, DiagnosticContext& ctx)
{
    size_t col = 0;
    while (col < line.size() && (line[col] == ' ' || line[col] == '\t'))
    {
        ++col;
    }

    std::string_view trimmed = line.substr(col);
    if (trimmed.starts_with("//") || trimmed.starts_with("/*"))
    {
        return;
    }

    if (!trimmed.starts_with("shared ") && !trimmed.starts_with("shared\t"))
    {
        return;
    }

    std::string codePart(trimmed);
    size_t commentIdx = codePart.find("//");
    if (commentIdx != std::string::npos)
    {
        codePart = codePart.substr(0, commentIdx);
    }

    std::string afterShared = codePart.substr(6);
    while (!afterShared.empty() && (afterShared.front() == ' ' || afterShared.front() == '\t'))
    {
        afterShared.erase(afterShared.begin());
    }

    if (!IsAllowedSharedEntity(afterShared))
    {
        uint32_t startCol = static_cast<uint32_t>(col);
        uint32_t endCol = static_cast<uint32_t>(col + 6);
        ctx.LogRule("CheckSharedEntityEligibility", "as-err-shared-not-allowed-on-entity", {});
        ctx.EmitAtRange({lineNum, startCol, lineNum, endCol}, "as-err-shared-not-allowed-on-entity");
    }
}

void CheckSharedEntityEligibility(std::string_view sourceCode, DiagnosticContext& ctx)
{
    size_t startPos = 0;
    uint32_t lineNum = 0;

    while (startPos < sourceCode.size())
    {
        size_t endPos = sourceCode.find('\n', startPos);
        if (endPos == std::string_view::npos)
        {
            endPos = sourceCode.size();
        }

        std::string_view line = sourceCode.substr(startPos, endPos - startPos);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        CheckSharedLine(line, lineNum, ctx);

        startPos = endPos + 1;
        ++lineNum;
    }
}

bool IsCallFunctionNode(TSNode p, TSNode node)
{
    TSNode fn = parser::GetCallCallee(p);
    return !ts_node_is_null(fn) && (fn.id == node.id || parser::GetParent(node).id == fn.id);
}

bool IsDeclarationNameNode(TSNode p, TSNode node, TSSymbol pSym)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (pSym == gs.symVariableDeclarator)
    {
        TSNode val = parser::GetChildByField(p, gs.fieldValue);
        return ts_node_is_null(val) || ts_node_start_byte(node) < ts_node_start_byte(val);
    }
    if (pSym == gs.symFuncDeclaration || pSym == gs.symClassDeclaration || pSym == gs.symParameter ||
        pSym == gs.symEnumMember)
    {
        TSNode nameNode = parser::GetChildByField(p, gs.fieldName);
        return !ts_node_is_null(nameNode) &&
               (nameNode.id == node.id || ts_node_start_byte(node) == ts_node_start_byte(nameNode));
    }
    if (pSym == gs.symMemberExpression)
    {
        TSNode propNode = parser::GetChildByField(p, gs.fieldMember);
        return !ts_node_is_null(propNode) && propNode.id == node.id;
    }
    return false;
}

struct IsolationVisitor
{
    const IsolationCheckRequest& request;
    DiagnosticContext& ctx;
    std::string currentClassName;
    bool isCurrentClassShared = false;

    bool IsClassShared(TSNode node, const std::string& className) const
    {
        if (NodeHasModifierToken(node, "shared", request.sourceCode))
        {
            return true;
        }
        if (!className.empty())
        {
            if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(className))
            {
                for (const auto& s : *syms)
                {
                    if (s.type == SymbolType::Class && s.fileUri == ctx.request.fileUri &&
                        s.GetClass().modifiers.isShared)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void VisitClassDeclaration(TSNode node, int depth)
    {
        TSNode nameNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName);
        std::string className = GetNodeText(nameNode, request.sourceCode);
        bool classShared = IsClassShared(node, className);

        std::string oldClassName = currentClassName;
        bool oldClassShared = isCurrentClassShared;
        currentClassName = className;
        isCurrentClassShared = classShared;

        ForEachChildNode(node, [&](TSNode child) { Visit(child, classShared, depth + 1); });

        currentClassName = oldClassName;
        isCurrentClassShared = oldClassShared;
    }

    bool IsFunctionShared(TSNode node, bool inSharedContext) const
    {
        if (inSharedContext || NodeHasModifierToken(node, "shared", request.sourceCode))
        {
            return true;
        }

        TSNode nameNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName);
        std::string funcName = GetNodeText(nameNode, request.sourceCode);
        if (!funcName.empty())
        {
            auto syms = currentClassName.empty()
                            ? ctx.request.symbolTable.FindSymbolsPtr(funcName)
                            : ctx.request.symbolTable.FindMemberSymbolPtr(currentClassName, funcName);
            if (syms)
            {
                for (const auto& s : *syms)
                {
                    if (s.type == SymbolType::Function && s.fileUri == ctx.request.fileUri &&
                        s.GetFunction().modifiers.isShared)
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void VisitFunctionDeclaration(TSNode node, bool inSharedContext, int depth)
    {
        bool funcShared = IsFunctionShared(node, inSharedContext);
        ForEachChildNode(node, [&](TSNode child) { Visit(child, funcShared, depth + 1); });
    }

    void CheckSharedDatatype(TSNode node)
    {
        std::string typeName = CleanSharedTypeName(GetNodeText(node, request.sourceCode));
        if (IsCoreOrRegisteredType(typeName, ctx.request))
        {
            return;
        }

        auto syms = ctx.request.symbolTable.FindSymbolsPtr(typeName);
        if (!syms)
        {
            return;
        }

        for (const auto& s : *syms)
        {
            if (s.type != SymbolType::Class && s.type != SymbolType::Interface && s.type != SymbolType::Enum &&
                s.type != SymbolType::Funcdef)
            {
                continue;
            }

            if (!IsSymbolShared(s) && !IsFromPredefinedStub(s, ctx))
            {
                ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
                EmitAtNode(node, ctx, "as-err-shared-cannot-access-non-shared", typeName);
                break;
            }
        }
    }

    bool IsAllowedCallee(std::string_view calleeName) const
    {
        if (!currentClassName.empty())
        {
            if (ctx.request.symbolTable.FindMemberSymbolPtr(currentClassName, calleeName) != nullptr)
            {
                return true;
            }
        }
        return ctx.request.IsRegisteredSymbol(std::string(calleeName));
    }

    void CheckSharedCallExpression(TSNode node)
    {
        TSNode funcNode = parser::GetCallCallee(node);
        if (ts_node_is_null(funcNode))
        {
            return;
        }

        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol funcSym = ts_node_symbol(funcNode);
        if (funcSym != gs.symScopedIdentifier && funcSym != gs.symIdentifier)
        {
            return;
        }

        std::string calleeName = TrimWhitespace(GetNodeText(funcNode, request.sourceCode));
        if (calleeName.empty() || IsAllowedCallee(calleeName))
        {
            return;
        }

        if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(calleeName))
        {
            for (const auto& s : *syms)
            {
                if (s.type == SymbolType::Function && s.containerName.empty())
                {
                    if (!s.GetFunction().modifiers.isShared && !IsFromPredefinedStub(s, ctx))
                    {
                        ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
                        EmitAtNode(funcNode, ctx, "as-err-shared-cannot-access-non-shared", calleeName);
                        break;
                    }
                }
            }
        }
    }

    bool IsIdentifierInSpecialContext(TSNode node) const
    {
        TSNode p = parser::GetParent(node);
        const auto& gs = parser::GetGrammarSymbols();
        while (!ts_node_is_null(p))
        {
            const TSSymbol pSym = ts_node_symbol(p);
            if (pSym == gs.symCallExpression && IsCallFunctionNode(p, node))
            {
                return true;
            }
            if (pSym == gs.symDatatype || pSym == gs.symType || pSym == gs.symBaseClassList)
            {
                return true;
            }
            if (IsDeclarationNameNode(p, node, pSym))
            {
                return true;
            }
            if (pSym == gs.symStatementBlock || pSym == gs.symFuncDeclaration || pSym == gs.symClassDeclaration)
            {
                break;
            }
            p = parser::GetParent(p);
        }
        return false;
    }

    bool IsAllowedVariable(std::string_view varName, TSNode node) const
    {
        TSPoint pt = ts_node_start_point(node);
        const Scope* scope =
            request.scopeRoot ? FindEnclosingScopeOrRoot(request.scopeRoot, pt.row, pt.column) : nullptr;
        if (scope && IsLocalVariableOrParameter(scope, varName))
        {
            return true;
        }

        if (!currentClassName.empty())
        {
            if (ctx.request.symbolTable.FindMemberSymbolPtr(currentClassName, varName) != nullptr)
            {
                return true;
            }
        }

        return ctx.request.IsRegisteredSymbol(std::string(varName));
    }

    void CheckSharedIdentifier(TSNode node)
    {
        if (IsIdentifierInSpecialContext(node))
        {
            return;
        }

        std::string varName = TrimWhitespace(GetNodeText(node, request.sourceCode));
        if (varName.empty() || IsAllowedVariable(varName, node))
        {
            return;
        }

        if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(varName))
        {
            for (const auto& s : *syms)
            {
                if (s.type == SymbolType::Variable && s.containerName.empty())
                {
                    if (!s.GetVariable().modifiers.isShared && !IsFromPredefinedStub(s, ctx))
                    {
                        ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
                        EmitAtNode(node, ctx, "as-err-shared-cannot-access-non-shared", varName);
                        break;
                    }
                }
            }
        }
    }

    void Visit(TSNode node, bool inSharedContext, int depth = 0)
    {
        if (depth > k_maxAstDepth || ts_node_is_null(node))
        {
            return;
        }

        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol nodeSym = ts_node_symbol(node);

        if (nodeSym == gs.symClassDeclaration)
        {
            VisitClassDeclaration(node, depth);
            return;
        }

        if (nodeSym == gs.symFuncDeclaration)
        {
            VisitFunctionDeclaration(node, inSharedContext, depth);
            return;
        }

        if (inSharedContext)
        {
            if (nodeSym == gs.symDatatype)
            {
                CheckSharedDatatype(node);
            }
            else if (nodeSym == gs.symCallExpression)
            {
                CheckSharedCallExpression(node);
            }
            else if (nodeSym == gs.symIdentifier)
            {
                CheckSharedIdentifier(node);
            }
        }

        ForEachChildNode(node, [&](TSNode child) { Visit(child, inSharedContext, depth + 1); });
    }
};
} // namespace

void CheckSharedIsolation(const IsolationCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
    {
        return;
    }

    CheckSharedEntityEligibility(request.sourceCode, ctx);

    IsolationVisitor visitor{request, ctx, "", false};
    visitor.Visit(request.rootNode, false);
}
} // namespace angel_lsp::analysis
