#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/IsolationSiteFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <string_view>

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

static bool IsAllowedVariable(std::string_view varName, const IsolationIdentifierSiteFact& fact, const Scope* scopeRoot,
                              const DiagnosticContext& ctx)
{
    const Scope* scope =
        scopeRoot ? FindEnclosingScopeOrRoot(scopeRoot, fact.range.startLine, fact.range.startCharacter) : nullptr;
    if (scope && IsLocalVariableOrParameter(scope, varName))
    {
        return true;
    }

    if (!fact.enclosingClass.empty())
    {
        if (ctx.request.symbolTable.FindMemberSymbolPtr(fact.enclosingClass, varName) != nullptr)
        {
            return true;
        }
    }

    return ctx.request.IsRegisteredSymbol(varName);
}

void CheckSharedIdentifier(const IsolationIdentifierSiteFact& fact, const Scope* scopeRoot, DiagnosticContext& ctx)
{
    if (!fact.inSharedContext || fact.isSpecialContext)
    {
        return;
    }

    const std::string_view varName = fact.name;
    if (varName.empty() || IsAllowedVariable(varName, fact, scopeRoot, ctx))
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
                    ctx.EmitAtRange(fact.range, "as-err-shared-cannot-access-non-shared", varName);
                    break;
                }
            }
        }
    }
}

} // namespace angel_lsp::analysis
