#include "analysis/LValueChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
void EmitAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code, std::string_view arg = {})
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    if (arg.empty())
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code);
    }
    else
    {
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code, arg);
    }
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
    std::string objType = ResolveExpressionType(objNode, ExpressionTypeContext(cctx.scope, cctx.ctx));
    std::string cleanObj = CleanBaseType(objType);
    std::string memName = NodeText(memNode, cctx.request.sourceCode);
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

TSNode UnwrapParentheses(TSNode node)
{
    TSNode current = node;
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(current) && ts_node_symbol(current) == gs.symParenthesizedExpression)
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
            if (ts_node_is_named(ch))
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

bool IsIdentifierAssignable(std::string_view name, const Scope* scope, const SymbolTable& table,
                            uint32_t refByte = UINT32_MAX)
{
    if (IsPrimitiveTypeName(name))
    {
        return false;
    }

    if (scope)
    {
        const LocalDefinition* localDef = ResolveInScopeAt(scope, name, refByte);
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
            if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

bool IsAssignableMember(TSNode node, const Scope* scope, const SymbolTable& table, std::string_view sourceCode)
{
    TSNode objNode = parser::GetChildByField(node, parser::fields::Object);
    TSNode memNode = parser::GetChildByField(node, parser::fields::Member);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return true;
    }
    std::string memName = NodeText(memNode, sourceCode);
    if (memName.empty())
    {
        return true;
    }
    std::string objType = ResolveExpressionType(objNode, ExpressionTypeContext(scope, table, sourceCode));
    std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty())
    {
        return true;
    }
    for (const auto& typeName : GetInheritedTypeHierarchy(cleanObj, table))
    {
        if (auto found = table.FindMemberSymbolPtr(typeName, memName))
        {
            bool hasAssignable = false;
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property)
                {
                    hasAssignable = true;
                    break;
                }
            }
            if (!hasAssignable)
            {
                bool hasSetter = false;
                for (const auto& ancestor : GetInheritedTypeHierarchy(cleanObj, table))
                {
                    auto setters = table.FindSymbolsPtr(ancestor + "::set_" + memName);
                    if (setters && !setters->empty())
                    {
                        hasSetter = true;
                        break;
                    }
                }
                if (!hasSetter)
                {
                    return false;
                }
            }
        }
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
    if (!IsAssignableLValueNode(consequence, scope, table, sourceCode) ||
        !IsAssignableLValueNode(alternative, scope, table, sourceCode))
    {
        return false;
    }
    const std::string type1 = ResolveExpressionType(consequence, {scope, table, sourceCode});
    const std::string type2 = ResolveExpressionType(alternative, {scope, table, sourceCode});
    if (!type1.empty() && !type2.empty() && CleanBaseType(type1) != CleanBaseType(type2))
    {
        return false;
    }
    return true;
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

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symTernaryExpression)
    {
        return IsTernaryAssignable(node, scope, table, sourceCode);
    }
    if (sym == gs.symUnaryExpression)
    {
        return IsUnaryAssignable(node, scope, table, sourceCode);
    }
    if (sym == gs.symMemberExpression)
    {
        return IsAssignableMember(node, scope, table, sourceCode);
    }
    if (sym == gs.symIndexExpression)
    {
        return true;
    }
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier || sym == gs.tokFunction)
    {
        return IsIdentifierAssignable(NodeText(node, sourceCode), scope, table, ts_node_start_byte(node));
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

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targetSym = ts_node_symbol(target);

    if (targetSym == gs.symCallExpression)
    {
        CheckCallLValue(target, request, scope, ctx);
        return;
    }

    TSNode unwrapped = UnwrapParentheses(target);
    if (ts_node_symbol(unwrapped) == gs.symCallExpression)
    {
        CheckCallLValue(unwrapped, request, scope, ctx);
        return;
    }

    if (targetSym == gs.symTernaryExpression)
    {
        return;
    }

    if (!IsAssignableLValueNode(target, scope, ctx.request.symbolTable, request.sourceCode))
    {
        if (const auto dt = IsBareDataType(target, scope, ctx.request.symbolTable, request.sourceCode))
        {
            EmitAtNode(target, ctx, diagnostics::codes::ExpressionIsDataType, *dt);
        }
        else
        {
            EmitAtNode(target, ctx, "as-err-not-lvalue");
        }
    }
}

void CheckLValuesIndexed(const LValueCheckRequest& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    for (const TSNode& node : request.nodeIndex->Nodes(parser::nodes::AssignmentExpression))
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }
        const TSPoint start = ts_node_start_point(node);
        const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
        CheckAssignmentTarget(node, request, scope, ctx);
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
        CheckLValuesIndexed(request, ctx);
        return;
    }

    NodeIndex localIndex(request.root);
    LValueCheckRequest indexedRequest = request;
    indexedRequest.nodeIndex = &localIndex;
    CheckLValuesIndexed(indexedRequest, ctx);
}
} // namespace angel_lsp::analysis
