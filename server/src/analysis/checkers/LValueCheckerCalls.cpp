#include "analysis/checkers/LValueChecker.h"
#include "analysis/checkers/LValueCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Collects candidate method symbols for a member expression callee.
 * @param[in] funcNode Member expression AST node representing the callee.
 * @param[in] cctx Call candidate context holding request, scope, and diagnostic context.
 * @param[out] candidates Vector of candidate symbols to append to.
 */
static void CollectMemberCandidates(TSNode funcNode, const CallCandidateContext& cctx, std::vector<Symbol>& candidates)
{
    TSNode objNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return;
    }
    std::string objType = ResolveExpressionType(objNode, ExpressionTypeContext(cctx.scope, cctx.ctx));
    std::string cleanObj = CleanBaseType(objType);
    std::string memName(parser::NodeText(memNode, cctx.request.sourceCode));
    if (cleanObj.empty() || memName.empty())
    {
        return;
    }
    for (const auto& typeName : GetInheritedTypeHierarchy(cleanObj, cctx.ctx.request.symbolTable))
    {
        if (auto found = cctx.ctx.request.symbolTable.FindMemberSymbolPtr(typeName, memName))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function)
                {
                    candidates.push_back(sym);
                }
            }
        }
    }
}

/**
 * @brief Collects candidate symbols for a scoped identifier callee.
 * @param[in] funcNode Scoped identifier AST node representing the callee.
 * @param[in] cctx Call candidate context holding request, scope, and diagnostic context.
 * @param[out] candidates Vector of candidate symbols to append to.
 */
static void CollectScopedCandidates(TSNode funcNode, const CallCandidateContext& cctx, std::vector<Symbol>& candidates)
{
    std::string qName(parser::NodeText(funcNode, cctx.request.sourceCode));
    if (auto found = cctx.ctx.request.symbolTable.FindSymbolsPtr(qName))
    {
        for (const auto& sym : *found)
        {
            if (sym.type == SymbolType::Function)
            {
                candidates.push_back(sym);
            }
        }
    }
}

/**
 * @brief Collects candidate symbols for an unqualified identifier callee.
 * @param[in] funcNode Identifier AST node representing the callee.
 * @param[in] cctx Call candidate context holding request, scope, and diagnostic context.
 * @param[out] candidates Vector of candidate symbols to append to.
 */
static void CollectIdentifierCandidates(TSNode funcNode, const CallCandidateContext& cctx,
                                        std::vector<Symbol>& candidates)
{
    std::string name(parser::NodeText(funcNode, cctx.request.sourceCode));
    for (const auto& sym : FindSymbolsInScope(name, funcNode, cctx.request.sourceCode, cctx.ctx.request.symbolTable,
                                              cctx.request.nodeIndex))
    {
        if (sym.type == SymbolType::Function)
        {
            candidates.push_back(sym);
        }
    }
    if (!candidates.empty())
    {
        return;
    }
    for (const auto& c : GetEnclosingContainers(funcNode, cctx.request.sourceCode, cctx.request.nodeIndex))
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            auto hierarchy = GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName,
                                                       cctx.ctx.request.symbolTable);
            for (const auto& cls : hierarchy)
            {
                if (auto found = cctx.ctx.request.symbolTable.FindMemberSymbolPtr(cls, name))
                {
                    for (const auto& sym : *found)
                    {
                        if (sym.type == SymbolType::Function)
                        {
                            candidates.push_back(sym);
                        }
                    }
                }
            }
            break;
        }
    }
}

/**
 * @brief Validate candidate return types.
 * @param[in] callNode Call expression AST node.
 * @param[in] candidates Candidate symbols or overloads.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void ValidateCandidateReturnTypes(TSNode callNode, const std::vector<Symbol>& candidates, DiagnosticContext& ctx)
{
    if (candidates.empty())
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(callNode), "as-err-assign-non-ref-call");
        return;
    }

    bool allVoid = true;
    bool anyReference = false;

    for (const auto& sym : candidates)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }

        const auto& fn = sym.GetFunction();
        std::string retClean = CleanBaseType(fn.returnType);
        if (fn.returnTypeKind != TypeKind::Void && retClean != "void")
        {
            allVoid = false;
        }

        if (fn.modifiers.isReturnReference || IsReferenceTypeView(fn.returnType))
        {
            anyReference = true;
        }
    }

    if (allVoid)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(callNode), "as-err-assign-void");
    }
    else if (!anyReference)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(callNode), "as-err-assign-non-ref-call");
    }
}

/**
 * @brief Validates whether a call expression target is a modifiable reference l-value.
 * @param[in] callNode Call expression AST node.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in] scope Lexical scope active at the call site.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckCallLValue(TSNode callNode, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode funcNode = parser::GetCallCallee(callNode);
    if (ts_node_is_null(funcNode))
    {
        return;
    }

    const CallCandidateContext cctx{request, scope, ctx};
    std::vector<Symbol> candidates;
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol funcSym = ts_node_symbol(funcNode);

    if (funcSym == gs.symMemberExpression)
    {
        CollectMemberCandidates(funcNode, cctx, candidates);
    }
    else if (funcSym == gs.symScopedIdentifier)
    {
        CollectScopedCandidates(funcNode, cctx, candidates);
    }
    else if (funcSym == gs.symIdentifier || funcSym == gs.tokFunction)
    {
        CollectIdentifierCandidates(funcNode, cctx, candidates);
    }

    ValidateCandidateReturnTypes(callNode, candidates, ctx);
}

} // namespace angel_lsp::analysis
