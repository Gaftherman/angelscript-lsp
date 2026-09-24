/**
 * @file CodeActionInterfaceImplementer.cpp
 * @brief Quick-fix provider for generating missing interface method implementations in classes.
 */

#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Returns default literal return string for a return type.
 * @param[in] returnType Return type text.
 * @return Default literal return text ("null", "false", "0", "\"\"").
 */
std::string GetDefaultReturnValue(std::string_view returnType)
{
    std::string cleanRet = analysis::CleanBaseType(returnType);
    if (returnType.ends_with("@"))
    {
        return "null";
    }
    if (cleanRet == "bool")
    {
        return "false";
    }
    if (cleanRet == "string")
    {
        return "\"\"";
    }
    static const ankerl::unordered_dense::set<std::string_view> kNumericTypes = {
        "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "float", "double"};
    if (kNumericTypes.contains(cleanRet))
    {
        return "0";
    }
    return "null";
}

/**
 * @brief Formats stub declarations for missing interface methods.
 * @param[in] missingMethods List of missing method symbols.
 * @return Formatted stub code string.
 */
std::string FormatMissingInterfaceMethodStubs(const std::vector<analysis::Symbol>& missingMethods)
{
    std::string stubs;
    for (const auto& m : missingMethods)
    {
        const auto& fn = m.GetFunction();
        std::string ret = fn.returnType.empty() ? "void" : fn.returnType;
        stubs += "\n    " + ret + " " + m.name + "(";
        for (size_t p = 0; p < fn.parameters.size(); ++p)
        {
            if (p > 0)
            {
                stubs += ", ";
            }
            const auto& param = fn.parameters[p];
            stubs += param.typeName;
            if (!param.name.empty())
            {
                stubs += " " + param.name;
            }
            if (!param.defaultValue.empty())
            {
                stubs += " = " + param.defaultValue;
            }
        }
        stubs += ")\n    {\n";
        if (ret != "void")
        {
            std::string defaultVal = GetDefaultReturnValue(ret);
            stubs += "        return " + defaultVal + ";\n";
        }
        stubs += "    }\n";
    }
    return stubs;
}

/**
 * @brief Identifies which interface methods are not implemented by a class.
 * @param[in] className Name of implementing class.
 * @param[in] cleanIface Cleaned interface type name.
 * @param[in] table Symbol table.
 * @return Vector of missing interface method symbols.
 */
std::vector<analysis::Symbol> CollectMissingInterfaceMethods(const std::string& className,
                                                             const std::string& cleanIface,
                                                             const analysis::SymbolTable& table)
{
    auto ifaceHierarchy = analysis::GetInheritedTypeHierarchy(cleanIface, table);
    if (ifaceHierarchy.empty())
    {
        return {};
    }

    std::vector<analysis::Symbol> ifaceMethods;
    for (const auto& ifaceName : ifaceHierarchy)
    {
        table.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& mSyms)
            {
                for (const auto& m : mSyms)
                {
                    if (m.type == analysis::SymbolType::Function && m.containerName == ifaceName)
                    {
                        ifaceMethods.push_back(m);
                    }
                }
            });
    }

    std::vector<analysis::Symbol> classMethods;
    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& mSyms)
        {
            for (const auto& m : mSyms)
            {
                if (m.type == analysis::SymbolType::Function && m.containerName == className)
                {
                    classMethods.push_back(m);
                }
            }
        });

    std::vector<analysis::Symbol> missingMethods;
    for (const auto& ifMethod : ifaceMethods)
    {
        bool implemented = false;
        for (const auto& cMethod : classMethods)
        {
            if (cMethod.name == ifMethod.name &&
                cMethod.GetFunction().parameters.size() == ifMethod.GetFunction().parameters.size())
            {
                implemented = true;
                break;
            }
        }
        if (!implemented)
        {
            missingMethods.push_back(ifMethod);
        }
    }
    return missingMethods;
}

/**
 * @brief Locates the insertion position before the closing brace of a class declaration.
 * @param[in] rootNode Root AST node.
 * @param[in] clsSym Class symbol.
 * @return LSP position for method stub insertion.
 */
lsp::Position FindClassInterfaceInsertionPosition(TSNode rootNode, const analysis::Symbol& clsSym)
{
    lsp::Position insertPos{clsSym.endLine, clsSym.endCharacter};
    TSPoint cPt = {clsSym.startLine, clsSym.startCharacter};
    TSNode cNode = ts_node_descendant_for_point_range(rootNode, cPt, cPt);
    while (!ts_node_is_null(cNode) && std::string_view(ts_node_type(cNode)) != "class_declaration")
    {
        cNode = ts_node_parent(cNode);
    }
    if (ts_node_is_null(cNode))
    {
        return insertPos;
    }

    TSNode bodyNode = parser::GetChildByField(cNode, parser::fields::Body);
    if (ts_node_is_null(bodyNode))
    {
        uint32_t cnt = ts_node_child_count(cNode);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(cNode, i);
            if (std::string_view(ts_node_type(ch)) == "class_body")
            {
                bodyNode = ch;
                break;
            }
        }
    }

    if (!ts_node_is_null(bodyNode))
    {
        uint32_t bCount = ts_node_child_count(bodyNode);
        for (int i = static_cast<int>(bCount) - 1; i >= 0; --i)
        {
            TSNode bChild = ts_node_child(bodyNode, static_cast<uint32_t>(i));
            if (std::string_view(ts_node_type(bChild)) == "}")
            {
                TSPoint pt = ts_node_start_point(bChild);
                insertPos = lsp::Position{pt.row, pt.column};
                break;
            }
        }
    }
    return insertPos;
}

} // namespace

void TryAddImplementInterfaceFixes(const CodeActionRequest& request, TSNode rootNode,
                                   std::vector<lsp::CodeAction>& actions)
{
    request.symbolTable.ForEachSymbol(
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

                const auto& cls = clsSym.GetClass();
                for (const auto& baseName : cls.bases)
                {
                    std::string cleanIface = analysis::CleanBaseType(baseName);
                    if (cleanIface.empty())
                    {
                        continue;
                    }

                    auto missingMethods = CollectMissingInterfaceMethods(clsSym.name, cleanIface, request.symbolTable);
                    if (missingMethods.empty())
                    {
                        continue;
                    }

                    std::string stubs = FormatMissingInterfaceMethodStubs(missingMethods);
                    lsp::Position insertPos = FindClassInterfaceInsertionPosition(rootNode, clsSym);

                    lsp::TextEdit edit;
                    edit.range = lsp::Range{insertPos, insertPos};
                    edit.newText = stubs;

                    lsp::CodeAction action;
                    action.title = "Implement missing interface methods for '" + cleanIface + "'";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                    action.isPreferred = true;

                    std::vector<lsp::Diagnostic> matchingDiags;
                    for (const auto& diag : request.context.diagnostics)
                    {
                        if (MatchDiagnosticCode(diag, "as-err-interface-impl-missing") &&
                            diag.range.start.line <= clsSym.endLine && diag.range.end.line >= clsSym.startLine)
                        {
                            matchingDiags.push_back(diag);
                        }
                    }
                    if (!matchingDiags.empty())
                    {
                        action.diagnostics = std::move(matchingDiags);
                    }

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)].push_back(std::move(edit));
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }
            }
        });
}

} // namespace angel_lsp::features
