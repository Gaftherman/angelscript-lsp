#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

std::string TrimWhitespace(std::string_view text)
{
    return std::string(utils::TrimView(text));
}

/**
 * @brief Normalizes a shared type name by trimming whitespace and trailing decorations.
 * @param[in] raw Raw type name string view.
 * @return Cleaned type name without trailing references or handle markers.
 */
static std::string CleanSharedTypeName(std::string_view raw)
{
    std::string_view sv = raw;
    TrimTypeWhitespace(sv);
    sv = StripTrailingDecorationsView(sv);
    return std::string(sv);
}

static bool IsSymbolShared(const Symbol& s)
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

static bool IsCoreOrRegisteredType(std::string_view typeName, const SemanticAnalysisRequest& request)
{
    return typeName.empty() || IsCorePrimitive(typeName) || typeName == request.GetStringTypeName() ||
           typeName == request.GetArrayTypeName() || request.IsRegisteredSymbol(std::string(typeName));
}

/**
 * @brief Checks whether a node has a direct child token with the given modifier symbol.
 * @param[in] node Declaration syntax node; a null node yields false.
 * @param[in] targetSym Modifier token symbol to look for.
 * @return True when a direct child of node carries targetSym.
 */
static bool NodeHasModifierToken(TSNode node, TSSymbol targetSym)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return false;
    }
    bool found = false;
    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == targetSym)
        {
            found = true;
            break;
        }
        if (sym == gs.symDeclarationModifier || sym == gs.symSharedExternalModifier)
        {
            if (!ts_node_is_null(parser::FindChildWithSymbol(child, targetSym)))
            {
                found = true;
                break;
            }
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);
    return found;
}

bool IsolationVisitor::IsClassShared(TSNode node, const std::string& className) const
{
    if (NodeHasModifierToken(node, parser::GetGrammarSymbols().tokShared))
    {
        return true;
    }
    if (!className.empty())
    {
        if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(className))
        {
            for (const auto& s : *syms)
            {
                if (s.type == SymbolType::Class && s.fileUri == ctx.request.fileUri && s.GetClass().modifiers.isShared)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool IsolationVisitor::IsFunctionShared(TSNode node, bool inSharedContext) const
{
    if (inSharedContext || NodeHasModifierToken(node, parser::GetGrammarSymbols().tokShared))
    {
        return true;
    }

    TSNode nameNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName);
    std::string funcName(parser::NodeText(nameNode, request.sourceCode));
    if (!funcName.empty())
    {
        auto syms = currentClassName.empty() ? ctx.request.symbolTable.FindSymbolsPtr(funcName)
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

void IsolationVisitor::CheckSharedDatatype(TSNode node)
{
    std::string typeName = CleanSharedTypeName(std::string(parser::NodeText(node, request.sourceCode)));
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
            ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-shared-cannot-access-non-shared", typeName);
            break;
        }
    }
}

bool IsolationVisitor::IsAllowedCallee(std::string_view calleeName) const
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

void IsolationVisitor::CheckSharedCallExpression(TSNode node)
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

    std::string calleeName = TrimWhitespace(std::string(parser::NodeText(funcNode, request.sourceCode)));
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
                    ctx.EmitAtNodeRange(ToOptionalSourceRange(funcNode), "as-err-shared-cannot-access-non-shared",
                                        calleeName);
                    break;
                }
            }
        }
    }
}

} // namespace angel_lsp::analysis
