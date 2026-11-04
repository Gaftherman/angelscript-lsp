/**
 * @file CodeActionInterfaceImplementer.cpp
 * @brief Quick-fix provider for generating missing interface method implementations in classes.
 */

#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/types/TypeSanitization.h"
#include "config/ServerConfig.h"
#include "features/code_action/CodeActionInternal.h"
#include "parser/ASTUtils.h"
#include "parser/Primitives.h"

namespace angel_lsp::features
{

/**
 * @brief Returns default literal return string for a return type.
 * @param[in] returnType Return type text.
 * @return Default literal return text ("null", "false", "0", "\"\"").
 */
static std::string GetDefaultReturnValue(std::string_view returnType, std::string_view stringTypeName)
{
    std::string cleanRet = analysis::CleanBaseType(returnType);
    if (analysis::IsHandleTypeView(returnType))
    {
        return "null";
    }
    if (cleanRet == "bool")
    {
        return "false";
    }
    if (!stringTypeName.empty() && cleanRet == stringTypeName)
    {
        return "\"\"";
    }
    return parser::primitives::IsNumeric(cleanRet) ? "0" : "";
}

/**
 * @brief Formats stub declarations for missing interface methods.
 * @param[in] missingMethods List of missing method symbols.
 * @param[in] stringTypeName Workspace-configured string type name.
 * @return Formatted stub code string.
 */
static std::string FormatMissingInterfaceMethodStubs(const std::vector<analysis::Symbol>& missingMethods,
                                                     std::string_view stringTypeName)
{
    std::string stubs;
    for (const auto& m : missingMethods)
    {
        const auto& fn = m.GetFunction();
        std::string ret = fn.returnType.empty() ? "void" : fn.returnType;
        if (fn.modifiers.isReturnReference && analysis::TrailingReferenceModifierLength(ret) == 0)
        {
            ret += "&";
        }
        stubs += "\n    " + ret + " " + m.name + "(";
        for (size_t p = 0; p < fn.parameters.size(); ++p)
        {
            if (p > 0)
            {
                stubs += ", ";
            }
            const auto& param = fn.parameters[p];
            auto parameter = param;
            parameter.defaultValue.clear();
            stubs += analysis::FormatParameter(parameter);
        }
        stubs += ")";
        if (fn.modifiers.isConst)
        {
            stubs += " const";
        }
        stubs += " override\n    {\n";
        if (ret != "void")
        {
            const auto value = GetDefaultReturnValue(ret, stringTypeName);
            if (value.empty() || fn.modifiers.isReturnReference)
            {
                return {};
            }
            stubs += "        return " + value + ";\n";
        }
        stubs += "    }\n";
    }
    return stubs;
}

/**
 * @brief Locates the insertion position right after the opening brace of a class declaration.
 * @param[in] rootNode Root AST node.
 * @param[in] clsSym Class symbol.
 * @return LSP position for method stub insertion.
 */
static lsp::Position FindClassInterfaceInsertionPosition(TSNode rootNode, const analysis::Symbol& clsSym)
{
    lsp::Position insertPos{clsSym.endLine, clsSym.endCharacter};
    TSPoint cPt = {clsSym.startLine, clsSym.startCharacter};
    const auto& gs = parser::GetGrammarSymbols();
    TSNode cNode =
        parser::FindAncestorWithSymbol(ts_node_descendant_for_point_range(rootNode, cPt, cPt), gs.symClassDeclaration);
    if (ts_node_is_null(cNode))
    {
        return insertPos;
    }

    TSNode bodyNode = parser::GetChildByField(cNode, parser::GetGrammarSymbols().fieldBody);
    if (ts_node_is_null(bodyNode))
    {
        TSTreeCursor declCursor = ts_tree_cursor_new(cNode);
        if (ts_tree_cursor_goto_first_child(&declCursor))
        {
            do
            {
                TSNode ch = ts_tree_cursor_current_node(&declCursor);
                if (ts_node_symbol(ch) == parser::GetGrammarSymbols().symClassBody)
                {
                    bodyNode = ch;
                    break;
                }
            } while (ts_tree_cursor_goto_next_sibling(&declCursor));
        }
        ts_tree_cursor_delete(&declCursor);
    }

    if (!ts_node_is_null(bodyNode))
    {
        TSTreeCursor cursor = ts_tree_cursor_new(bodyNode);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode bChild = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(bChild) == parser::GetGrammarSymbols().tokOpenBrace)
                {
                    TSPoint pt = ts_node_end_point(bChild);
                    insertPos = lsp::Position{pt.row, pt.column};
                    break;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);
    }
    return insertPos;
}

