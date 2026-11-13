/**
 * @file CodeActionQuickFixes.cpp
 * @brief Miscellaneous quick-fix code action providers (primitive handles, funcdefs, bool conversions).
 */

#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "i18n/i18n.h"
#include "parser/Keywords.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis::code_action
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
        [&](std::string_view name, const std::vector<analysis::Symbol>& symbols)
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
    std::string declaration{parser::keywords::Funcdef};
    declaration += ' ';
    declaration += signature.returnType.empty() ? parser::keywords::Void : std::string_view{signature.returnType};
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
 * @brief Emits a quick-fix entry that prepends a funcdef declaration and renames the type reference.
 * @param[in] query Incoming code action query context.
 * @param[in] diagIndex Triggering diagnostic index.
 * @param[in] item Funcdef fix metadata item.
 * @param[in,out] entries Destination entries vector.
 */
static void EmitFuncdefEntry(const CodeActionQuery& query, uint32_t diagIndex, const FuncdefFixItem& item,
                             std::vector<CodeActionEntry>& entries)
{
    std::vector<CodeActionTextEdit> edits;
    edits.push_back(CodeActionTextEdit{
        analysis::SourceRange{0, 0, 0, 0},
        item.declaration,
    });
    edits.push_back(CodeActionTextEdit{
        item.typeRange,
        item.funcdefName,
    });

    std::string title = i18n::FormatMessage(query.i18n, "action-declare-funcdef", "Declare funcdef '{}' for '{}'",
                                            std::string_view(item.funcdefName), std::string_view(item.functionName));

    entries.push_back(MakeQuickFixEntry(QuickFixEntryOptions{
        std::move(title),
        diagIndex,
        std::move(edits),
        true,
    }));
}

/**
 * @brief Generates quick-fix entries to remove illegal handle modifiers from primitive types.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddHandleOnPrimitiveFix(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        if (!MatchDiagnosticCode(diag, diagnostics::codes::HandleOnPrimitive))
        {
            continue;
        }

        const std::string_view line = angel_lsp::utils::GetLine(query.sourceCode, diag.range.startLine);
        const size_t at = line.find_first_of('@', diag.range.startCharacter);
        if (at == std::string_view::npos || at >= diag.range.endCharacter)
        {
            continue;
        }

        CodeActionTextEdit edit{analysis::SourceRange{diag.range.startLine, static_cast<uint32_t>(at),
                                                      diag.range.startLine, static_cast<uint32_t>(at + 1)},
                                ""};

        std::string title = i18n::FormatMessage(query.i18n, "action-remove-primitive-handle",
                                                "Remove '@' - a primitive has no handle type");

        entries.push_back(MakeQuickFixEntry(QuickFixEntryOptions{std::move(title), i, {std::move(edit)}, true}));
    }
}

/**
 * @brief Generates quick-fix entries to declare missing funcdef signatures.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddGenerateFuncdefFix(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        if (!MatchDiagnosticCode(diag, "as-hint-funcdef-missing"))
        {
            continue;
        }

        const auto typeSpan =
            analysis::FindNodeSpanAtPoint(*query.nodeIndex, diag.range.startLine, diag.range.startCharacter);
        if (!typeSpan)
        {
            continue;
        }

        const std::string_view functionNameView =
            utils::SliceSource(query.sourceCode, typeSpan->bytes.startByte, typeSpan->bytes.endByte);
        if (functionNameView.empty())
        {
            continue;
        }

        const std::string functionName(functionNameView);
        const analysis::Symbol* target = FindUniqueGlobalFunction(query.symbolTable, functionName);
        if (!target)
        {
            continue;
        }

        const auto& signature = target->GetFunction();
        const std::string funcdefName = functionName + "Func";
        std::string declaration = FormatFuncdefDeclarationText(signature, funcdefName);
        EmitFuncdefEntry(query, i, FuncdefFixItem{typeSpan->range, functionName, funcdefName, std::move(declaration)},
                         entries);
    }
}

/**
 * @brief Generates quick-fix entries for explicit bool conversion calls and diagnostic suppression.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddBoolConversionFix(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        if (!MatchDiagnosticCode(diag, "as-hint-bool-conversion"))
        {
            continue;
        }

        if (!diag.messageIsPlainText)
        {
            continue;
        }
        const std::string_view message = diag.message;

        const size_t firstQuote = message.find('\'');
        const size_t firstClose =
            firstQuote == std::string_view::npos ? std::string_view::npos : message.find('\'', firstQuote + 1);
        const size_t secondQuote =
            firstClose == std::string_view::npos ? std::string_view::npos : message.find('\'', firstClose + 1);
        const size_t secondClose =
            secondQuote == std::string_view::npos ? std::string_view::npos : message.find('\'', secondQuote + 1);
        if (secondClose == std::string_view::npos)
        {
            continue;
        }

        const std::string_view oper = message.substr(secondQuote + 1, secondClose - secondQuote - 1);
        if (oper != "opImplConv" && oper != "opConv")
        {
            continue;
        }

        CodeActionTextEdit edit{analysis::SourceRange{diag.range.endLine, diag.range.endCharacter, diag.range.endLine,
                                                      diag.range.endCharacter},
                                "." + std::string(oper) + "()"};

        std::string title = i18n::FormatMessage(query.i18n, "action-call-explicitly", "Call {}() explicitly", oper);

        entries.push_back(MakeQuickFixEntry(QuickFixEntryOptions{std::move(title), i, {std::move(edit)}, true}));

        entries.push_back(MakeDisableDiagnosticEntry(DisableDiagnosticEntryOptions{
            i, "diagnostics.reportBoolConversion", "as-hint-bool-conversion", query.i18n}));
    }
}

} // namespace angel_lsp::analysis::code_action
