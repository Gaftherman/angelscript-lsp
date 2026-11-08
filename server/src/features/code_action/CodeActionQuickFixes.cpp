/**
 * @file CodeActionQuickFixes.cpp
 * @brief Miscellaneous quick-fix code action providers (primitive handles, funcdefs, bool conversions).
 */

#include "analysis/documentation/SignatureFormatter.h"
#include "features/code_action/CodeActionInternal.h"
#include "features/common/FeatureSharedUtils.h"
#include "i18n/i18n.h"

namespace angel_lsp::features
{

/**
 * @brief Locates a unique global function symbol by name.
 * @param[in] table Symbol table.
 * @param[in] functionName Name of global function.
 * @return Unique function symbol pointer or nullptr if ambiguous/missing.
 */
static const analysis::Symbol* FindUniqueGlobalFunction(const analysis::SymbolTable& table,
                                                        const std::string& functionName)
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
static std::string FormatFuncdefDeclarationText(const analysis::FunctionSignature& signature,
                                                const std::string& funcdefName)
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
        declaration += analysis::FormatParameter(signature.parameters[i]);
    }
    declaration += ");\n";
    return declaration;
}

/**
 * @brief Emits a quick fix that prepends a funcdef declaration and renames the type reference.
 * @param[in] request Code action request.
 * @param[in] diag Triggering diagnostic.
 * @param[in] item Funcdef fix metadata item.
 * @param[out] actions Destination actions vector.
 */
static void EmitFuncdefCodeAction(const CodeActionRequest& request, const lsp::Diagnostic& diag,
                                  const FuncdefFixItem& item, std::vector<lsp::CodeAction>& actions)
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
    rename.range = common::ToLspRange(item.typeRange);
    rename.newText = item.funcdefName;
    edits.push_back(std::move(rename));

    actions.push_back(MakeQuickFixAction(QuickFixOptions{
        .title = i18n::FormatMessage(request.i18n, "action-declare-funcdef", "Declare funcdef '{}' for '{}'",
                                     std::string_view(item.funcdefName), std::string_view(item.functionName)),
        .diag = diag,
        .uri = request.uri,
        .edits = std::move(edits),
        .isPreferred = true,
    }));
}

/**
 * @brief Attempts to construct a QuickFix CodeAction to remove illegal handle '@' modifiers from primitive types.
 * @param[in] request Code action request bundle containing AST root, source text, and diagnostics.
 * @param[in,out] actions Output vector where constructed quick fix actions are appended.
 */
void TryAddHandleOnPrimitiveFix(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (!request.nodeIndex || request.sourceCode.empty())
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
        const size_t at = line.find_first_of('@', diag.range.start.character);
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

        actions.push_back(MakeQuickFixAction(QuickFixOptions{
            .title = i18n::FormatMessage(request.i18n, "action-remove-primitive-handle",
                                         "Remove '@' - a primitive has no handle type"),
            .diag = diag,
            .uri = request.uri,
            .edits = {std::move(edit)},
            .isPreferred = true,
        }));
    }
}

void TryAddGenerateFuncdefFix(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-funcdef-missing"))
        {
            continue;
        }

        const auto typeSpan =
            analysis::FindNodeSpanAtPoint(*request.nodeIndex, diag.range.start.line, diag.range.start.character);
        if (!typeSpan)
        {
            continue;
        }

        const std::string_view functionNameView =
            utils::SliceSource(request.sourceCode, typeSpan->bytes.startByte, typeSpan->bytes.endByte);
        if (functionNameView.empty())
        {
            continue;
        }

        const std::string functionName(functionNameView);
        const analysis::Symbol* target = FindUniqueGlobalFunction(request.symbolTable, functionName);
        if (!target)
        {
            continue;
        }

        const auto& signature = target->GetFunction();
        const std::string funcdefName = functionName + "Func";
        std::string declaration = FormatFuncdefDeclarationText(signature, funcdefName);
        EmitFuncdefCodeAction(
            request, diag, FuncdefFixItem{typeSpan->range, functionName, funcdefName, std::move(declaration)}, actions);
    }
}

void TryAddBoolConversionFix(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (!request.nodeIndex || request.sourceCode.empty())
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

        actions.push_back(MakeQuickFixAction(QuickFixOptions{
            .title = i18n::FormatMessage(request.i18n, "action-call-explicitly", "Call {}() explicitly", oper),
            .diag = diag,
            .uri = request.uri,
            .edits = {std::move(edit)},
            .isPreferred = true,
        }));

        actions.push_back(MakeDisableDiagnosticAction(
            {diag, "diagnostics.reportBoolConversion", "as-hint-bool-conversion", request.i18n}));
    }
}

} // namespace angel_lsp::features
