#include "features/code_action/CodeActionHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "analysis/DiagnosticCodes.h"
#include "utils/Utils.h"
#include <filesystem>

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <functional>
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
         * @brief Extracts leading whitespace indentation of a given 0-indexed line.
         */
        std::string GetLineIndentation(std::string_view sourceCode, uint32_t line)
        {
            uint32_t currLine = 0;
            size_t lineStart = 0;
            for (size_t i = 0; i < sourceCode.size(); ++i)
            {
                if (currLine == line)
                {
                    lineStart = i;
                    break;
                }
                if (sourceCode[i] == '\n')
                {
                    currLine++;
                }
            }
            if (currLine != line)
            {
                return "";
            }
            size_t i = lineStart;
            while (i < sourceCode.size() && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
            {
                i++;
            }
            return std::string(sourceCode.substr(lineStart, i - lineStart));
        }

        /**
         * @brief Locates the innermost lexical scope containing the given point.
         */
        /**
         * @brief Innermost scope by *line only*, falling back to `root` when nothing contains it.
         *
         * Deliberately not analysis::FindInnermostScope, and named apart from it so the difference
         * is visible rather than shadowed. Two things differ: the column is ignored, and a point
         * outside every scope yields the root instead of nullptr. The code actions built here work
         * on whole lines and want a scope to attach to even when the cursor sits between them, so
         * both differences are load-bearing.
         */
        const analysis::Scope *FindScopeByLineOrRoot(const analysis::Scope *root, uint32_t line, uint32_t character)
        {
            if (!root)
            {
                return nullptr;
            }
            for (const auto &child : root->children)
            {
                if (child->startLine <= line && child->endLine >= line)
                {
                    if (const analysis::Scope *deeper = FindScopeByLineOrRoot(child.get(), line, character))
                    {
                        return deeper;
                    }
                    return child.get();
                }
            }
            return root;
        }

        /**
         * @brief Recursively collects all identifier reference names across the scope hierarchy.
         */
        void CollectAllReferences(const analysis::Scope *scope, ankerl::unordered_dense::set<std::string> &refs)
        {
            if (!scope)
            {
                return;
            }
            for (const auto &ref : scope->references)
            {
                refs.insert(ref.name);
            }
            for (const auto &child : scope->children)
            {
                CollectAllReferences(child.get(), refs);
            }
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

        /**
         * @brief Checks whether an AST node is an extractable expression.
         */
        bool IsExtractableExpression(TSNode node)
        {
            if (ts_node_is_null(node))
            {
                return false;
            }
            std::string_view type = ts_node_type(node);
            return type == "binary_expression" ||
                   type == "unary_expression" ||
                   type == "postfix_expression" ||
                   type == "call_expression" ||
                   type == "member_expression" ||
                   type == "ternary_expression" ||
                   type == "cast_expression" ||
                   type == "functional_cast_expression" ||
                   type == "construct_call_expression" ||
                   type == "index_expression" ||
                   type == "parenthesized_expression" ||
                   type == "scoped_identifier" ||
                   type == "identifier" ||
                   type == "number_literal" ||
                   type == "string_literal" ||
                   type == "boolean_literal";
        }

        /**
         * @brief Checks whether an AST node is the left-hand side of an assignment.
         */
        bool IsLhsOfAssignment(TSNode node)
        {
            TSNode curr = node;
            while (!ts_node_is_null(curr))
            {
                TSNode parent = ts_node_parent(curr);
                if (!ts_node_is_null(parent))
                {
                    std::string_view pType = ts_node_type(parent);
                    if (pType == "assignment_expression")
                    {
                        TSNode left = ts_node_child_by_field_name(parent, "left", 4);
                        if (ts_node_start_byte(left) == ts_node_start_byte(curr) &&
                            ts_node_end_byte(left) == ts_node_end_byte(curr))
                        {
                            return true;
                        }
                    }
                }
                curr = parent;
            }
            return false;
        }

        /**
         * @brief Cleans property name by stripping m_ or _ prefix and capitalizing the first character.
         */
        std::string CleanPropertyName(std::string_view fieldName)
        {
            std::string prop(fieldName);
            if (prop.starts_with("m_") && prop.size() > 2)
            {
                prop = prop.substr(2);
            }
            else if (prop.starts_with("_") && prop.size() > 1)
            {
                prop = prop.substr(1);
            }
            if (!prop.empty())
            {
                prop[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(prop[0])));
            }
            return prop;
        }

        /**
         * @brief Checks if a method body mutates class fields or calls non-const methods on `this`.
         */
        bool MethodBodyMutatesClassState(
            TSNode bodyNode,
            TSNode classNode,
            std::string_view sourceCode,
            const analysis::SymbolTable &table,
            const std::string &className,
            const analysis::Scope *scope)
        {
            if (ts_node_is_null(bodyNode))
            {
                return false;
            }

            ankerl::unordered_dense::set<std::string> classFields;
            table.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
            {
                for (const auto &sym : symList)
                {
                    if (sym.containerName == className &&
                        (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property))
                    {
                        classFields.insert(sym.name);
                    }
                }
            });

            if (!ts_node_is_null(classNode))
            {
                TSNode cBody = ts_node_child_by_field_name(classNode, "body", 4);
                if (ts_node_is_null(cBody))
                {
                    uint32_t cnt = ts_node_child_count(classNode);
                    for (uint32_t i = 0; i < cnt; ++i)
                    {
                        TSNode ch = ts_node_child(classNode, i);
                        if (std::string_view(ts_node_type(ch)) == "class_body")
                        {
                            cBody = ch;
                            break;
                        }
                    }
                }

                if (!ts_node_is_null(cBody))
                {
                    uint32_t bCnt = ts_node_child_count(cBody);
                    for (uint32_t i = 0; i < bCnt; ++i)
                    {
                        TSNode ch = ts_node_child(cBody, i);
                        if (std::string_view(ts_node_type(ch)) == "variable_declaration")
                        {
                            uint32_t vCnt = ts_node_child_count(ch);
                            for (uint32_t j = 0; j < vCnt; ++j)
                            {
                                TSNode vCh = ts_node_child(ch, j);
                                if (std::string_view(ts_node_type(vCh)) == "variable_declarator")
                                {
                                    TSNode vNameNode = ts_node_child_by_field_name(vCh, "name", 4);
                                    std::string fName = GetNodeText(vNameNode, sourceCode);
                                    if (!fName.empty())
                                    {
                                        classFields.insert(fName);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            bool mutates = false;
            std::vector<TSNode> stack = { bodyNode };
            while (!stack.empty() && !mutates)
            {
                TSNode curr = stack.back();
                stack.pop_back();

                std::string_view type = ts_node_type(curr);

                if (type == "assignment_expression")
                {
                    TSNode left = ts_node_child_by_field_name(curr, "left", 4);
                    if (!ts_node_is_null(left))
                    {
                        std::string_view lType = ts_node_type(left);
                        if (lType == "identifier" || lType == "scoped_identifier")
                        {
                            std::string varName = GetNodeText(left, sourceCode);
                            TSPoint pt = ts_node_start_point(left);
                            const analysis::Scope *inner = FindScopeByLineOrRoot(scope, pt.row, pt.column);
                            const analysis::LocalDefinition *localDef = analysis::ResolveInScope(inner, varName);

                            bool isField = classFields.contains(varName);
                            if (localDef)
                            {
                                if (localDef->kind == analysis::LocalDefinitionKind::Field)
                                {
                                    isField = true;
                                }
                                else if (localDef->kind == analysis::LocalDefinitionKind::Variable ||
                                         localDef->kind == analysis::LocalDefinitionKind::Parameter)
                                {
                                    if (localDef->startLine >= ts_node_start_point(bodyNode).row &&
                                        localDef->endLine <= ts_node_end_point(bodyNode).row)
                                    {
                                        isField = false;
                                    }
                                }
                            }

                            if (isField)
                            {
                                mutates = true;
                                break;
                            }
                        }
                        else if (lType == "member_expression")
                        {
                            TSNode obj = ts_node_child_by_field_name(left, "object", 6);
                            TSNode mem = ts_node_child_by_field_name(left, "member", 6);
                            if (!ts_node_is_null(obj) && std::string_view(ts_node_type(obj)) == "this_expression")
                            {
                                std::string memName = GetNodeText(mem, sourceCode);
                                if (classFields.contains(memName))
                                {
                                    mutates = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                else if (type == "postfix_expression" || type == "unary_expression")
                {
                    TSNode opNode = ts_node_child_by_field_name(curr, "operator", 8);
                    std::string op = GetNodeText(opNode, sourceCode);
                    if (op == "++" || op == "--")
                    {
                        TSNode arg = ts_node_child_by_field_name(curr, "argument", 8);
                        if (!ts_node_is_null(arg))
                        {
                            std::string_view aType = ts_node_type(arg);
                            if (aType == "identifier" || aType == "scoped_identifier")
                            {
                                std::string varName = GetNodeText(arg, sourceCode);
                                TSPoint pt = ts_node_start_point(arg);
                                const analysis::Scope *inner = FindScopeByLineOrRoot(scope, pt.row, pt.column);
                                const analysis::LocalDefinition *localDef = analysis::ResolveInScope(inner, varName);

                                bool isField = classFields.contains(varName);
                                if (localDef)
                                {
                                    if (localDef->kind == analysis::LocalDefinitionKind::Field)
                                    {
                                        isField = true;
                                    }
                                    else if (localDef->kind == analysis::LocalDefinitionKind::Variable ||
                                             localDef->kind == analysis::LocalDefinitionKind::Parameter)
                                    {
                                        if (localDef->startLine >= ts_node_start_point(bodyNode).row &&
                                            localDef->endLine <= ts_node_end_point(bodyNode).row)
                                        {
                                            isField = false;
                                        }
                                    }
                                }

                                if (isField)
                                {
                                    mutates = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                else if (type == "call_expression")
                {
                    TSNode callee = ts_node_child_by_field_name(curr, "function", 8);
                    if (!ts_node_is_null(callee))
                    {
                        std::string_view cType = ts_node_type(callee);
                        std::string callMethodName;
                        bool isMemberOnThis = false;

                        if (cType == "identifier" || cType == "scoped_identifier")
                        {
                            callMethodName = GetNodeText(callee, sourceCode);
                            isMemberOnThis = true;
                        }
                        else if (cType == "member_expression")
                        {
                            TSNode obj = ts_node_child_by_field_name(callee, "object", 6);
                            TSNode mem = ts_node_child_by_field_name(callee, "member", 6);
                            if (!ts_node_is_null(obj) && std::string_view(ts_node_type(obj)) == "this_expression")
                            {
                                callMethodName = GetNodeText(mem, sourceCode);
                                isMemberOnThis = true;
                            }
                        }

                        if (isMemberOnThis && !callMethodName.empty())
                        {
                            table.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
                            {
                                for (const auto &sym : symList)
                                {
                                    if (sym.type == analysis::SymbolType::Function &&
                                        sym.containerName == className &&
                                        sym.name == callMethodName)
                                    {
                                        if (!sym.GetFunction().modifiers.isConst)
                                        {
                                            mutates = true;
                                        }
                                    }
                                }
                            });
                        }
                    }
                }

                uint32_t childCount = ts_node_child_count(curr);
                for (uint32_t i = 0; i < childCount; ++i)
                {
                    stack.push_back(ts_node_child(curr, i));
                }
            }

            return mutates;
        }

        /**
         * @brief Tries to generate an Extract Variable refactoring code action.
         */
        void TryAddExtractVariableAction(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            TSPoint startPt = { request.range.start.line, request.range.start.character };
            TSPoint endPt = { request.range.end.line, request.range.end.character };
            TSNode targetNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
            if (ts_node_is_null(targetNode))
            {
                return;
            }

            while (!ts_node_is_null(targetNode) && !IsExtractableExpression(targetNode))
            {
                TSNode p = ts_node_parent(targetNode);
                if (ts_node_is_null(p))
                {
                    break;
                }
                targetNode = p;
            }

            if (ts_node_is_null(targetNode) || !IsExtractableExpression(targetNode))
            {
                return;
            }

            std::string_view nodeType = ts_node_type(targetNode);
            if (nodeType.ends_with("_statement") || nodeType.ends_with("_declaration") ||
                nodeType == "statement_block" || nodeType == "class_body" ||
                nodeType == "parameter" || nodeType == "type_specifier" || nodeType == "primitive_type")
            {
                return;
            }

            if (IsLhsOfAssignment(targetNode))
            {
                return;
            }

            TSNode stmtNode = targetNode;
            while (!ts_node_is_null(stmtNode))
            {
                TSNode parent = ts_node_parent(stmtNode);
                if (!ts_node_is_null(parent))
                {
                    std::string_view pType = ts_node_type(parent);
                    if (pType == "statement_block")
                    {
                        break;
                    }
                }
                stmtNode = parent;
            }

            if (ts_node_is_null(stmtNode) || ts_node_is_null(ts_node_parent(stmtNode)))
            {
                return;
            }

            auto rootScope = request.scopeIndex.GetRoot(request.uri);
            TSPoint exprStart = ts_node_start_point(targetNode);
            TSPoint exprEnd = ts_node_end_point(targetNode);
            const analysis::Scope *scope = FindScopeByLineOrRoot(rootScope.get(), exprStart.row, exprStart.column);

            std::string varType = analysis::ResolveExpressionType(targetNode, scope, request.symbolTable, request.sourceCode, request.uri);
            if (varType.empty() || varType == "null")
            {
                varType = "auto";
            }

            std::string varName = "newVar";
            std::string exprText = GetNodeText(targetNode, request.sourceCode);
            if (exprText.empty())
            {
                return;
            }

            uint32_t stmtRow = ts_node_start_point(stmtNode).row;
            std::string indent = GetLineIndentation(request.sourceCode, stmtRow);

            lsp::TextEdit declEdit;
            declEdit.range = lsp::Range{ { stmtRow, 0 }, { stmtRow, 0 } };
            declEdit.newText = indent + varType + " " + varName + " = " + exprText + ";\n";

            lsp::TextEdit replEdit;
            replEdit.range = lsp::Range{ { exprStart.row, exprStart.column }, { exprEnd.row, exprEnd.column } };
            replEdit.newText = varName;

            lsp::CodeAction action;
            action.title = "Extract Variable";
            action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);

            lsp::WorkspaceEdit wsEdit;
            lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
            changes[lsp::DocumentUri::parse(request.uri)] = { std::move(declEdit), std::move(replEdit) };
            wsEdit.changes = std::move(changes);
            action.edit = std::move(wsEdit);

            actions.push_back(std::move(action));
        }

        /**
         * @brief Tries to generate an Extract Method refactoring code action.
         */
        void TryAddExtractMethodAction(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            TSPoint startPt = { request.range.start.line, request.range.start.character };
            TSPoint endPt = { request.range.end.line, request.range.end.character };
            TSNode selNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
            if (ts_node_is_null(selNode))
            {
                return;
            }

            TSNode fnNode = selNode;
            while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
            {
                fnNode = ts_node_parent(fnNode);
            }
            if (ts_node_is_null(fnNode))
            {
                return;
            }

            TSNode bodyNode = ts_node_child_by_field_name(fnNode, "body", 4);
            if (ts_node_is_null(bodyNode))
            {
                uint32_t cnt = ts_node_child_count(fnNode);
                for (uint32_t i = 0; i < cnt; ++i)
                {
                    TSNode ch = ts_node_child(fnNode, i);
                    if (std::string_view(ts_node_type(ch)) == "statement_block")
                    {
                        bodyNode = ch;
                        break;
                    }
                }
            }
            if (ts_node_is_null(bodyNode))
            {
                return;
            }

            std::vector<TSNode> selectedStmts;
            uint32_t childCount = ts_node_child_count(bodyNode);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode ch = ts_node_child(bodyNode, i);
                if (!ts_node_is_named(ch))
                {
                    continue;
                }
                std::string_view t = ts_node_type(ch);
                if (t == "{" || t == "}")
                {
                    continue;
                }
                TSPoint cStart = ts_node_start_point(ch);
                TSPoint cEnd = ts_node_end_point(ch);
                if (cStart.row <= request.range.end.line && cEnd.row >= request.range.start.line)
                {
                    selectedStmts.push_back(ch);
                }
            }

            if (selectedStmts.empty())
            {
                return;
            }

            TSNode firstStmt = selectedStmts.front();
            TSNode lastStmt = selectedStmts.back();
            TSPoint firstStart = ts_node_start_point(firstStmt);
            TSPoint lastEnd = ts_node_end_point(lastStmt);

            uint32_t startByte = ts_node_start_byte(firstStmt);
            uint32_t endByte = ts_node_end_byte(lastStmt);
            if (startByte >= request.sourceCode.size() || endByte > request.sourceCode.size() || startByte >= endByte)
            {
                return;
            }
            std::string selectedCode = request.sourceCode.substr(startByte, endByte - startByte);

            TSNode classNode = fnNode;
            while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
            {
                classNode = ts_node_parent(classNode);
            }

            auto rootScope = request.scopeIndex.GetRoot(request.uri);
            const analysis::Scope *fnScope = FindScopeByLineOrRoot(rootScope.get(), ts_node_start_point(fnNode).row, ts_node_start_point(fnNode).column);

            struct VarInfo
            {
                std::string name;
                std::string typeName;
                bool declaredInside = false;
            };

            std::vector<VarInfo> inputParams;
            std::vector<VarInfo> outputVars;
            ankerl::unordered_dense::set<std::string> seenInputs;
            ankerl::unordered_dense::set<std::string> seenOutputs;

            if (fnScope)
            {
                std::function<void(const analysis::Scope *)> scanScope = [&](const analysis::Scope *sc)
                {
                    if (!sc) return;
                    for (const auto &ref : sc->references)
                    {
                        if (ref.isMemberAccess) continue;
                        if (ref.startLine >= firstStart.row && ref.endLine <= lastEnd.row)
                        {
                            const analysis::LocalDefinition *def = analysis::ResolveInScope(sc, ref.name);
                            if (def)
                            {
                                if (!ts_node_is_null(classNode) && def->kind == analysis::LocalDefinitionKind::Field)
                                {
                                    continue;
                                }
                                if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
                                {
                                    continue;
                                }

                                if (def->endLine < firstStart.row || (def->endLine == firstStart.row && def->endCharacter <= firstStart.column) || def->kind == analysis::LocalDefinitionKind::Parameter)
                                {
                                    if (!seenInputs.contains(def->name))
                                     {
                                        seenInputs.insert(def->name);
                                        std::string tName = def->typeName.empty() ? "auto" : def->typeName;
                                        inputParams.push_back({ def->name, tName, false });
                                    }
                                }
                            }
                        }
                    }

                    for (const auto &child : sc->children)
                    {
                        scanScope(child.get());
                    }
                };
                scanScope(fnScope);

                // Collect mutated variables inside selected statements
                ankerl::unordered_dense::set<std::string> mutatedVars;
                auto inspectMutations = [&](TSNode node)
                {
                    std::vector<TSNode> stack = { node };
                    while (!stack.empty())
                    {
                        TSNode curr = stack.back();
                        stack.pop_back();

                        std::string_view type = ts_node_type(curr);

                        if (type == "assignment_expression")
                        {
                            TSNode left = ts_node_child_by_field_name(curr, "left", 4);
                            if (ts_node_is_null(left) && ts_node_child_count(curr) > 0)
                            {
                                left = ts_node_child(curr, 0);
                            }
                            while (!ts_node_is_null(left) && std::string_view(ts_node_type(left)) == "parenthesized_expression")
                            {
                                if (ts_node_named_child_count(left) > 0)
                                {
                                    left = ts_node_named_child(left, 0);
                                }
                                else
                                {
                                    break;
                                }
                            }
                            if (!ts_node_is_null(left))
                            {
                                std::string_view lType = ts_node_type(left);
                                if (lType == "identifier")
                                {
                                    std::string varName = GetNodeText(left, request.sourceCode);
                                    if (!varName.empty())
                                    {
                                        mutatedVars.insert(varName);
                                    }
                                }
                                else if (lType == "scoped_identifier")
                                {
                                    if (ts_node_named_child_count(left) == 1)
                                    {
                                        std::string varName = GetNodeText(ts_node_named_child(left, 0), request.sourceCode);
                                        if (!varName.empty())
                                        {
                                            mutatedVars.insert(varName);
                                        }
                                    }
                                }
                            }
                        }
                        else if (type == "postfix_expression" || type == "unary_expression" || type == "update_expression")
                        {
                            TSNode opNode = ts_node_child_by_field_name(curr, "operator", 8);
                            std::string op = !ts_node_is_null(opNode) ? GetNodeText(opNode, request.sourceCode) : "";
                            bool isIncDec = (op == "++" || op == "--");
                            TSNode targetArg = ts_node_child_by_field_name(curr, "argument", 8);
                            if (!isIncDec || ts_node_is_null(targetArg))
                            {
                                uint32_t cnt = ts_node_child_count(curr);
                                for (uint32_t i = 0; i < cnt; ++i)
                                {
                                    TSNode ch = ts_node_child(curr, i);
                                    std::string chText = GetNodeText(ch, request.sourceCode);
                                    if (chText == "++" || chText == "--")
                                    {
                                        isIncDec = true;
                                    }
                                    else if (ts_node_is_named(ch))
                                    {
                                        targetArg = ch;
                                    }
                                }
                            }
                            if (isIncDec && !ts_node_is_null(targetArg))
                            {
                                while (!ts_node_is_null(targetArg) && std::string_view(ts_node_type(targetArg)) == "parenthesized_expression")
                                {
                                    if (ts_node_named_child_count(targetArg) > 0)
                                    {
                                        targetArg = ts_node_named_child(targetArg, 0);
                                    }
                                    else
                                    {
                                        break;
                                    }
                                }
                                if (!ts_node_is_null(targetArg))
                                {
                                    std::string_view aType = ts_node_type(targetArg);
                                    if (aType == "identifier")
                                    {
                                        std::string varName = GetNodeText(targetArg, request.sourceCode);
                                        if (!varName.empty())
                                        {
                                            mutatedVars.insert(varName);
                                        }
                                    }
                                    else if (aType == "scoped_identifier" && ts_node_named_child_count(targetArg) == 1)
                                    {
                                        std::string varName = GetNodeText(ts_node_named_child(targetArg, 0), request.sourceCode);
                                        if (!varName.empty())
                                        {
                                            mutatedVars.insert(varName);
                                        }
                                    }
                                }
                            }
                        }
                        else if (type == "call_expression")
                        {
                            TSNode argsNode = ts_node_child_by_field_name(curr, "arguments", 9);
                            if (!ts_node_is_null(argsNode))
                            {
                                uint32_t argCnt = ts_node_named_child_count(argsNode);
                                for (uint32_t i = 0; i < argCnt; ++i)
                                {
                                    TSNode arg = ts_node_named_child(argsNode, i);
                                    std::string argText = GetNodeText(arg, request.sourceCode);
                                    if (argText.starts_with("&out ") || argText.starts_with("&inout ") || argText.starts_with("out ") || argText.starts_with("inout "))
                                    {
                                        TSNode idNode = ts_node_child_by_field_name(arg, "name", 4);
                                        if (ts_node_is_null(idNode)) idNode = ts_node_child_by_field_name(arg, "argument", 8);
                                        if (ts_node_is_null(idNode) && ts_node_named_child_count(arg) > 0) idNode = ts_node_named_child(arg, ts_node_named_child_count(arg) - 1);
                                        if (!ts_node_is_null(idNode))
                                        {
                                            std::string varName = GetNodeText(idNode, request.sourceCode);
                                            if (!varName.empty()) mutatedVars.insert(varName);
                                        }
                                    }
                                }
                            }
                        }

                        uint32_t childCount = ts_node_child_count(curr);
                        for (uint32_t i = 0; i < childCount; ++i)
                        {
                            stack.push_back(ts_node_child(curr, i));
                        }
                    }
                };

                for (const auto &stmt : selectedStmts)
                {
                    inspectMutations(stmt);
                }

                // Check mutated pre-declared variables for usage after selection
                const analysis::Scope *stmtScope = FindScopeByLineOrRoot(rootScope.get(), firstStart.row, firstStart.column);
                for (const auto &mName : mutatedVars)
                {
                    const analysis::LocalDefinition *def = nullptr;
                    if (stmtScope)
                    {
                        def = analysis::ResolveInScope(stmtScope, mName);
                    }
                    if (!def && fnScope)
                    {
                        def = analysis::ResolveInScope(fnScope, mName);
                    }
                    if (!def && fnScope)
                    {
                        std::function<void(const analysis::Scope *)> findDef = [&](const analysis::Scope *s)
                        {
                            if (!s || def) return;
                            for (const auto &d : s->definitions)
                            {
                                if (d.name == mName)
                                {
                                    def = &d;
                                    return;
                                }
                            }
                            for (const auto &c : s->children)
                            {
                                findDef(c.get());
                            }
                        };
                        findDef(fnScope);
                    }

                    if (def)
                    {
                        if (!ts_node_is_null(classNode) && def->kind == analysis::LocalDefinitionKind::Field)
                        {
                            continue;
                        }
                        if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
                        {
                            continue;
                        }

                        bool declaredBefore = (def->endLine < firstStart.row || (def->endLine == firstStart.row && def->endCharacter <= firstStart.column) || def->kind == analysis::LocalDefinitionKind::Parameter);
                        if (declaredBefore)
                        {
                            bool usedAfter = false;
                            std::function<void(const analysis::Scope *)> checkUsed = [&](const analysis::Scope *s)
                            {
                                if (!s || usedAfter) return;
                                for (const auto &r : s->references)
                                {
                                    if (!r.isMemberAccess && r.name == def->name)
                                    {
                                        if (r.startLine > lastEnd.row || (r.startLine == lastEnd.row && r.startCharacter >= lastEnd.column))
                                        {
                                            usedAfter = true;
                                            break;
                                        }
                                    }
                                }
                                for (const auto &c : s->children)
                                {
                                    checkUsed(c.get());
                                }
                            };
                            checkUsed(fnScope);

                            if (usedAfter && !seenOutputs.contains(def->name))
                            {
                                seenOutputs.insert(def->name);
                                std::string tName = def->typeName.empty() ? "auto" : def->typeName;
                                outputVars.push_back({ def->name, tName, false });
                            }
                        }
                    }
                }

                // Also check definitions declared inside selection for usage after selection
                std::function<void(const analysis::Scope *)> scanDefsInside = [&](const analysis::Scope *sc)
                {
                    if (!sc) return;
                    for (const auto &def : sc->definitions)
                    {
                        if (def.startLine >= firstStart.row && def.endLine <= lastEnd.row)
                        {
                            bool usedAfter = false;
                            std::function<void(const analysis::Scope *)> checkUsed = [&](const analysis::Scope *s)
                            {
                                if (!s || usedAfter) return;
                                for (const auto &r : s->references)
                                {
                                    if (!r.isMemberAccess && r.name == def.name && r.startLine > lastEnd.row)
                                    {
                                        usedAfter = true;
                                        break;
                                    }
                                }
                                for (const auto &c : s->children)
                                {
                                    checkUsed(c.get());
                                }
                            };
                            checkUsed(fnScope);

                            if (usedAfter && !seenOutputs.contains(def.name))
                            {
                                seenOutputs.insert(def.name);
                                std::string tName = def.typeName.empty() ? "auto" : def.typeName;
                                outputVars.push_back({ def.name, tName, true });
                            }
                        }
                    }

                    for (const auto &child : sc->children)
                    {
                        scanDefsInside(child.get());
                    }
                };
                scanDefsInside(fnScope);
            }

            std::string methodName = "NewMethod";

            std::vector<VarInfo> effectiveInputs;
            for (const auto &inp : inputParams)
            {
                bool isOutParam = false;
                for (size_t k = 1; k < outputVars.size(); ++k)
                {
                    if (outputVars[k].name == inp.name)
                    {
                        isOutParam = true;
                        break;
                    }
                }
                if (!isOutParam)
                {
                    effectiveInputs.push_back(inp);
                }
            }

            std::string paramsStr;
            std::string argsStr;
            for (size_t i = 0; i < effectiveInputs.size(); ++i)
            {
                if (i > 0)
                {
                    paramsStr += ", ";
                    argsStr += ", ";
                }
                paramsStr += effectiveInputs[i].typeName + " " + effectiveInputs[i].name;
                argsStr += effectiveInputs[i].name;
            }

            std::string returnType = "void";
            std::string callSiteText;
            std::string extractedBody = selectedCode;

            if (outputVars.size() == 1)
            {
                returnType = outputVars[0].typeName;
                if (extractedBody.find("return " + outputVars[0].name) == std::string::npos &&
                    !extractedBody.ends_with("return " + outputVars[0].name + ";"))
                {
                    extractedBody += "\n    return " + outputVars[0].name + ";";
                }
                if (outputVars[0].declaredInside)
                {
                    callSiteText = returnType + " " + outputVars[0].name + " = " + methodName + "(" + argsStr + ");";
                }
                else
                {
                    callSiteText = outputVars[0].name + " = " + methodName + "(" + argsStr + ");";
                }
            }
            else if (outputVars.empty())
            {
                TSNode lastNode = selectedStmts.back();
                if (std::string_view(ts_node_type(lastNode)) == "return_statement")
                {
                    TSNode retTypeNode = ts_node_child_by_field_name(fnNode, "type", 4);
                    if (!ts_node_is_null(retTypeNode))
                    {
                        returnType = GetNodeText(retTypeNode, request.sourceCode);
                    }
                }
                callSiteText = methodName + "(" + argsStr + ");";
            }
            else
            {
                returnType = outputVars[0].typeName;
                for (size_t i = 1; i < outputVars.size(); ++i)
                {
                    if (!paramsStr.empty()) paramsStr += ", ";
                    if (!argsStr.empty()) argsStr += ", ";
                    paramsStr += outputVars[i].typeName + " &out " + outputVars[i].name;
                    argsStr += outputVars[i].name;
                }
                if (extractedBody.find("return " + outputVars[0].name) == std::string::npos &&
                    !extractedBody.ends_with("return " + outputVars[0].name + ";"))
                {
                    extractedBody += "\n    return " + outputVars[0].name + ";";
                }
                if (outputVars[0].declaredInside)
                {
                    callSiteText = returnType + " " + outputVars[0].name + " = " + methodName + "(" + argsStr + ");";
                }
                else
                {
                    callSiteText = outputVars[0].name + " = " + methodName + "(" + argsStr + ");";
                }
            }

            lsp::TextEdit methodDefEdit;
            if (!ts_node_is_null(classNode))
            {
                TSNode classBody = ts_node_child_by_field_name(classNode, "body", 4);
                if (ts_node_is_null(classBody))
                {
                    uint32_t cnt = ts_node_child_count(classNode);
                    for (uint32_t i = 0; i < cnt; ++i)
                    {
                        TSNode ch = ts_node_child(classNode, i);
                        if (std::string_view(ts_node_type(ch)) == "class_body")
                        {
                            classBody = ch;
                            break;
                        }
                    }
                }

                TSPoint insertPt = { 0, 0 };
                if (!ts_node_is_null(classBody))
                {
                    uint32_t cnt = ts_node_child_count(classBody);
                    for (int i = static_cast<int>(cnt) - 1; i >= 0; --i)
                    {
                        TSNode ch = ts_node_child(classBody, static_cast<uint32_t>(i));
                        if (std::string_view(ts_node_type(ch)) == "}")
                        {
                            insertPt = ts_node_start_point(ch);
                            break;
                        }
                    }
                }

                std::string methodCode = "\n    " + returnType + " " + methodName + "(" + paramsStr + ")\n    {\n        " + extractedBody + "\n    }\n";
                methodDefEdit.range = lsp::Range{ { insertPt.row, insertPt.column }, { insertPt.row, insertPt.column } };
                methodDefEdit.newText = methodCode;
            }
            else
            {
                TSPoint fnEnd = ts_node_end_point(fnNode);
                std::string methodCode = "\n\n" + returnType + " " + methodName + "(" + paramsStr + ")\n{\n    " + extractedBody + "\n}\n";
                methodDefEdit.range = lsp::Range{ { fnEnd.row + 1, 0 }, { fnEnd.row + 1, 0 } };
                methodDefEdit.newText = methodCode;
            }

            lsp::TextEdit callEdit;
            callEdit.range = lsp::Range{ { firstStart.row, firstStart.column }, { lastEnd.row, lastEnd.column } };
            callEdit.newText = callSiteText;

            lsp::CodeAction action;
            action.title = "Extract Method";
            action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);

            lsp::WorkspaceEdit wsEdit;
            lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
            changes[lsp::DocumentUri::parse(request.uri)] = { std::move(callEdit), std::move(methodDefEdit) };
            wsEdit.changes = std::move(changes);
            action.edit = std::move(wsEdit);

            actions.push_back(std::move(action));
        }

        /**
         * @brief Tries to generate Getters and Setters code actions for class fields.
         */
        void TryAddGetterSetterActions(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            TSPoint pt = { request.range.start.line, request.range.start.character };
            TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
            if (ts_node_is_null(leaf))
            {
                return;
            }

            TSNode classNode = leaf;
            while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
            {
                classNode = ts_node_parent(classNode);
            }
            if (ts_node_is_null(classNode))
            {
                return;
            }

            TSNode classBody = ts_node_child_by_field_name(classNode, "body", 4);
            if (ts_node_is_null(classBody))
            {
                uint32_t cnt = ts_node_child_count(classNode);
                for (uint32_t i = 0; i < cnt; ++i)
                {
                    TSNode ch = ts_node_child(classNode, i);
                    if (std::string_view(ts_node_type(ch)) == "class_body")
                    {
                        classBody = ch;
                        break;
                    }
                }
            }
            if (ts_node_is_null(classBody))
            {
                return;
            }

            TSNode nameNode = ts_node_child_by_field_name(classNode, "name", 4);
            std::string className = GetNodeText(nameNode, request.sourceCode);

            TSNode varDecl = leaf;
            while (!ts_node_is_null(varDecl) &&
                   std::string_view(ts_node_type(varDecl)) != "variable_declaration" &&
                   varDecl.id != classBody.id)
            {
                varDecl = ts_node_parent(varDecl);
            }

            std::vector<std::pair<std::string, std::string>> fields;

            if (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) == "variable_declaration")
            {
                TSNode typeNode = ts_node_child_by_field_name(varDecl, "var_type", 8);
                if (ts_node_is_null(typeNode))
                {
                    typeNode = ts_node_child_by_field_name(varDecl, "type", 4);
                }
                std::string fieldType = GetNodeText(typeNode, request.sourceCode);
                if (fieldType.empty())
                {
                    fieldType = "int";
                }

                uint32_t dCnt = ts_node_child_count(varDecl);
                for (uint32_t i = 0; i < dCnt; ++i)
                {
                    TSNode ch = ts_node_child(varDecl, i);
                    if (std::string_view(ts_node_type(ch)) == "variable_declarator")
                    {
                        TSNode vNameNode = ts_node_child_by_field_name(ch, "name", 4);
                        std::string fName = GetNodeText(vNameNode, request.sourceCode);
                        if (!fName.empty())
                        {
                            fields.push_back({ fName, fieldType });
                        }
                    }
                }
            }
            else
            {
                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
                {
                    for (const auto &sym : symList)
                    {
                        if (sym.containerName == className &&
                            (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property))
                        {
                            fields.push_back({ sym.name, sym.GetVariable().typeName });
                        }
                    }
                });
            }

            if (fields.empty())
            {
                return;
            }

            TSPoint insertPt = { 0, 0 };
            uint32_t cnt = ts_node_child_count(classBody);
            for (int i = static_cast<int>(cnt) - 1; i >= 0; --i)
            {
                TSNode ch = ts_node_child(classBody, static_cast<uint32_t>(i));
                if (std::string_view(ts_node_type(ch)) == "}")
                {
                    insertPt = ts_node_start_point(ch);
                    break;
                }
            }

            for (const auto &[fName, fType] : fields)
            {
                std::string propName = CleanPropertyName(fName);
                if (propName.empty()) continue;

                std::string getterName = "get_" + propName;
                std::string setterName = "set_" + propName;

                bool hasGetter = false;
                bool hasSetter = false;

                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
                {
                    for (const auto &sym : symList)
                    {
                        if (sym.containerName == className && sym.type == analysis::SymbolType::Function)
                        {
                            if (sym.name == getterName) hasGetter = true;
                            if (sym.name == setterName) hasSetter = true;
                        }
                    }
                });

                if (hasGetter && hasSetter)
                {
                    continue;
                }

                std::string cleanType = fType.empty() ? "int" : fType;
                bool isPassByValue = analysis::IsPrimitiveTypeName(cleanType) || cleanType.ends_with("@");
                std::string setterParamType = isPassByValue ? cleanType : ("const " + cleanType + " &in");

                std::string getterCode = "    " + cleanType + " " + getterName + "() const\n    {\n        return " + fName + ";\n    }\n";
                std::string setterCode = "    void " + setterName + "(" + setterParamType + " value)\n    {\n        " + fName + " = value;\n    }\n";

                if (!hasGetter)
                {
                    lsp::CodeAction action;
                    action.title = "Generate Getter";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

                    lsp::TextEdit edit;
                    edit.range = lsp::Range{ { insertPt.row, insertPt.column }, { insertPt.row, insertPt.column } };
                    edit.newText = "\n" + getterCode;

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }

                if (!hasSetter)
                {
                    lsp::CodeAction action;
                    action.title = "Generate Setter";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

                    lsp::TextEdit edit;
                    edit.range = lsp::Range{ { insertPt.row, insertPt.column }, { insertPt.row, insertPt.column } };
                    edit.newText = "\n" + setterCode;

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }

                if (!hasGetter && !hasSetter)
                {
                    lsp::CodeAction action;
                    action.title = "Generate Getter and Setter";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

                    lsp::TextEdit edit;
                    edit.range = lsp::Range{ { insertPt.row, insertPt.column }, { insertPt.row, insertPt.column } };
                    edit.newText = "\n" + getterCode + "\n" + setterCode;

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }
            }
        }

        bool MatchDiagnosticCode(const lsp::Diagnostic &diag, std::string_view expectedCode)
        {
            if (!diag.code.has_value())
            {
                return false;
            }
            if (std::holds_alternative<lsp::String>(diag.code.value()))
            {
                return std::get<lsp::String>(diag.code.value()) == expectedCode;
            }
            return false;
        }

        /**
         * @brief Edit distance between two names, abandoned as soon as it passes `limit`.
         *
         * Bounded rather than exact because the answer is only ever compared against a threshold:
         * once every cell of a row is over the limit no later row can come back under it, so the
         * remaining work cannot change the verdict. That matters here - this runs against every
         * name in the workspace symbol table.
         */
        size_t BoundedEditDistance(std::string_view a, std::string_view b, size_t limit)
        {
            if (a.size() > b.size())
            {
                std::swap(a, b);
            }

            // Length alone already settles it.
            if (b.size() - a.size() > limit)
            {
                return limit + 1;
            }

            std::vector<size_t> previous(a.size() + 1);
            std::vector<size_t> current(a.size() + 1);
            for (size_t i = 0; i <= a.size(); ++i)
            {
                previous[i] = i;
            }

            for (size_t j = 1; j <= b.size(); ++j)
            {
                current[0] = j;
                size_t rowBest = current[0];
                for (size_t i = 1; i <= a.size(); ++i)
                {
                    const size_t substitution = previous[i - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
                    current[i] = std::min({ substitution, previous[i] + 1, current[i - 1] + 1 });
                    rowBest = std::min(rowBest, current[i]);
                }

                if (rowBest > limit)
                {
                    return limit + 1;
                }
                previous.swap(current);
            }

            return previous[a.size()];
        }

        /**
         * @brief How far a suggestion may sit from what the user typed, by name length.
         *
         * Deliberately tight. A wrong suggestion is worse than none: the user reads "did you mean"
         * as the server knowing something, and two edits away from a three-letter name is not a
         * typo, it is a different name. Zero is still useful - the comparison is case-folded, so a
         * limit of zero catches `myvar` for `myVar`, which is the single most common miss.
         */
        size_t SuggestionLimit(size_t nameLength)
        {
            if (nameLength < 3)
            {
                return 0;
            }
            if (nameLength < 5)
            {
                return 1;
            }
            if (nameLength < 8)
            {
                return 2;
            }
            return 3;
        }

        std::string FoldCase(std::string_view text)
        {
            std::string folded(text);
            std::transform(folded.begin(), folded.end(), folded.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return folded;
        }

        /** @brief Every name declared in the scope chain enclosing a point. */
        void CollectVisibleLocalNames(const analysis::Scope *scope, std::vector<std::string> &names)
        {
            for (const analysis::Scope *walk = scope; walk != nullptr; walk = walk->parent)
            {
                for (const auto &def : walk->definitions)
                {
                    names.push_back(def.name);
                }
            }
        }

        /** @brief True for the symbol kinds that can legally stand where a type name was written. */
        bool IsTypeLikeSymbol(analysis::SymbolType type)
        {
            return type == analysis::SymbolType::Class
                || type == analysis::SymbolType::Interface
                || type == analysis::SymbolType::Enum
                || type == analysis::SymbolType::Typedef
                || type == analysis::SymbolType::Funcdef;
        }

        /**
         * @brief "Did you mean 'X'?" for a name that resolved to nothing, when something is close.
         *
         * Of the 132 codes this server can emit, one had a quick fix before this. These two are
         * what a typo looks like - an identifier or a type name that resolved to nothing - and the
         * name the user meant is nearly always already in the symbol table.
         *
         * The candidate set differs by which one it is. An unresolved TYPE is only ever a type, so
         * offering a function name there would be a suggestion that cannot compile; an unresolved
         * identifier can be anything in scope. Everything else about the two is the same.
         *
         * Offers nothing when nothing is close. That is the whole design - see SuggestionLimit.
         */
        void TryAddUndefinedIdentifierSuggestions(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            for (const auto &diag : request.context.diagnostics)
            {
                const bool isIdentifier = MatchDiagnosticCode(diag, diagnostics::codes::UndefinedIdentifier);
                const bool isType = MatchDiagnosticCode(diag, diagnostics::codes::UnknownType);
                if (!isIdentifier && !isType)
                {
                    continue;
                }

                const TSPoint point = { diag.range.start.line, diag.range.start.character };
                TSNode identifier = ts_node_descendant_for_point_range(rootNode, point, point);
                if (ts_node_is_null(identifier))
                {
                    continue;
                }

                const std::string typed = GetNodeText(identifier, request.sourceCode);
                if (typed.empty())
                {
                    continue;
                }

                const std::string typedFolded = FoldCase(typed);
                const size_t limit = SuggestionLimit(typed.size());

                std::vector<std::string> candidates;

                // Locals and parameters first - they are what an identifier in a function body most
                // often meant. Not for a type: no local declares one.
                if (isIdentifier)
                {
                    auto rootScope = request.scopeIndex.GetRoot(request.uri);
                    if (rootScope)
                    {
                        CollectVisibleLocalNames(FindScopeByLineOrRoot(rootScope.get(), point.row, point.column), candidates);
                    }
                }

                request.symbolTable.ForEachSymbol([&candidates, isType](const std::string &name, const std::vector<analysis::Symbol> &symbols)
                {
                    if (isType && std::none_of(symbols.begin(), symbols.end(),
                                               [](const analysis::Symbol &sym) { return IsTypeLikeSymbol(sym.type); }))
                    {
                        return;
                    }
                    candidates.push_back(name);
                });

                struct Suggestion
                {
                    std::string name;
                    size_t distance = 0;
                };

                std::vector<Suggestion> ranked;
                ankerl::unordered_dense::set<std::string> seen;

                for (const auto &candidate : candidates)
                {
                    if (candidate.empty() || candidate == typed || !seen.insert(candidate).second)
                    {
                        continue;
                    }

                    const size_t distance = BoundedEditDistance(typedFolded, FoldCase(candidate), limit);
                    if (distance <= limit)
                    {
                        ranked.push_back({ candidate, distance });
                    }
                }

                if (ranked.empty())
                {
                    continue;
                }

                std::sort(ranked.begin(), ranked.end(), [](const Suggestion &a, const Suggestion &b)
                {
                    if (a.distance != b.distance)
                    {
                        return a.distance < b.distance;
                    }
                    return a.name < b.name;
                });

                // Three at most. A list of near-misses is a menu to read, not a fix to accept.
                const size_t offered = std::min<size_t>(ranked.size(), 3);
                const bool hasClearWinner = ranked.size() == 1 || ranked[0].distance < ranked[1].distance;

                for (size_t i = 0; i < offered; ++i)
                {
                    lsp::CodeAction action;
                    action.title = "Did you mean '" + ranked[i].name + "'?";
                    action.kind = lsp::CodeActionKind::QuickFix;
                    action.diagnostics = std::vector<lsp::Diagnostic>{ diag };

                    // Only when one candidate is strictly closer than the rest. Marking a preferred
                    // fix is what lets an editor apply it without asking, so a tie must not have one.
                    if (i == 0 && hasClearWinner)
                    {
                        action.isPreferred = true;
                    }

                    lsp::TextEdit edit;
                    edit.range.start.line = ts_node_start_point(identifier).row;
                    edit.range.start.character = ts_node_start_point(identifier).column;
                    edit.range.end.line = ts_node_end_point(identifier).row;
                    edit.range.end.character = ts_node_end_point(identifier).column;
                    edit.newText = ranked[i].name;

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }
            }
        }

        /**
         * @brief Drops the `@` from a handle declared on a primitive.
         *
         * AngelScript has no handle to a primitive - `int@` is not a type that exists - so there is
         * exactly one thing the user can have meant, and the fix is to delete one character. Worth
         * having precisely because it is unambiguous: the diagnostic already says what is wrong and
         * the reader still has to go and edit it by hand.
         */
        void TryAddHandleOnPrimitiveFix(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            for (const auto &diag : request.context.diagnostics)
            {
                if (!MatchDiagnosticCode(diag, diagnostics::codes::HandleOnPrimitive))
                {
                    continue;
                }

                // Located in the source rather than trusted from the range: the diagnostic points at
                // the type, and the `@` may sit anywhere across it (`int@`, `int @`, `const int@`).
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
                action.diagnostics = std::vector<lsp::Diagnostic>{ diag };

                lsp::WorkspaceEdit wsEdit;
                lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                wsEdit.changes = std::move(changes);
                action.edit = std::move(wsEdit);

                actions.push_back(std::move(action));
            }
        }

        /**
         * @brief "Did you mean this file?" for an `#include` that resolves to nothing.
         *
         * The candidates are the files the server has actually indexed, which is the honest set: it
         * is what the server can see, and a file it has never heard of is one it cannot vouch for.
         * A moved file is the common case and lands at distance zero - the name is unchanged and
         * only the directory moved - so it sorts to the front on its own.
         *
         * The replacement is spelled relative to the including file with forward slashes, which is
         * how every `#include` in the corpus is written and what ResolveIncludePath reads back.
         */
        void TryAddUnresolvedIncludeSuggestions(
            const CodeActionRequest &request,
            std::vector<lsp::CodeAction> &actions)
        {
            if (request.sourceCode.empty())
            {
                return;
            }

            const bool anyUnresolvedInclude = std::any_of(
                request.context.diagnostics.begin(), request.context.diagnostics.end(),
                [](const lsp::Diagnostic &diag) { return MatchDiagnosticCode(diag, "as-warn-include-not-found"); });
            if (!anyUnresolvedInclude)
            {
                return;
            }

            const std::string includerPath = angel_lsp::utils::UriToPath(request.uri);
            if (includerPath.empty())
            {
                return;
            }

            // One entry per file, not per symbol: a file with four hundred declarations is still one
            // candidate.
            ankerl::unordered_dense::set<std::string> indexedUris;
            request.symbolTable.ForEachSymbol([&indexedUris](const std::string &, const std::vector<analysis::Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    if (!sym.fileUri.empty())
                    {
                        indexedUris.insert(sym.fileUri);
                    }
                }
            });

            const auto directives = angel_lsp::utils::IncludeResolver::ExtractIncludes(request.sourceCode);

            for (const auto &diag : request.context.diagnostics)
            {
                if (!MatchDiagnosticCode(diag, "as-warn-include-not-found"))
                {
                    continue;
                }

                const auto directive = std::find_if(directives.begin(), directives.end(),
                    [&diag](const angel_lsp::utils::IncludeDirective &candidate)
                    {
                        return candidate.line == diag.range.start.line;
                    });
                if (directive == directives.end() || directive->rawPath.empty())
                {
                    continue;
                }

                const std::string typedName = std::filesystem::path(directive->rawPath).filename().generic_string();
                if (typedName.empty())
                {
                    continue;
                }

                const std::string typedFolded = FoldCase(typedName);
                const size_t limit = SuggestionLimit(typedName.size());

                struct Candidate
                {
                    std::string spelling;
                    size_t distance = 0;
                };

                std::vector<Candidate> ranked;
                ankerl::unordered_dense::set<std::string> seen;

                for (const std::string &candidateUri : indexedUris)
                {
                    if (candidateUri == request.uri)
                    {
                        continue;
                    }

                    const std::string candidatePath = angel_lsp::utils::UriToPath(candidateUri);
                    if (candidatePath.empty())
                    {
                        continue;
                    }

                    const std::string candidateName = std::filesystem::path(candidatePath).filename().generic_string();
                    const size_t distance = BoundedEditDistance(typedFolded, FoldCase(candidateName), limit);
                    if (distance > limit)
                    {
                        continue;
                    }

                    std::error_code relativeError;
                    const std::filesystem::path relative = std::filesystem::relative(
                        std::filesystem::path(candidatePath),
                        std::filesystem::path(includerPath).parent_path(),
                        relativeError);
                    if (relativeError || relative.empty())
                    {
                        continue;
                    }

                    const std::string spelling = relative.generic_string();
                    if (spelling == directive->rawPath || !seen.insert(spelling).second)
                    {
                        continue;
                    }

                    ranked.push_back({ spelling, distance });
                }

                if (ranked.empty())
                {
                    continue;
                }

                std::sort(ranked.begin(), ranked.end(), [](const Candidate &a, const Candidate &b)
                {
                    if (a.distance != b.distance)
                    {
                        return a.distance < b.distance;
                    }
                    return a.spelling < b.spelling;
                });

                // The span inside the quotes or brackets, which is all that may be rewritten -
                // replacing the whole line would take the directive and any trailing comment with it.
                const std::string_view line = angel_lsp::utils::GetLine(request.sourceCode, static_cast<uint32_t>(directive->line));
                const char open = directive->isAngled ? '<' : '"';
                const char close = directive->isAngled ? '>' : '"';
                const size_t openPos = line.find(open);
                if (openPos == std::string_view::npos)
                {
                    continue;
                }
                const size_t closePos = line.find(close, openPos + 1);
                if (closePos == std::string_view::npos)
                {
                    continue;
                }

                const size_t offered = std::min<size_t>(ranked.size(), 3);
                const bool hasClearWinner = ranked.size() == 1 || ranked[0].distance < ranked[1].distance;

                for (size_t i = 0; i < offered; ++i)
                {
                    lsp::CodeAction action;
                    action.title = "Did you mean '" + ranked[i].spelling + "'?";
                    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                    action.diagnostics = std::vector<lsp::Diagnostic>{ diag };
                    if (i == 0 && hasClearWinner)
                    {
                        action.isPreferred = true;
                    }

                    lsp::TextEdit edit;
                    edit.range.start.line = static_cast<uint32_t>(directive->line);
                    edit.range.start.character = static_cast<uint32_t>(openPos + 1);
                    edit.range.end.line = static_cast<uint32_t>(directive->line);
                    edit.range.end.character = static_cast<uint32_t>(closePos);
                    edit.newText = ranked[i].spelling;

                    lsp::WorkspaceEdit wsEdit;
                    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                    changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                    wsEdit.changes = std::move(changes);
                    action.edit = std::move(wsEdit);

                    actions.push_back(std::move(action));
                }
            }
        }

        /**
         * @brief Calls the bool conversion operator explicitly, turning `if (h)` into `if (h.opImplConv())`.
         *
         * Sound for the same reason the accessor fix is, and verified the same way: measured against
         * the oracle, the explicit call is accepted under asEP_BOOL_CONVERSION_MODE 0 AND 1, while
         * the bare `if (h)` is accepted only under 1. Applying it cannot break a host that already
         * works; it can only stop one from breaking on the engine's own default.
         *
         * The operator's name comes from the diagnostic's own message rather than being resolved
         * again here: the rule already picked between opImplConv and opConv, and picking a second
         * time is a second chance to disagree with it.
         */
        void TryAddBoolConversionFix(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            for (const auto &diag : request.context.diagnostics)
            {
                if (!MatchDiagnosticCode(diag, "as-hint-bool-conversion"))
                {
                    continue;
                }

                // The message names the operator in quotes, second of the two quoted spans. Read
                // from there because the rule's choice is the one that must be honoured - and if the
                // message ever stops carrying it, no fix is offered rather than a guessed one.
                //
                // Coupled to the message's shape, and bounded on purpose: the operator name is an
                // AngelScript identifier and is not translated, and the result is checked against
                // the only two names that exist before any edit is built. A message that changes
                // shape produces no fix rather than a wrong one.
                if (!std::holds_alternative<lsp::String>(diag.message))
                {
                    continue;
                }
                const std::string &message = std::get<lsp::String>(diag.message);

                const size_t firstQuote = message.find('\'');
                const size_t firstClose = firstQuote == std::string::npos
                                              ? std::string::npos
                                              : message.find('\'', firstQuote + 1);
                const size_t secondQuote = firstClose == std::string::npos
                                               ? std::string::npos
                                               : message.find('\'', firstClose + 1);
                const size_t secondClose = secondQuote == std::string::npos
                                               ? std::string::npos
                                               : message.find('\'', secondQuote + 1);
                if (secondClose == std::string::npos)
                {
                    continue;
                }

                const std::string oper = message.substr(secondQuote + 1, secondClose - secondQuote - 1);
                if (oper != "opImplConv" && oper != "opConv")
                {
                    continue;
                }

                // Appended after the condition, not wrapped around it: the diagnostic's range is
                // exactly the condition expression, so `.opImplConv()` at its end is the whole edit.
                lsp::TextEdit edit;
                edit.range.start = diag.range.end;
                edit.range.end = diag.range.end;
                edit.newText = "." + oper + "()";

                lsp::CodeAction action;
                action.title = "Call " + oper + "() explicitly";
                action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                action.isPreferred = true;
                action.diagnostics = std::vector<lsp::Diagnostic>{ diag };

                lsp::WorkspaceEdit wsEdit;
                lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                wsEdit.changes = std::move(changes);
                action.edit = std::move(wsEdit);

                actions.push_back(std::move(action));
            }
        }

        /**
         * @brief Adds the `property` keyword to an accessor that carries none.
         *
         * The fix for the portability hint, and the reason it is offered as a fix at all: measured
         * against the oracle, an accessor WITH the keyword is accepted under
         * asEP_PROPERTY_ACCESSOR_MODE 2 and 3 alike, while one without it is rejected under 3. So
         * applying this cannot break a build that works today - it can only stop one from breaking
         * on a host that runs the engine's own default.
         */
        void TryAddAccessorPropertyKeywordFix(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            for (const auto &diag : request.context.diagnostics)
            {
                if (!MatchDiagnosticCode(diag, "as-hint-accessor-portability"))
                {
                    continue;
                }

                const TSPoint point = { diag.range.start.line, diag.range.start.character };
                TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);
                while (!ts_node_is_null(node) && std::string_view(ts_node_type(node)) != "func_declaration")
                {
                    node = ts_node_parent(node);
                }
                if (ts_node_is_null(node))
                {
                    continue;
                }

                // After the parameter list, which is where `property` goes and where `const` would
                // also sit. Anchored to the parameter list rather than to the body: an interface
                // declaration or a stub has no body to anchor to.
                const TSNode parameters = ts_node_child_by_field_name(node, "parameters", 10);
                if (ts_node_is_null(parameters))
                {
                    continue;
                }

                const TSPoint insertAt = ts_node_end_point(parameters);

                lsp::TextEdit edit;
                edit.range.start.line = insertAt.row;
                edit.range.start.character = insertAt.column;
                edit.range.end.line = insertAt.row;
                edit.range.end.character = insertAt.column;
                edit.newText = " property";

                lsp::CodeAction action;
                action.title = "Add the 'property' keyword";
                action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                action.isPreferred = true;
                action.diagnostics = std::vector<lsp::Diagnostic>{ diag };

                lsp::WorkspaceEdit wsEdit;
                lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                wsEdit.changes = std::move(changes);
                action.edit = std::move(wsEdit);

                actions.push_back(std::move(action));
            }
        }

        /**
         * @brief Tries to generate Missing const Qualifier quick fixes and intention actions.
         */
        void TryAddConstQualifierActions(
            const CodeActionRequest &request,
            TSNode rootNode,
            std::vector<lsp::CodeAction> &actions)
        {
            if (ts_node_is_null(rootNode) || request.sourceCode.empty())
            {
                return;
            }

            for (const auto &diag : request.context.diagnostics)
            {
                if (MatchDiagnosticCode(diag, "as-err-const-method-required"))
                {
                    TSPoint dPt = { diag.range.start.line, diag.range.start.character };
                    TSNode memberNode = ts_node_descendant_for_point_range(rootNode, dPt, dPt);
                    std::string methodName = GetNodeText(memberNode, request.sourceCode);

                    TSNode callee = ts_node_parent(memberNode);
                    TSNode objNode = ts_node_child_by_field_name(callee, "object", 6);
                    auto rootScope = request.scopeIndex.GetRoot(request.uri);
                    const analysis::Scope *scope = FindScopeByLineOrRoot(rootScope.get(), dPt.row, dPt.column);
                    std::string objType = analysis::CleanBaseType(analysis::ResolveExpressionType(objNode, scope, request.symbolTable, request.sourceCode, request.uri));

                    request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
                    {
                        for (const auto &sym : symList)
                        {
                            if (sym.type == analysis::SymbolType::Function &&
                                sym.name == methodName &&
                                (objType.empty() || sym.containerName == objType) &&
                                sym.fileUri == request.uri)
                            {
                                TSPoint fnPt = { sym.startLine, sym.startCharacter };
                                TSNode fnNode = ts_node_descendant_for_point_range(rootNode, fnPt, fnPt);
                                while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
                                {
                                    fnNode = ts_node_parent(fnNode);
                                }
                                if (!ts_node_is_null(fnNode))
                                {
                                    TSNode paramList = ts_node_child_by_field_name(fnNode, "parameters", 10);
                                    if (ts_node_is_null(paramList))
                                    {
                                        uint32_t cnt = ts_node_child_count(fnNode);
                                        for (uint32_t c = 0; c < cnt; ++c)
                                        {
                                            TSNode ch = ts_node_child(fnNode, c);
                                            if (std::string_view(ts_node_type(ch)) == "parameter_list")
                                            {
                                                paramList = ch;
                                                break;
                                            }
                                        }
                                    }
                                    if (!ts_node_is_null(paramList))
                                    {
                                        TSPoint insertPt = ts_node_end_point(paramList);
                                        lsp::TextEdit edit;
                                        edit.range = lsp::Range{ { insertPt.row, insertPt.column }, { insertPt.row, insertPt.column } };
                                        edit.newText = " const";

                                        lsp::CodeAction action;
                                        action.title = "Add 'const' qualifier to method";
                                        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
                                        action.isPreferred = true;
                                        action.diagnostics = { diag };

                                        lsp::WorkspaceEdit wsEdit;
                                        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                                        changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                                        wsEdit.changes = std::move(changes);
                                        action.edit = std::move(wsEdit);

                                        actions.push_back(std::move(action));
                                    }
                                }
                            }
                        }
                    });
                }
            }

            TSPoint pt = { request.range.start.line, request.range.start.character };
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
            if (ts_node_is_null(fnNode))
            {
                return;
            }

            TSNode classNode = fnNode;
            while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
            {
                classNode = ts_node_parent(classNode);
            }
            if (ts_node_is_null(classNode))
            {
                return;
            }

            TSNode paramList = ts_node_child_by_field_name(fnNode, "parameters", 10);
            if (ts_node_is_null(paramList))
            {
                uint32_t cnt = ts_node_child_count(fnNode);
                for (uint32_t c = 0; c < cnt; ++c)
                {
                    TSNode ch = ts_node_child(fnNode, c);
                    if (std::string_view(ts_node_type(ch)) == "parameter_list")
                    {
                        paramList = ch;
                        break;
                    }
                }
            }
            if (ts_node_is_null(paramList))
            {
                return;
            }

            TSNode bodyNode = ts_node_child_by_field_name(fnNode, "body", 4);
            if (ts_node_is_null(bodyNode))
            {
                uint32_t cnt = ts_node_child_count(fnNode);
                for (uint32_t i = 0; i < cnt; ++i)
                {
                    TSNode ch = ts_node_child(fnNode, i);
                    if (std::string_view(ts_node_type(ch)) == "statement_block")
                    {
                        bodyNode = ch;
                        break;
                    }
                }
            }

            uint32_t pEndByte = ts_node_end_byte(paramList);
            uint32_t bStartByte = !ts_node_is_null(bodyNode) ? ts_node_start_byte(bodyNode) : ts_node_end_byte(fnNode);
            if (pEndByte < bStartByte && bStartByte <= request.sourceCode.size())
            {
                std::string between = request.sourceCode.substr(pEndByte, bStartByte - pEndByte);
                if (between.find("const") != std::string::npos)
                {
                    return;
                }
            }

            TSNode classNameNode = ts_node_child_by_field_name(classNode, "name", 4);
            std::string className = GetNodeText(classNameNode, request.sourceCode);

            auto rootScope = request.scopeIndex.GetRoot(request.uri);
            const analysis::Scope *scope = FindScopeByLineOrRoot(rootScope.get(), ts_node_start_point(fnNode).row, ts_node_start_point(fnNode).column);

            if (!ts_node_is_null(bodyNode) && !MethodBodyMutatesClassState(bodyNode, classNode, request.sourceCode, request.symbolTable, className, scope))
            {
                TSPoint insertPt = ts_node_end_point(paramList);
                lsp::TextEdit edit;
                edit.range = lsp::Range{ { insertPt.row, insertPt.column }, { insertPt.row, insertPt.column } };
                edit.newText = " const";

                lsp::CodeAction action;
                action.title = "Add 'const' qualifier to method";
                action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

                lsp::WorkspaceEdit wsEdit;
                lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
                changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
                wsEdit.changes = std::move(changes);
                action.edit = std::move(wsEdit);

                actions.push_back(std::move(action));
            }
        }

        /**
         * @brief Tries to generate a Sort and Clean #include Directives code action.
         */
        void TryAddSortAndCleanIncludesAction(
            const CodeActionRequest &request,
            TSNode /*rootNode*/,
            std::vector<lsp::CodeAction> &actions)
        {
            auto includes = utils::IncludeResolver::ExtractIncludes(request.sourceCode);
            if (includes.empty())
            {
                return;
            }

            size_t firstLine = includes.front().line;
            size_t lastLine = includes.back().line;

            ankerl::unordered_dense::set<std::string> docReferences;
            auto rootScope = request.scopeIndex.GetRoot(request.uri);
            CollectAllReferences(rootScope.get(), docReferences);

            std::vector<std::string> angledIncludes;
            std::vector<std::string> quotedIncludes;
            ankerl::unordered_dense::set<std::string> seen;

            for (const auto &inc : includes)
            {
                if (seen.contains(inc.rawPath))
                {
                    continue;
                }
                seen.insert(inc.rawPath);

                std::string resolved = utils::IncludeResolver::ResolveIncludePath(inc.rawPath, request.uri, {});
                bool isUnused = false;

                std::vector<std::string> symbolsInFile;
                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symList)
                {
                    for (const auto &s : symList)
                    {
                        if ((!resolved.empty() && s.fileUri == resolved) ||
                            (!inc.rawPath.empty() && s.fileUri.find(inc.rawPath) != std::string::npos))
                        {
                            symbolsInFile.push_back(s.name);
                        }
                    }
                });

                if (!symbolsInFile.empty())
                {
                    bool anyReferenced = false;
                    for (const auto &symName : symbolsInFile)
                    {
                        if (docReferences.contains(symName))
                        {
                            anyReferenced = true;
                            break;
                        }
                    }
                    if (!anyReferenced)
                    {
                        isUnused = true;
                    }
                }

                if (isUnused)
                {
                    continue;
                }

                if (inc.isAngled)
                {
                    angledIncludes.push_back(inc.rawPath);
                }
                else
                {
                    quotedIncludes.push_back(inc.rawPath);
                }
            }

            std::sort(angledIncludes.begin(), angledIncludes.end());
            std::sort(quotedIncludes.begin(), quotedIncludes.end());

            std::string newHeaderBlock;
            for (const auto &p : angledIncludes)
            {
                newHeaderBlock += "#include <" + p + ">\n";
            }
            if (!angledIncludes.empty() && !quotedIncludes.empty())
            {
                newHeaderBlock += "\n";
            }
            for (const auto &p : quotedIncludes)
            {
                newHeaderBlock += "#include \"" + p + "\"\n";
            }

            lsp::TextEdit edit;
            edit.range.start = lsp::Position{ static_cast<uint32_t>(firstLine), 0 };
            edit.range.end = lsp::Position{ static_cast<uint32_t>(lastLine + 1), 0 };
            edit.newText = newHeaderBlock;

            lsp::CodeAction action;
            action.title = "Sort and Clean #include Directives";
            action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::SourceOrganizeImports);

            lsp::WorkspaceEdit wsEdit;
            lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
            changes[lsp::DocumentUri::parse(request.uri)] = { std::move(edit) };
            wsEdit.changes = std::move(changes);
            action.edit = std::move(wsEdit);

            actions.push_back(std::move(action));
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

                    // Bound to the diagnostic that asks for it, not only to the cursor sitting in
                    // the class. Without this the action existed but never appeared as the fix for
                    // the problem it fixes: an editor grouping quick fixes under a diagnostic, or
                    // asking for actions at a diagnostic's range, sees nothing to offer.
                    std::vector<lsp::Diagnostic> matchingDiags;
                    for (const auto &diag : request.context.diagnostics)
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

        // =========================================================================
        // Feature 1: Extract Variable Refactoring
        // =========================================================================
        TryAddExtractVariableAction(request, rootNode, actions);

        // =========================================================================
        // Feature 2: Extract Method Refactoring
        // =========================================================================
        TryAddExtractMethodAction(request, rootNode, actions);

        // =========================================================================
        // Feature 3: Getters and Setters Generation
        // =========================================================================
        TryAddGetterSetterActions(request, rootNode, actions);

        // =========================================================================
        // Feature 4: Missing const Qualifier Quick Fix & Intention
        // =========================================================================
        TryAddConstQualifierActions(request, rootNode, actions);
        TryAddUndefinedIdentifierSuggestions(request, rootNode, actions);
        TryAddHandleOnPrimitiveFix(request, rootNode, actions);
        TryAddUnresolvedIncludeSuggestions(request, actions);
        TryAddAccessorPropertyKeywordFix(request, rootNode, actions);
        TryAddBoolConversionFix(request, rootNode, actions);

        // =========================================================================
        // Feature 5: Sort and Clean #include Directives
        // =========================================================================
        TryAddSortAndCleanIncludesAction(request, rootNode, actions);

        if (actions.empty())
        {
            return std::nullopt;
        }

        return actions;
    }

    std::optional<lsp::CodeAction> ResolveCodeAction(const CodeActionResolveRequest &request)
    {
        return request.action;
    }
}
