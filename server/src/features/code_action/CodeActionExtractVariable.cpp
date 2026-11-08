/**
 * @file CodeActionExtractVariable.cpp
 * @brief Refactoring provider for Extract Variable code action.
 */

#include "analysis/index/ExtractVariableFacts.h"
#include "analysis/resolution/CodeActionResolution.h"
#include "features/code_action/CodeActionInternal.h"
#include "parser/Keywords.h"
#include "utils/AccessorNames.h"
#include "utils/Utils.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

static std::string ToLowerFirst(std::string_view str)
{
    if (str.empty())
    {
        return "newVar";
    }
    std::string result(str);
    result[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[0])));
    return result;
}

static std::string SuggestNameFromCall(std::string_view fnNameText)
{
    if (fnNameText.starts_with("Get") && fnNameText.size() > 3)
    {
        return ToLowerFirst(fnNameText.substr(3));
    }
    if (fnNameText.starts_with(utils::k_getterPrefix) && fnNameText.size() > utils::k_getterPrefix.size())
    {
        return ToLowerFirst(fnNameText.substr(utils::k_getterPrefix.size()));
    }
    if (fnNameText.starts_with("Find") && fnNameText.size() > 4)
    {
        return ToLowerFirst(fnNameText.substr(4));
    }
    if (fnNameText.starts_with("Create") && fnNameText.size() > 6)
    {
        return ToLowerFirst(fnNameText.substr(6));
    }
    return "";
}

static std::string SuggestNameFromMemberOrConstruct(analysis::ExtractNameHintKind kind, std::string_view hintText)
{
    if (kind == analysis::ExtractNameHintKind::Member)
    {
        if (hintText.starts_with("m_") && hintText.size() > 2)
        {
            return ToLowerFirst(hintText.substr(2));
        }
        if (!hintText.empty())
        {
            return ToLowerFirst(hintText);
        }
    }
    if (kind == analysis::ExtractNameHintKind::ConstructType)
    {
        if (!hintText.empty())
        {
            return ToLowerFirst(hintText);
        }
    }
    return "";
}

static std::string SuggestCandidateName(const analysis::IndexedExtractVariableSite& site, std::string_view varType,
                                        std::string_view sourceCode, const analysis::SymbolTable* symbolTable)
{
    const std::string_view hintText = utils::SliceSource(sourceCode, site.hintName.startByte, site.hintName.endByte);
    if (site.hintKind == analysis::ExtractNameHintKind::CallCallee)
    {
        if (std::string callName = SuggestNameFromCall(hintText); !callName.empty())
        {
            return callName;
        }
    }
    if (std::string memName = SuggestNameFromMemberOrConstruct(site.hintKind, hintText); !memName.empty())
    {
        return memName;
    }
    const std::string_view strType = symbolTable ? symbolTable->GetStringType() : std::string_view("string");
    if (!varType.empty() && varType != parser::keywords::Auto && varType != parser::keywords::Null &&
        !analysis::IsPrimitiveTypeName(varType))
    {
        std::string clean = analysis::CleanBaseType(varType);
        if (!clean.empty() && clean != strType)
        {
            return ToLowerFirst(clean);
        }
    }
    return "newVar";
}

static bool IsNameInScope(const analysis::Scope* scope, std::string_view name)
{
    const analysis::Scope* curr = scope;
    while (curr)
    {
        for (const auto& def : curr->definitions)
        {
            if (def.name == name)
            {
                return true;
            }
        }
        curr = curr->parent;
    }
    return false;
}

static std::string SuggestVariableName(const analysis::IndexedExtractVariableSite& site, std::string_view varType,
                                       const analysis::code_action::CodeActionQuery& query,
                                       const analysis::Scope* scope)
{
    std::string candidate = SuggestCandidateName(site, varType, query.sourceCode, &query.symbolTable);
    if (!scope)
    {
        return candidate;
    }
    std::string result = candidate;
    int suffix = 1;
    while (IsNameInScope(scope, result))
    {
        result = candidate + std::to_string(suffix++);
    }
    return result;
}

/**
 * @brief Constructs an Extract Variable refactoring code action entry.
 * @param query Code action query context.
 * @param site Extract variable candidate site facts.
 * @return Code action entry if construction succeeds, std::nullopt otherwise.
 */
static std::optional<analysis::code_action::CodeActionEntry>
BuildExtractVariableEntry(const analysis::code_action::CodeActionQuery& query,
                          const analysis::IndexedExtractVariableSite& site)
{
    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), site.expressionRange.startLine);

    std::string varType = analysis::ResolveExpressionTypeAt(
        site.expression, {scope, query.symbolTable, query.sourceCode, query.uri, query.nodeIndex});
    if (analysis::CleanBaseTypeView(varType) == "void")
    {
        return std::nullopt;
    }
    if (varType.empty() || varType == "null")
    {
        varType = "auto";
    }

    std::string varName = SuggestVariableName(site, varType, query, scope);
    const std::string_view exprText =
        utils::SliceSource(query.sourceCode, site.expression.startByte, site.expression.endByte);
    if (exprText.empty())
    {
        return std::nullopt;
    }

    uint32_t stmtRow = site.statementLine;
    std::string indent = GetLineIndentation(query.sourceCode, stmtRow);

    analysis::code_action::CodeActionTextEdit declEdit;
    if (site.statementSharesBlockLine)
    {
        declEdit.range = analysis::SourceRange{stmtRow, site.statementColumn, stmtRow, site.statementColumn};
        declEdit.newText = varType + " " + varName + " = " + std::string(exprText) + "; ";
    }
    else
    {
        declEdit.range = analysis::SourceRange{stmtRow, 0, stmtRow, 0};
        declEdit.newText = indent + varType + " " + varName + " = " + std::string(exprText) + ";\n";
    }

    analysis::code_action::CodeActionTextEdit replEdit;
    replEdit.range = site.expressionRange;
    replEdit.newText = varName;

    analysis::code_action::CodeActionEntry entry;
    entry.title = i18n::FormatMessage(query.i18n, "action-extract-variable", "Extract Variable");
    entry.kind = analysis::code_action::CodeActionEntryKind::RefactorExtract;
    entry.edits = std::vector<analysis::code_action::CodeActionTextEdit>{std::move(declEdit), std::move(replEdit)};

    return entry;
}

/**
 * @brief Generates a refactoring entry to extract an expression into a local variable.
 * @param query Incoming code action query context.
 * @param entries Destination vector for generated code action entries.
 */
void TryAddExtractVariableAction(const analysis::code_action::CodeActionQuery& query,
                                 std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }
    const auto site = analysis::FindExtractVariableSite(*query.nodeIndex, query.range);
    if (!site)
    {
        return;
    }
    if (auto entry = BuildExtractVariableEntry(query, *site))
    {
        entries.push_back(std::move(*entry));
    }
}

} // namespace angel_lsp::features
