/**
 * @file CodeActionConstQualifier.cpp
 * @brief Code action providers for adding const qualifier to methods.
 */

#include "analysis/types/TypeSanitization.h"
#include "features/code_action/CodeActionClassMutation.h"
#include "features/code_action/CodeActionInternal.h"
#include "parser/ASTUtils.h"

namespace angel_lsp::features
{

/**
 * @brief Finds the statement block body of a function node.
 * @param[in] fnNode Function declaration node.
 * @return Statement block node or null node.
 */
static TSNode FindFunctionBodyBlock(TSNode fnNode)
{
    TSNode bodyNode = parser::GetChildByField(fnNode, parser::GetGrammarSymbols().fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        return bodyNode;
    }
    return parser::FindFirstChildOfType(fnNode, "statement_block");
}

/**
 * @brief Finds the parameter list node of a function declaration.
 * @param[in] fnNode Function declaration node.
 * @return Parameter list node or null node.
 */
static TSNode FindFunctionParametersNode(TSNode fnNode)
{
    TSNode paramList = parser::GetChildByField(fnNode, parser::GetGrammarSymbols().fieldParameters);
    if (!ts_node_is_null(paramList))
    {
        return paramList;
    }
    return parser::FindFirstChildOfType(fnNode, "parameter_list");
}

/**
 * @brief Attempts to construct a QuickFix CodeAction to add a const qualifier to a referenced method declaration.
 * @param[in] request Code action request bundle containing AST root, source text, and parameters.
 * @param[in] rootNode Root AST node of the document.
 * @param[in] diag LSP diagnostic reporting the missing const method call error.
 * @param[in] sym Target method symbol needing const qualification.
 * @return QuickFix CodeAction if method declaration node is located, or std::nullopt otherwise.
 */
static std::optional<lsp::CodeAction> TryBuildMissingConstFixForSymbol(const CodeActionRequest& request,
                                                                       TSNode rootNode, const lsp::Diagnostic& diag,
                                                                       const analysis::Symbol& sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSPoint fnPt = {sym.startLine, sym.startCharacter};
    TSNode fnNode =
        parser::FindAncestorWithSymbol(ts_node_descendant_for_point_range(rootNode, fnPt, fnPt), gs.symFuncDeclaration);
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
    action.title = i18n::FormatMessage(request.i18n, "action-add-const-qualifier", "Add 'const' qualifier to method");
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

static void TryAddMissingConstDiagnosticFix(const CodeActionRequest& request, TSNode rootNode,
                                            const lsp::Diagnostic& diag, std::vector<lsp::CodeAction>& actions)
{
    TSPoint dPt = {diag.range.start.line, diag.range.start.character};
    TSNode memberNode = ts_node_descendant_for_point_range(rootNode, dPt, dPt);
    std::string methodName = parser::GetNodeText(memberNode, request.sourceCode);

    TSNode callee = parser::GetParent(memberNode);
    TSNode objNode = parser::GetChildByField(callee, parser::GetGrammarSymbols().fieldObject);
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), dPt.row);
    std::string objType = analysis::CleanBaseType(
        analysis::ResolveExpressionType(objNode, {scope, request.symbolTable, request.sourceCode, request.uri}));

