#include "features/code_action/CodeActionHandler.h"
#include "analysis/SemanticHelpers.h"
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Extracts text slice of an AST node from the source code.
         */
        std::string GetNodeText(TSNode node, std::string_view sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return "";
            }
            uint32_t startByte = ts_node_start_byte(node);
            uint32_t endByte = ts_node_end_byte(node);
            if (startByte >= sourceCode.size() || endByte > sourceCode.size() || startByte >= endByte)
            {
                return "";
            }
            return std::string(sourceCode.substr(startByte, endByte - startByte));
        }

        /**
         * @brief Recursively marks all LocalDefinition entries as used if they have a matching reference.
         */
        void CollectUsedDefinitions(
            const analysis::Scope *scope,
            ankerl::unordered_dense::set<const analysis::LocalDefinition *> &used)
        {
            if (!scope)
            {
                return;
            }

            for (const auto &ref : scope->references)
            {
                if (ref.isMemberAccess)
                {
                    continue;
                }

                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, ref.name);
                if (!def)
                {
                    continue;
                }

                // Skip self-reference at the declaration site
                if (def->startLine == ref.startLine && def->startCharacter == ref.startCharacter &&
                    def->endLine == ref.endLine && def->endCharacter == ref.endCharacter)
                {
                    continue;
                }

                used.insert(def);
            }

            for (const auto &child : scope->children)
            {
                CollectUsedDefinitions(child.get(), used);
            }
        }

        /**
         * @brief Recursively collects all unused local variables inside function scopes.
         */
        void CollectUnusedVariables(
            const analysis::Scope *scope,
            const ankerl::unordered_dense::set<const analysis::LocalDefinition *> &used,
            bool isFunctionNested,
            std::vector<const analysis::LocalDefinition *> &unused)
        {
            if (!scope)
            {
                return;
            }

            bool funcNested = isFunctionNested || scope->isFunctionScope;

            if (funcNested)
            {
                for (const auto &def : scope->definitions)
                {
                    if (def.kind == analysis::LocalDefinitionKind::Variable && !used.contains(&def))
                    {
                        unused.push_back(&def);
                    }
                }
            }

            for (const auto &child : scope->children)
            {
                CollectUnusedVariables(child.get(), used, funcNested, unused);
            }
        }

        /**
         * @brief Returns default literal return string for a return type.
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
            if (cleanRet == "int" || cleanRet == "int8" || cleanRet == "int16" || cleanRet == "int32" || cleanRet == "int64" ||
                cleanRet == "uint" || cleanRet == "uint8" || cleanRet == "uint16" || cleanRet == "uint32" || cleanRet == "uint64" ||
                cleanRet == "float" || cleanRet == "double")
            {
                return "0";
            }
            return "null";
        }
    }

    std::optional<std::vector<lsp::CodeAction>> GetCodeActions(const CodeActionRequest &request)
    {
        if (!request.tree || request.sourceCode.empty())
        {
            return std::nullopt;
        }

        TSNode rootNode = ts_tree_root_node(request.tree);
        if (ts_node_is_null(rootNode))
        {
            return std::nullopt;
        }

        std::vector<lsp::CodeAction> actions;

        // =========================================================================
        // Quick-Fix 1: Remove Unused Local Variables
        // =========================================================================
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        if (rootScope)
        {
            ankerl::unordered_dense::set<const analysis::LocalDefinition *> usedDefs;
            CollectUsedDefinitions(rootScope.get(), usedDefs);

            std::vector<const analysis::LocalDefinition *> unusedVars;
            CollectUnusedVariables(rootScope.get(), usedDefs, false, unusedVars);

            for (const auto *def : unusedVars)
            {
                bool matchesRange = (request.range.start.line <= def->endLine && request.range.end.line >= def->startLine);
                bool matchesDiag = false;

                for (const auto &diag : request.context.diagnostics)
                {
                    if (diag.range.start.line <= def->endLine && diag.range.end.line >= def->startLine)
                    {
                        matchesDiag = true;
                        break;
                    }
                }

                // If request range overlaps the unused variable or diagnostic overlaps it
                if (matchesRange || matchesDiag)
                {
                    TSPoint pt = { def->startLine, def->startCharacter };
                    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);

                    TSNode declarator = leaf;
                    while (!ts_node_is_null(declarator) && std::string_view(ts_node_type(declarator)) != "variable_declarator")
                    {
                        declarator = ts_node_parent(declarator);
                    }

                    TSNode decl = declarator;
                    while (!ts_node_is_null(decl) && std::string_view(ts_node_type(decl)) != "variable_declaration")
                    {
                        decl = ts_node_parent(decl);
                    }

                    lsp::TextEdit edit;

                    if (!ts_node_is_null(decl))
                    {
                        std::vector<TSNode> declarators;
                        uint32_t dCount = ts_node_child_count(decl);
                        for (uint32_t i = 0; i < dCount; ++i)
                        {
                            TSNode child = ts_node_child(decl, i);
                            if (std::string_view(ts_node_type(child)) == "variable_declarator")
                            {
                                declarators.push_back(child);
                            }
                        }

                        if (declarators.size() <= 1)
                        {
                            // Single variable in declaration: delete entire line(s)
                            uint32_t lineCount = 1;
                            for (char c : request.sourceCode)
                            {
                                if (c == '\n')
                                {
                                    lineCount++;
                                }
                            }

                            if (def->endLine + 1 < lineCount)
                            {
                                edit.range.start = lsp::Position{ def->startLine, 0 };
                                edit.range.end = lsp::Position{ def->endLine + 1, 0 };
                            }
                            else
                            {
                                edit.range.start = lsp::Position{ def->startLine, 0 };
                                size_t lastNewline = request.sourceCode.rfind('\n');
                                size_t lastLineLen = (lastNewline != std::string::npos) ? (request.sourceCode.size() - (lastNewline + 1)) : request.sourceCode.size();
                                edit.range.end = lsp::Position{ def->endLine, static_cast<uint32_t>(lastLineLen) };
                            }
                            edit.newText = "";
                        }
                        else
                        {
                            // Multi-variable declaration: remove declarator and comma
                            size_t targetIdx = 0;
                            for (size_t k = 0; k < declarators.size(); ++k)
                            {
                                if (ts_node_start_byte(declarators[k]) == ts_node_start_byte(declarator))
                                {
                                    targetIdx = k;
                                    break;
                                }
                            }

                            if (targetIdx == 0 && declarators.size() > 1)
                            {
                                TSPoint startPt = ts_node_start_point(declarators[0]);
                                TSPoint endPt = ts_node_start_point(declarators[1]);
                                edit.range.start = lsp::Position{ startPt.row, startPt.column };
                                edit.range.end = lsp::Position{ endPt.row, endPt.column };
                            }
                            else
                            {
                                TSPoint startPt = ts_node_end_point(declarators[targetIdx - 1]);
                                TSPoint endPt = ts_node_end_point(declarators[targetIdx]);
                                edit.range.start = lsp::Position{ startPt.row, startPt.column };
                                edit.range.end = lsp::Position{ endPt.row, endPt.column };
                            }
                            edit.newText = "";
                        }
                    }
                    else
                    {
                        // Fallback: delete declaration range line
                        edit.range.start = lsp::Position{ def->startLine, 0 };
                        edit.range.end = lsp::Position{ def->endLine + 1, 0 };
                        edit.newText = "";
                    }

                    lsp::CodeAction action;
                    action.title = "Remove unused variable '" + def->name + "'";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                    action.isPreferred = true;

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    std::vector<lsp::Diagnostic> matchingDiags;
                    for (const auto &diag : request.context.diagnostics)
                    {
                        if (diag.range.start.line <= def->endLine && diag.range.end.line >= def->startLine)
                        {
                            matchingDiags.push_back(diag);
                        }
                    }
                    if (!matchingDiags.empty())
                    {
                        action.diagnostics = std::move(matchingDiags);
                    }

                    actions.push_back(std::move(action));
                }
            }
        }

        // =========================================================================
        // Quick-Fix 2: Implement Missing Interface Methods in Implementing Classes
        // =========================================================================
        request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
        {
            for (const auto &clsSym : symbols)
            {
                if (clsSym.type != analysis::SymbolType::Class || clsSym.fileUri != request.uri)
                {
                    continue;
                }

                // Check if request range overlaps this class definition
                bool classMatchesRange = (request.range.start.line <= clsSym.endLine && request.range.end.line >= clsSym.startLine);
                if (!classMatchesRange)
                {
                    continue;
                }

                const auto &cls = clsSym.GetClass();
                for (const auto &baseName : cls.bases)
                {
                    std::string cleanIface = analysis::CleanBaseType(baseName);
                    if (cleanIface.empty())
                    {
                        continue;
                    }

                    bool isInterface = false;
                    std::vector<analysis::Symbol> ifaceMethods;
                    auto ifaceHierarchy = analysis::GetInheritedTypeHierarchy(cleanIface, request.symbolTable);
                    if (!ifaceHierarchy.empty())
                    {
                        isInterface = true;
                        for (const auto &ifaceName : ifaceHierarchy)
                        {
                            request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &mSyms)
                            {
                                for (const auto &m : mSyms)
                                {
                                    if (m.type == analysis::SymbolType::Function && m.containerName == ifaceName)
                                    {
                                        ifaceMethods.push_back(m);
                                    }
                                }
                            });
                        }
                    }

                    if (!isInterface || ifaceMethods.empty())
                    {
                        continue;
                    }

                    // Collect existing methods in the class
                    std::vector<analysis::Symbol> classMethods;
                    request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &mSyms)
                    {
                        for (const auto &m : mSyms)
                        {
                            if (m.type == analysis::SymbolType::Function && m.containerName == clsSym.name)
                            {
                                classMethods.push_back(m);
                            }
                        }
                    });

                    // Determine missing methods
                    std::vector<analysis::Symbol> missingMethods;
                    for (const auto &ifMethod : ifaceMethods)
                    {
                        bool implemented = false;
                        for (const auto &cMethod : classMethods)
                        {
                            if (cMethod.name == ifMethod.name)
                            {
                                if (cMethod.GetFunction().parameters.size() == ifMethod.GetFunction().parameters.size())
                                {
                                    implemented = true;
                                    break;
                                }
                            }
                        }
                        if (!implemented)
                        {
                            missingMethods.push_back(ifMethod);
                        }
                    }

                    if (missingMethods.empty())
                    {
                        continue;
                    }

                    // Generate method stubs
                    std::string stubs;
                    for (const auto &m : missingMethods)
                    {
                        const auto &fn = m.GetFunction();
                        std::string ret = fn.returnType.empty() ? "void" : fn.returnType;
                        stubs += "\n    " + ret + " " + m.name + "(";
                        for (size_t p = 0; p < fn.parameters.size(); ++p)
                        {
                            if (p > 0)
                            {
                                stubs += ", ";
                            }
                            const auto &param = fn.parameters[p];
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

                    // Locate insertion position: closing brace of class
                    lsp::Position insertPos{ clsSym.endLine, clsSym.endCharacter };
                    TSPoint cPt = { clsSym.startLine, clsSym.startCharacter };
                    TSNode cNode = ts_node_descendant_for_point_range(rootNode, cPt, cPt);
                    while (!ts_node_is_null(cNode) && std::string_view(ts_node_type(cNode)) != "class_declaration")
                    {
                        cNode = ts_node_parent(cNode);
                    }

                    if (!ts_node_is_null(cNode))
                    {
                        TSNode bodyNode = ts_node_child_by_field_name(cNode, "body", 4);
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
                                    insertPos = lsp::Position{ pt.row, pt.column };
                                    break;
                                }
                            }
                        }
                    }

                    lsp::TextEdit edit;
                    edit.range = lsp::Range{ insertPos, insertPos };
                    edit.newText = stubs;

                    lsp::CodeAction action;
                    action.title = "Implement missing interface methods for '" + cleanIface + "'";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                    action.isPreferred = true;

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)].push_back(std::move(edit));
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }
            }
        });

        if (actions.empty())
        {
            return std::nullopt;
        }

        return actions;
    }
}
