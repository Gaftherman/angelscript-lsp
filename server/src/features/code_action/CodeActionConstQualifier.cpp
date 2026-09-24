/**
 * @file CodeActionConstQualifier.cpp
 * @brief Code action providers for adding const qualifier to methods.
 */

#include "features/code_action/CodeActionClassMutation.h"
#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Finds the statement block body of a function node.
 * @param[in] fnNode Function declaration node.
 * @return Statement block node or null node.
 */
TSNode FindFunctionBodyBlock(TSNode fnNode)
{
    TSNode bodyNode = parser::GetChildByField(fnNode, parser::fields::Body);
    if (!ts_node_is_null(bodyNode))
    {
        return bodyNode;
    }
    uint32_t cnt = ts_node_child_count(fnNode);
    for (uint32_t i = 0; i < cnt; ++i)
    {
        TSNode ch = ts_node_child(fnNode, i);
        if (std::string_view(ts_node_type(ch)) == "statement_block")
        {
            return ch;
        }
    }
    return bodyNode;
}

/**
 * @brief Finds the parameter list node of a function declaration.
 * @param[in] fnNode Function declaration node.
 * @return Parameter list node or null node.
 */
TSNode FindFunctionParametersNode(TSNode fnNode)
{
    TSNode paramList = parser::GetChildByField(fnNode, parser::fields::Parameters);
    if (!ts_node_is_null(paramList))
    {
        return paramList;
    }
    uint32_t cnt = ts_node_child_count(fnNode);
    for (uint32_t c = 0; c < cnt; ++c)
    {
        TSNode ch = ts_node_child(fnNode, c);
        if (std::string_view(ts_node_type(ch)) == "parameter_list")
        {
            return ch;
        }
    }
    return paramList;
}

std::optional<lsp::CodeAction> TryBuildMissingConstFixForSymbol(const CodeActionRequest& request, TSNode rootNode,
                                                                const lsp::Diagnostic& diag,
                                                                const analysis::Symbol& sym)
{
    TSPoint fnPt = {sym.startLine, sym.startCharacter};
    TSNode fnNode = ts_node_descendant_for_point_range(rootNode, fnPt, fnPt);
    while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
    {
        fnNode = ts_node_parent(fnNode);
    }
    if (ts_node_is_null(fnNode))
    {
        return std::nullopt;
    }
    TSNode paramList = FindFunctionParametersNode(fnNode);
    if (ts_node_is_null(paramList))
    {
        return std::nullopt;
    }

    TSPoint insertPt = ts_node_end_point(paramList);
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
    edit.newText = " const";

    lsp::CodeAction action;
    action.title = "Add 'const' qualifier to method";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = {diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    return action;
}

void TryAddMissingConstDiagnosticFix(const CodeActionRequest& request, TSNode rootNode, const lsp::Diagnostic& diag,
                                     std::vector<lsp::CodeAction>& actions)
{
    TSPoint dPt = {diag.range.start.line, diag.range.start.character};
    TSNode memberNode = ts_node_descendant_for_point_range(rootNode, dPt, dPt);
    std::string methodName = GetNodeText(memberNode, request.sourceCode);

    TSNode callee = ts_node_parent(memberNode);
    TSNode objNode = parser::GetChildByField(callee, parser::fields::Object);
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), dPt.row);
    std::string objType = analysis::CleanBaseType(
        analysis::ResolveExpressionType(objNode, {scope, request.symbolTable, request.sourceCode, request.uri}));

    request.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (sym.type == analysis::SymbolType::Function && sym.name == methodName &&
                    (objType.empty() || sym.containerName == objType) && sym.fileUri == request.uri)
                {
                    if (auto action = TryBuildMissingConstFixForSymbol(request, rootNode, diag, sym))
                    {
                        actions.push_back(std::move(*action));
                    }
                }
            }
        });
}

bool HasConstQualifierBetween(std::string_view sourceCode, TSNode paramList, TSNode bodyNode, TSNode fnNode)
{
    uint32_t pEndByte = ts_node_end_byte(paramList);
    uint32_t bStartByte = !ts_node_is_null(bodyNode) ? ts_node_start_byte(bodyNode) : ts_node_end_byte(fnNode);
    if (pEndByte < bStartByte && bStartByte <= sourceCode.size())
    {
        std::string between = std::string(sourceCode.substr(pEndByte, bStartByte - pEndByte));
        return between.find("const") != std::string::npos;
    }
    return false;
}

void TryAddIntentionalConstAction(const CodeActionRequest& request, TSNode rootNode,
                                  std::vector<lsp::CodeAction>& actions)
{
    TSPoint pt = {request.range.start.line, request.range.start.character};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return;
    }

    TSNode fnNode = leaf;
    while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
    {
        fnNode = ts_node_parent(fnNode);
    }
    TSNode classNode = fnNode;
    while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
    {
        classNode = ts_node_parent(classNode);
    }
    if (ts_node_is_null(fnNode) || ts_node_is_null(classNode))
    {
        return;
    }

    TSNode paramList = FindFunctionParametersNode(fnNode);
    TSNode bodyNode = FindFunctionBodyBlock(fnNode);
    if (ts_node_is_null(paramList) || ts_node_is_null(bodyNode))
    {
        return;
    }
    if (HasConstQualifierBetween(request.sourceCode, paramList, bodyNode, fnNode))
    {
        return;
    }

    TSNode classNameNode = parser::GetChildByField(classNode, parser::fields::Name);
    std::string className = GetNodeText(classNameNode, request.sourceCode);

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope =
        FindScopeByLineOrRoot(rootScope.get(), ts_node_start_point(fnNode).row);

    ClassMutationContext mutCtx{bodyNode, classNode, request.sourceCode, request.symbolTable, className, scope};
    if (!MethodBodyMutatesClassState(mutCtx))
    {
        TSPoint insertPt = ts_node_end_point(paramList);
        lsp::TextEdit edit;
        edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
        edit.newText = " const";

        lsp::CodeAction action;
        action.title = "Add 'const' qualifier to method";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

} // namespace

void TryAddConstQualifierActions(const CodeActionRequest& request, TSNode rootNode,
                                 std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }
    for (const auto& diag : request.context.diagnostics)
    {
        if (MatchDiagnosticCode(diag, "as-err-const-method-required"))
        {
            TryAddMissingConstDiagnosticFix(request, rootNode, diag, actions);
        }
    }
    TryAddIntentionalConstAction(request, rootNode, actions);
}

} // namespace angel_lsp::features
