/**
 * @file CodeActionQuickFixes.cpp
 * @brief Miscellaneous quick-fix code action providers (primitive handles, funcdefs, bool conversions).
 */

#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Locates a unique global function symbol by name.
 * @param[in] table Symbol table.
 * @param[in] functionName Name of global function.
 * @return Unique function symbol pointer or nullptr if ambiguous/missing.
 */
const analysis::Symbol* FindUniqueGlobalFunction(const analysis::SymbolTable& table, const std::string& functionName)
{
    const analysis::Symbol* target = nullptr;
    size_t globalOverloads = 0;
    table.ForEachSymbol(
        [&](const std::string& name, const std::vector<analysis::Symbol>& symbols)
        {
            if (name != functionName)
            {
                return;
            }
            for (const auto& sym : symbols)
            {
                if (sym.type == analysis::SymbolType::Function && sym.containerName.empty() &&
                    std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                {
                    ++globalOverloads;
                    target = &sym;
                }
            }
        });
    return (target != nullptr && globalOverloads == 1) ? target : nullptr;
}

/**
 * @brief Formats a `funcdef` declaration string matching a function signature.
 * @param[in] signature Function signature.
 * @param[in] funcdefName Target funcdef type name.
 * @return Formatted funcdef declaration line.
 */
std::string FormatFuncdefDeclarationText(const analysis::FunctionSignature& signature, const std::string& funcdefName)
{
    std::string declaration = "funcdef ";
    declaration += signature.returnType.empty() ? "void" : signature.returnType;
    declaration += " " + funcdefName + "(";
    for (size_t i = 0; i < signature.parameters.size(); ++i)
    {
        if (i > 0)
        {
            declaration += ", ";
        }
        const auto& parameter = signature.parameters[i];
        declaration += parameter.rawText.empty() ? parameter.typeName : parameter.rawText;
    }
    declaration += ");\n";
    return declaration;
}

/**
 * @brief Item information for generating a funcdef quick-fix action.
 */
struct FuncdefFixItem
{
    TSNode typeNode;
    std::string functionName;
    std::string funcdefName;
    std::string declaration;
};

/**
 * @brief Emits a quick fix that prepends a funcdef declaration and renames the type reference.
 * @param[in] request Code action request.
 * @param[in] diag Triggering diagnostic.
 * @param[in] item Funcdef fix metadata item.
 * @param[out] actions Destination actions vector.
 */
void EmitFuncdefCodeAction(const CodeActionRequest& request, const lsp::Diagnostic& diag, const FuncdefFixItem& item,
                           std::vector<lsp::CodeAction>& actions)
{
    std::vector<lsp::TextEdit> edits;
    lsp::TextEdit insertion;
    insertion.range.start.line = 0;
    insertion.range.start.character = 0;
    insertion.range.end.line = 0;
    insertion.range.end.character = 0;
    insertion.newText = item.declaration;
    edits.push_back(std::move(insertion));

    lsp::TextEdit rename;
    rename.range.start.line = ts_node_start_point(item.typeNode).row;
    rename.range.start.character = ts_node_start_point(item.typeNode).column;
    rename.range.end.line = ts_node_end_point(item.typeNode).row;
    rename.range.end.character = ts_node_end_point(item.typeNode).column;
    rename.newText = item.funcdefName;
    edits.push_back(std::move(rename));

    lsp::CodeAction action;
    action.title = "Declare funcdef '" + item.funcdefName + "' for '" + item.functionName + "'";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = std::vector<lsp::Diagnostic>{diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = std::move(edits);
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
}

} // namespace

void TryAddHandleOnPrimitiveFix(const CodeActionRequest& request, TSNode rootNode,
                                std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, diagnostics::codes::HandleOnPrimitive))
        {
            continue;
        }

        const std::string_view line = angel_lsp::utils::GetLine(request.sourceCode, diag.range.start.line);
        const size_t at = line.find('@', diag.range.start.character);
        if (at == std::string_view::npos || at >= diag.range.end.character)
        {
            continue;
        }

        lsp::TextEdit edit;
        edit.range.start.line = diag.range.start.line;
        edit.range.start.character = static_cast<uint32_t>(at);
        edit.range.end.line = diag.range.start.line;
        edit.range.end.character = static_cast<uint32_t>(at + 1);
        edit.newText = "";

        lsp::CodeAction action;
        action.title = "Remove '@' - a primitive has no handle type";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;
        action.diagnostics = std::vector<lsp::Diagnostic>{diag};

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

void TryAddGenerateFuncdefFix(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-funcdef-missing"))
        {
            continue;
        }

        const TSPoint point = {diag.range.start.line, diag.range.start.character};
        TSNode typeNode = ts_node_descendant_for_point_range(rootNode, point, point);
        if (ts_node_is_null(typeNode))
        {
            continue;
        }

        const std::string functionName = GetNodeText(typeNode, request.sourceCode);
        if (functionName.empty())
        {
            continue;
        }

        const analysis::Symbol* target = FindUniqueGlobalFunction(request.symbolTable, functionName);
        if (!target)
        {
            continue;
        }

        const auto& signature = target->GetFunction();
        const std::string funcdefName = functionName + "Func";
        std::string declaration = FormatFuncdefDeclarationText(signature, funcdefName);
        EmitFuncdefCodeAction(request, diag,
                              FuncdefFixItem{typeNode, functionName, funcdefName, std::move(declaration)}, actions);
    }
}

void TryAddBoolConversionFix(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-bool-conversion"))
        {
            continue;
        }

        if (!std::holds_alternative<lsp::String>(diag.message))
        {
            continue;
        }
        const std::string& message = std::get<lsp::String>(diag.message);

        const size_t firstQuote = message.find('\'');
        const size_t firstClose =
            firstQuote == std::string::npos ? std::string::npos : message.find('\'', firstQuote + 1);
        const size_t secondQuote =
            firstClose == std::string::npos ? std::string::npos : message.find('\'', firstClose + 1);
        const size_t secondClose =
            secondQuote == std::string::npos ? std::string::npos : message.find('\'', secondQuote + 1);
        if (secondClose == std::string::npos)
        {
            continue;
        }

        const std::string oper = message.substr(secondQuote + 1, secondClose - secondQuote - 1);
        if (oper != "opImplConv" && oper != "opConv")
        {
            continue;
        }

        lsp::TextEdit edit;
        edit.range.start = diag.range.end;
        edit.range.end = diag.range.end;
        edit.newText = "." + oper + "()";

        lsp::CodeAction action;
        action.title = "Call " + oper + "() explicitly";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;
        action.diagnostics = std::vector<lsp::Diagnostic>{diag};

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

} // namespace angel_lsp::features