/**
 * @brief Collects diagnostics matching missing interface implementation for a class symbol.
 * @param[in] contextDiagnostics LSP diagnostics in request context.
 * @param[in] clsSym Class symbol to filter diagnostics by line range.
 * @return Vector of matching diagnostics.
 */
static std::vector<lsp::Diagnostic>
CollectMatchingInterfaceDiagnostics(const std::vector<lsp::Diagnostic>& contextDiagnostics,
                                    const analysis::Symbol& clsSym)
{
    std::vector<lsp::Diagnostic> matching;
    for (const auto& diag : contextDiagnostics)
    {
        if (MatchDiagnosticCode(diag, "as-err-interface-impl-missing") && diag.range.start.line <= clsSym.endLine &&
            diag.range.end.line >= clsSym.startLine)
        {
            matching.push_back(diag);
        }
    }
    return matching;
}

void TryAddImplementInterfaceFixes(const CodeActionRequest& request, TSNode rootNode,
                                   std::vector<lsp::CodeAction>& actions)
{
    const std::string_view strType = (request.config && !request.config->types.stringTypeName.empty())
                                         ? std::string_view(request.config->types.stringTypeName)
                                         : request.symbolTable.GetStringType();

    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& clsSym : symbols)
            {
                if (clsSym.type != analysis::SymbolType::Class || clsSym.fileUri != request.uri)
                {
                    continue;
                }
                if (request.range.start.line > clsSym.endLine || request.range.end.line < clsSym.startLine)
                {
                    continue;
                }

                const auto missing = analysis::FindMissingInterfaceMethods(clsSym, request.symbolTable);
                ankerl::unordered_dense::map<std::string, std::vector<analysis::Symbol>> byInterface;
                ankerl::unordered_dense::set<std::string> blocked;
                for (const auto& entry : missing)
                {
                    if (!entry.canOverride)
                    {
                        blocked.insert(entry.interfaceName);
                    }
                    byInterface[entry.interfaceName].push_back(entry.method);
                }
                for (const auto& [cleanIface, methods] : byInterface)
                {
                    if (blocked.contains(cleanIface))
                    {
                        continue;
                    }
                    auto stubs = FormatMissingInterfaceMethodStubs(methods, strType);
                    if (stubs.empty())
                    {
                        continue;
                    }
                    lsp::Position insertPos = FindClassInterfaceInsertionPosition(rootNode, clsSym);

                    lsp::CodeAction action;
                    action.title = i18n::FormatMessage(request.i18n, "action-implement-interface",
                                                       "Implement missing interface methods for '{}'",
                                                       std::string_view(cleanIface));
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                    action.isPreferred = true;

                    auto matchingDiags = CollectMatchingInterfaceDiagnostics(request.context.diagnostics, clsSym);
                    if (!matchingDiags.empty())
                    {
                        action.diagnostics = std::move(matchingDiags);
                    }

                    lsp::WorkspaceEdit wsEdit;
                    wsEdit.changes = lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>>{
                        {lsp::DocumentUri::parse(request.uri),
                         {lsp::TextEdit{lsp::Range{insertPos, insertPos}, std::move(stubs)}}}};
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }
            }
        });
}

} // namespace angel_lsp::features
