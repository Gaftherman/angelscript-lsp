/**
 * @file CodeActionExtractVariable.cpp
 * @brief Refactoring provider for Extract Variable code action.
 */

#include "analysis/index/ExtractVariableFacts.h"
#include "analysis/resolution/CodeActionResolution.h"
#include "features/code_action/CodeActionInternal.h"
#include "features/common/FeatureSharedUtils.h"
#include "parser/Keywords.h"
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
    if (fnNameText.starts_with("get_") && fnNameText.size() > 4)
    {
        return ToLowerFirst(fnNameText.substr(4));
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
                                       const CodeActionRequest& request, const analysis::Scope* scope)
{
    std::string candidate = SuggestCandidateName(site, varType, request.sourceCode, &request.symbolTable);
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
 * @brief Constructs an Extract Variable refactoring code action.
 * @param[in] request Code action request context.
 * @param[in] site Extract variable candidate site facts.
 * @return Code action if construction succeeds, std::nullopt otherwise.
 */
static std::optional<lsp::CodeAction> BuildExtractVariableAction(const CodeActionRequest& request,
                                                                 const analysis::IndexedExtractVariableSite& site)
{
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), site.expressionRange.startLine);

    std::string varType = analysis::ResolveExpressionTypeAt(
        site.expression, {scope, request.symbolTable, request.sourceCode, request.uri, request.nodeIndex});
    if (analysis::CleanBaseTypeView(varType) == "void")
    {
        return std::nullopt;
    }
    if (varType.empty() || varType == "null")
    {
        varType = "auto";
    }

    std::string varName = SuggestVariableName(site, varType, request, scope);
    const std::string_view exprText =
        utils::SliceSource(request.sourceCode, site.expression.startByte, site.expression.endByte);
    if (exprText.empty())
    {
        return std::nullopt;
    }

    uint32_t stmtRow = site.statementLine;
    std::string indent = GetLineIndentation(request.sourceCode, stmtRow);

    lsp::TextEdit declEdit;
    if (site.statementSharesBlockLine)
    {
        declEdit.range = lsp::Range{{stmtRow, site.statementColumn}, {stmtRow, site.statementColumn}};
        declEdit.newText = varType + " " + varName + " = " + std::string(exprText) + "; ";
    }
    else
    {
        declEdit.range = lsp::Range{{stmtRow, 0}, {stmtRow, 0}};
        declEdit.newText = indent + varType + " " + varName + " = " + std::string(exprText) + ";\n";
    }

    lsp::TextEdit replEdit;
    replEdit.range = common::ToLspRange(site.expressionRange);
    replEdit.newText = varName;

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-extract-variable", "Extract Variable");
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(declEdit), std::move(replEdit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    return action;
}

void TryAddExtractVariableAction(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return;
    }
    const auto site = analysis::FindExtractVariableSite(
        *request.nodeIndex, analysis::SourceRange{request.range.start.line, request.range.start.character,
                                                  request.range.end.line, request.range.end.character});
    if (!site)
    {
        return;
    }
    if (auto action = BuildExtractVariableAction(request, *site))
    {
        actions.push_back(std::move(*action));
    }
}

} // namespace angel_lsp::features
