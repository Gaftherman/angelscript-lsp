#include "analysis/LValueChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/**
 * @brief Node text as an owning string.
 *
 * Kept per translation unit rather than shared with ASTUtils::NodeText, which returns a
 * string_view. The two are not interchangeable: callers here store the result, concatenate
 * it, and use it after the node has gone out of scope, so handing them a view would trade a
 * duplicated three-line function for a lifetime question at several dozen call sites.
 * Deduplicating it was attempted and reverted for exactly that reason.
 */
std::string NodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return "";
    }

    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return "";
    }
    return std::string(sourceCode.substr(start, end - start));
}

void EmitAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange(start.row, start.column, end.row, end.column, code);
}

struct CallCandidateContext
{
    const LValueCheckRequest& request;
    const Scope* scope = nullptr;
    DiagnosticContext& ctx;
};

void CollectMemberCandidates(TSNode funcNode, const CallCandidateContext& cctx, std::vector<Symbol>& candidates)
{
    TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
    TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return;
    }
    std::string objType = ResolveExpressionType(objNode, cctx.scope, cctx.ctx.request.symbolTable,
                                                cctx.request.sourceCode, cctx.ctx.request.fileUri);
    std::string cleanObj = CleanBaseType(objType);
    std::string memName = NodeText(memNode, cctx.request.sourceCode);
    if (cleanObj.empty() || memName.empty())
    {
        return;
    }
    for (const auto& typeName : GetInheritedTypeHierarchy(cleanObj, cctx.ctx.request.symbolTable))
    {
        if (auto found = cctx.ctx.request.symbolTable.FindSymbolsPtr(typeName + "::" + memName))
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

void CollectScopedCandidates(TSNode funcNode, const CallCandidateContext& cctx, std::vector<Symbol>& candidates)
{
    std::string qName = NodeText(funcNode, cctx.request.sourceCode);
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

void CollectIdentifierCandidates(TSNode funcNode, const CallCandidateContext& cctx, std::vector<Symbol>& candidates)
{
    std::string name = NodeText(funcNode, cctx.request.sourceCode);
    for (const auto& sym : FindSymbolsInScope(name, funcNode, cctx.request.sourceCode, cctx.ctx.request.symbolTable))
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
    for (const auto& c : GetEnclosingContainers(funcNode, cctx.request.sourceCode))
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            auto hierarchy = GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName,
                                                       cctx.ctx.request.symbolTable);
            for (const auto& cls : hierarchy)
            {
                if (auto found = cctx.ctx.request.symbolTable.FindSymbolsPtr(cls + "::" + name))
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

void ValidateCandidateReturnTypes(TSNode callNode, const std::vector<Symbol>& candidates, DiagnosticContext& ctx)
{
    if (candidates.empty())
    {
        EmitAtNode(callNode, ctx, "as-err-assign-non-ref-call");
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

        if (fn.modifiers.isReturnReference || (!fn.returnType.empty() && fn.returnType.back() == '&'))
        {
            anyReference = true;
        }
    }

    if (allVoid)
    {
        EmitAtNode(callNode, ctx, "as-err-assign-void");
    }
    else if (!anyReference)
    {
        EmitAtNode(callNode, ctx, "as-err-assign-non-ref-call");
    }
}

void CheckCallLValue(TSNode callNode, const LValueCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode funcNode = parser::GetChildByField(callNode, parser::fields::Function);
    if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
    {
        funcNode = ts_node_child(callNode, 0);
    }
    if (ts_node_is_null(funcNode))
    {
        return;
    }

    const CallCandidateContext cctx{request, scope, ctx};
    std::vector<Symbol> candidates;
    const std::string_view funcNodeType = ts_node_type(funcNode);

    if (funcNodeType == "member_expression")
    {
        CollectMemberCandidates(funcNode, cctx, candidates);
    }
    else if (funcNodeType == "scoped_identifier")
    {
        CollectScopedCandidates(funcNode, cctx, candidates);
    }
    else if (funcNodeType == "identifier")
    {
        CollectIdentifierCandidates(funcNode, cctx, candidates);
    }

    ValidateCandidateReturnTypes(callNode, candidates, ctx);
}

TSNode UnwrapParentheses(TSNode node)
{
    TSNode current = node;
    while (!ts_node_is_null(current) && std::string_view(ts_node_type(current)) == "parenthesized_expression")
    {
        uint32_t count = ts_node_named_child_count(current);
        if (count > 0)
        {
            current = ts_node_named_child(current, 0);
            continue;
        }
        TSNode found{};
        for (uint32_t i = 0; i < ts_node_child_count(current); ++i)
        {
            TSNode ch = ts_node_child(current, i);
            std::string_view ct = ts_node_type(ch);
            if (ct != "(" && ct != ")")
            {
                found = ch;
                break;
            }
        }
        if (ts_node_is_null(found))
        {
            break;
        }
        current = found;
    }
    return current;
}

bool IsIdentifierAssignable(std::string_view name, const Scope* scope, const SymbolTable& table)
{
    if (scope)
    {
        const LocalDefinition* localDef = ResolveInScope(scope, name);
        if (localDef)
        {
            return localDef->kind != LocalDefinitionKind::Function && localDef->kind != LocalDefinitionKind::Method;
        }
    }

    auto symbols = table.FindSymbolsPtr(name);
    if (symbols && !symbols->empty())
    {
        for (const auto& sym : *symbols)
        {
            if (sym.type != SymbolType::Function && sym.type != SymbolType::Funcdef)
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

bool IsAssignableLValueNode(TSNode rawNode, const Scope* scope, const SymbolTable& table, std::string_view sourceCode);

bool IsTernaryAssignable(TSNode node, const Scope* scope, const SymbolTable& table, std::string_view sourceCode)
{
    TSNode consequence = parser::GetChildByField(node, parser::fields::Consequence);
    TSNode alternative = parser::GetChildByField(node, parser::fields::Alternative);
    if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
    {
        if (ts_node_named_child_count(node) >= 3)
        {
            consequence = ts_node_named_child(node, 1);
            alternative = ts_node_named_child(node, 2);
        }
    }
    return IsAssignableLValueNode(consequence, scope, table, sourceCode) &&
           IsAssignableLValueNode(alternative, scope, table, sourceCode);
}

bool IsUnaryAssignable(TSNode node, const Scope* scope, const SymbolTable& table, std::string_view sourceCode)
{
    TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
    if (!ts_node_is_null(opNode) && NodeText(opNode, sourceCode) == "@")
    {
        TSNode operand = parser::GetChildByField(node, parser::fields::Operand);
        return IsAssignableLValueNode(operand, scope, table, sourceCode);
    }
    return false;
}

bool IsAssignableLValueNode(TSNode rawNode, const Scope* scope, const SymbolTable& table, std::string_view sourceCode)
{
    if (ts_node_is_null(rawNode))
    {
        return false;
    }

    TSNode node = UnwrapParentheses(rawNode);
    if (ts_node_is_null(node))
    {
        return false;
    }

    const std::string_view nodeType = ts_node_type(node);
    if (nodeType == "ternary_expression")
    {
        return IsTernaryAssignable(node, scope, table, sourceCode);
    }
    if (nodeType == "unary_expression")
    {
        return IsUnaryAssignable(node, scope, table, sourceCode);
    }
    if (nodeType == "member_expression" || nodeType == "index_expression")
    {
        return true;
    }
    if (nodeType == "identifier" || nodeType == "scoped_identifier")
    {
        return IsIdentifierAssignable(NodeText(node, sourceCode), scope, table);
    }
    return false;
}

void CheckAssignmentTarget(TSNode node, const LValueCheckRequest& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode target = parser::GetChildByField(node, parser::fields::Left);
    if (ts_node_is_null(target))
    {
        return;
    }

    const std::string_view targetType = ts_node_type(target);

    if (targetType == "call_expression")
    {
        CheckCallLValue(target, request, scope, ctx);
        return;
    }

    TSNode unwrapped = UnwrapParentheses(target);
    if (std::string_view(ts_node_type(unwrapped)) == "call_expression")
    {
        CheckCallLValue(unwrapped, request, scope, ctx);
        return;
    }

    if (targetType == "ternary_expression")
    {
        return;
    }

    if (!IsAssignableLValueNode(target, scope, ctx.request.symbolTable, request.sourceCode))
    {
        EmitAtNode(target, ctx, "as-err-not-lvalue");
    }
}

void VisitNode(TSNode node, const LValueCheckRequest& request, DiagnosticContext& ctx, int depth = 0)
{
    // Pathologically nested source would otherwise recurse until the stack gives out; see
    // k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth)
        return;

    std::string_view nodeType = ts_node_type(node);
    if (nodeType == "assignment_expression")
    {
        const TSPoint start = ts_node_start_point(node);
        const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
        CheckAssignmentTarget(node, request, scope, ctx);
    }

    uint32_t childCount = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        VisitNode(ts_node_named_child(node, i), request, ctx, depth + 1);
    }
}
} // namespace

void CheckLValues(const LValueCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        for (const TSNode& node : request.nodeIndex->Nodes(parser::nodes::AssignmentExpression))
        {
            const TSPoint start = ts_node_start_point(node);
            const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
            CheckAssignmentTarget(node, request, scope, ctx);
        }
        return;
    }

    VisitNode(request.root, request, ctx);
}
} // namespace angel_lsp::analysis