    request.symbolTable.ForEachSymbolInFile(
        request.uri,
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

/**
 * @brief Checks whether the 'const' keyword appears in source text between the parameter list and function body.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] paramList AST node of the function parameter list.
 * @param[in] bodyNode AST node of the function compound body (or null if abstract).
 * @param[in] fnNode AST node of the enclosing function declaration.
 * @return True if 'const' token is found in the intervening source span, false otherwise.
 */
static bool HasConstQualifierBetween(TSNode fnNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode attr = parser::FindChildWithSymbol(fnNode, gs.symFuncAttributes);
    if (!ts_node_is_null(attr) && !ts_node_is_null(parser::FindChildWithSymbol(attr, gs.tokConst)))
    {
        return true;
    }
    return !ts_node_is_null(parser::FindChildWithSymbol(fnNode, gs.tokConst));
}

static bool FindEnclosingMethodAndClass(TSNode leaf, TSNode& outFn, TSNode& outClass, const parser::GrammarSymbols& gs)
{
    TSNode fnNode = parser::FindAncestorWithSymbol(leaf, gs.symFuncDeclaration);
    TSNode classNode = parser::FindAncestorWithSymbol(fnNode, gs.symClassDeclaration);
    if (ts_node_is_null(fnNode) || ts_node_is_null(classNode))
    {
        return false;
    }
    outFn = fnNode;
    outClass = classNode;
    return true;
}

/**
 * @brief Constructs a QuickFix CodeAction to append the 'const' qualifier to a method parameter list.
 * @param[in] paramList AST node of the method parameter list after which 'const' is inserted.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @return QuickFix CodeAction with text edit inserting ' const'.
 */
static lsp::CodeAction BuildAddConstAction(TSNode paramList, const CodeActionRequest& request)
{
    TSPoint insertPt = ts_node_end_point(paramList);
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
    edit.newText = " const";

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-add-const-qualifier", "Add 'const' qualifier to method");
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    return action;
}

static void TryAddIntentionalConstAction(const CodeActionRequest& request, TSNode rootNode,
                                         std::vector<lsp::CodeAction>& actions)
{
    TSPoint pt = {request.range.start.line, request.range.start.character};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode fnNode = {};
    TSNode classNode = {};
    if (!FindEnclosingMethodAndClass(leaf, fnNode, classNode, gs))
    {
        return;
    }

    TSNode paramList = FindFunctionParametersNode(fnNode);
    TSNode bodyNode = FindFunctionBodyBlock(fnNode);
    if (ts_node_is_null(paramList) || ts_node_is_null(bodyNode))
    {
        return;
    }
    if (HasConstQualifierBetween(fnNode))
    {
        return;
    }

    TSNode classNameNode = parser::GetChildByField(classNode, parser::GetGrammarSymbols().fieldName);
    std::string className = parser::GetNodeText(classNameNode, request.sourceCode);

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), ts_node_start_point(fnNode).row);

    ClassMutationContext mutCtx{bodyNode, classNode, request.sourceCode, request.symbolTable, className, scope};
    if (!MethodBodyMutatesClassState(mutCtx))
    {
        actions.push_back(BuildAddConstAction(paramList, request));
    }
}

/**
 * @brief Try add const handle diagnostic fix.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in] rootNode AST node under evaluation.
 * @param[in] diag LSP Diagnostic triggering the const qualifier action.
 * @param[in,out] actions Output list where constructed CodeAction is appended.
 */
static void TryAddConstHandleDiagnosticFix(const CodeActionRequest& request, TSNode rootNode,
                                           const lsp::Diagnostic& diag, std::vector<lsp::CodeAction>& actions)
{
    const TSPoint dPt = {diag.range.start.line, diag.range.start.character};
    TSNode node = ts_node_descendant_for_point_range(rootNode, dPt, dPt);
    if (ts_node_is_null(node))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode declNode = parser::FindAncestorWithSymbol(node, gs.symVariableDeclaration);
    if (ts_node_is_null(declNode))
    {
        return;
    }

    TSNode typeNode = parser::GetChildByField(declNode, gs.fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::FindChildWithSymbol(declNode, gs.symType);
    }
    if (ts_node_is_null(typeNode))
    {
        return;
    }

    const std::string typeText = parser::GetNodeText(typeNode, request.sourceCode);
    if (!analysis::IsHandleTypeView(typeText) || analysis::IsConstTypeView(typeText))
    {
        return;
    }

    const TSPoint insertPt = ts_node_start_point(typeNode);
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
    edit.newText = "const ";

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-add-const-variable-qualifier", "Add 'const' qualifier");
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = {diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    actions.push_back(std::move(action));
}

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
        else if (MatchDiagnosticCode(diag, "as-err-no-implicit-conversion"))
        {
            TryAddConstHandleDiagnosticFix(request, rootNode, diag, actions);
        }
    }
    TryAddIntentionalConstAction(request, rootNode, actions);
}

} // namespace angel_lsp::features
