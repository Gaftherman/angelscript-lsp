#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Innermost containing scope, falling back to `root` rather than nullptr.
 *        Named apart from analysis::FindInnermostScope on purpose: that one reports "no scope"
 */
static const Scope* FindEnclosingScopeOrRoot(const Scope* root, uint32_t line, uint32_t character)
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

static bool IsLocalVariableOrParameter(const Scope* startScope, std::string_view name)
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

static bool IsCallFunctionNode(TSNode p, TSNode node)
{
    TSNode fn = parser::GetCallCallee(p);
    return !ts_node_is_null(fn) && (fn.id == node.id || parser::GetParent(node).id == fn.id);
}

static bool IsDeclarationNameNode(TSNode p, TSNode node, TSSymbol pSym)
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

bool IsolationVisitor::IsIdentifierInSpecialContext(TSNode node) const
{
    const auto& gs = parser::GetGrammarSymbols();
    bool result = false;
    parser::ForEachAncestor(parser::GetParent(node),
                            [&](TSNode p) -> bool
                            {
                                const TSSymbol pSym = ts_node_symbol(p);
                                if (pSym == gs.symCallExpression && IsCallFunctionNode(p, node))
                                {
                                    result = true;
                                    return false;
                                }
                                if (pSym == gs.symDatatype || pSym == gs.symType || pSym == gs.symBaseClassList)
                                {
                                    result = true;
                                    return false;
                                }
                                if (IsDeclarationNameNode(p, node, pSym))
                                {
                                    result = true;
                                    return false;
                                }
                                return pSym != gs.symStatementBlock && pSym != gs.symFuncDeclaration &&
                                       pSym != gs.symClassDeclaration;
                            });
    return result;
}

bool IsolationVisitor::IsAllowedVariable(std::string_view varName, TSNode node) const
{
    TSPoint pt = ts_node_start_point(node);
    const Scope* scope = request.scopeRoot ? FindEnclosingScopeOrRoot(request.scopeRoot, pt.row, pt.column) : nullptr;
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

void IsolationVisitor::CheckSharedIdentifier(TSNode node)
{
    if (IsIdentifierInSpecialContext(node))
    {
        return;
    }

    std::string varName = TrimWhitespace(std::string(parser::NodeText(node, request.sourceCode)));
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
                    ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-shared-cannot-access-non-shared", varName);
                    break;
                }
            }
        }
    }
}

} // namespace angel_lsp::analysis
