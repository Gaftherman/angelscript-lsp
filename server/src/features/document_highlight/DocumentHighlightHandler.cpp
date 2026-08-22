#include "features/document_highlight/DocumentHighlightHandler.h"
#include "analysis/SemanticHelpers.h"
#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Checks if a given line and character position falls within a scope's range.
         * @param scope Target scope.
         * @param line 0-based line number.
         * @param character 0-based character offset.
         * @return True if position is inside scope boundaries.
         */
        bool IsInsideScope(const analysis::Scope &scope, uint32_t line, uint32_t character)
        {
            if (line < scope.startLine || line > scope.endLine)
            {
                return false;
            }
            if (line == scope.startLine && character < scope.startCharacter)
            {
                return false;
            }
            if (line == scope.endLine && character > scope.endCharacter)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief Finds the deepest/innermost scope enclosing a given source position.
         * @param root Root scope of the document.
         * @param line 0-based line number.
         * @param character 0-based character offset.
         * @return Innermost Scope pointer or nullptr if position is outside root.
         */
        const analysis::Scope *FindInnermostScope(const analysis::Scope *root, uint32_t line, uint32_t character)
        {
            if (!root || !IsInsideScope(*root, line, character))
            {
                return nullptr;
            }

            const analysis::Scope *current = root;
            bool foundChild = true;
            while (foundChild)
            {
                foundChild = false;
                for (const auto &child : current->children)
                {
                    if (child && IsInsideScope(*child, line, character))
                    {
                        current = child.get();
                        foundChild = true;
                        break;
                    }
                }
            }
            return current;
        }

        /**
         * @brief Recursively searches for the Scope that contains the given LocalDefinition.
         * @param current Current scope node in the tree.
         * @param def Target local definition to match.
         * @return Pointer to declaring Scope or nullptr if not found.
         */
        const analysis::Scope *FindScopeDeclaringDefinition(const analysis::Scope *current, const analysis::LocalDefinition &def)
        {
            if (!current)
            {
                return nullptr;
            }

            for (const auto &d : current->definitions)
            {
                if (d.name == def.name &&
                    d.startLine == def.startLine &&
                    d.startCharacter == def.startCharacter &&
                    d.endLine == def.endLine &&
                    d.endCharacter == def.endCharacter)
                {
                    return current;
                }
            }

            for (const auto &child : current->children)
            {
                const analysis::Scope *found = FindScopeDeclaringDefinition(child.get(), def);
                if (found)
                {
                    return found;
                }
            }

            return nullptr;
        }

        /**
         * @brief Checks whether a definition's declaring scope is inside any function/method/lambda body.
         */
        bool IsDeclaredInFunctionScope(const analysis::Scope *defScope)
        {
            for (const analysis::Scope *cur = defScope; cur != nullptr; cur = cur->parent)
            {
                if (cur->isFunctionScope)
                {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Resolves the enclosing class name for a given source position.
         * @param symbolTable Symbol table to look up class definitions.
         * @param uri Document file URI.
         * @param line 0-based line number.
         * @return Enclosing class name or empty string if not in a class.
         */
        std::string GetEnclosingClassName(const analysis::SymbolTable &symbolTable, const std::string &uri, uint32_t line, const std::string &excludeName = "")
        {
            std::string enclosingClass;
            symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if ((sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface) &&
                            sym.fileUri == uri && (excludeName.empty() || sym.name != excludeName))
                        {
                            if (line >= sym.startLine && line <= sym.endLine)
                            {
                                enclosingClass = sym.name;
                            }
                        }
                    }
                });
            return enclosingClass;
        }

        std::string GetNodeText(TSNode node, std::string_view sourceCode)
        {
            if (ts_node_is_null(node) || sourceCode.empty())
            {
                return "";
            }
            uint32_t start = ts_node_start_byte(node);
            uint32_t end = ts_node_end_byte(node);
            if (start < sourceCode.size() && end <= sourceCode.size() && start < end)
            {
                return std::string(sourceCode.substr(start, end - start));
            }
            return "";
        }

        /**
         * @brief Extracts token text and AST node under cursor with trailing-edge tolerance.
         * @param sourceCode Document source text.
         * @param tree Tree-sitter AST.
         * @param position Cursor position.
         * @param outNode Output TSNode.
         * @return Token text or empty string if not an identifier.
         */
        std::string GetNodeTextAt(const std::string &sourceCode, TSTree *tree, lsp::Position position, TSNode &outNode)
        {
            if (!tree || sourceCode.empty())
            {
                return "";
            }

            TSNode rootNode = ts_tree_root_node(tree);
            TSPoint point = { position.line, position.character };
            TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

            if (ts_node_is_null(node))
            {
                return "";
            }

            std::string_view nodeType = ts_node_type(node);
            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                if (position.character > 0)
                {
                    TSPoint prevPoint = { position.line, position.character - 1 };
                    TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPoint, prevPoint);
                    if (!ts_node_is_null(prevNode))
                    {
                        std::string_view prevType = ts_node_type(prevNode);
                        if (prevType == "identifier" || prevType == "primitive_type" || prevType == "scoped_identifier")
                        {
                            node = prevNode;
                            nodeType = prevType;
                        }
                    }
                }
            }

            if (nodeType == "scoped_identifier")
            {
                TSNode leaf = ts_node_descendant_for_point_range(node, point, point);
                if (!ts_node_is_null(leaf) && std::string_view(ts_node_type(leaf)) == "identifier")
                {
                    node = leaf;
                    nodeType = "identifier";
                }
            }

            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                return "";
            }

            uint32_t startByte = ts_node_start_byte(node);
            uint32_t endByte = ts_node_end_byte(node);
            if (startByte >= sourceCode.size() || endByte > sourceCode.size() || startByte >= endByte)
            {
                return "";
            }

            outNode = node;
            return sourceCode.substr(startByte, endByte - startByte);
        }

        enum class TargetKind
        {
            Local,
            ClassMember,
            NamespaceSymbol,
            GlobalSymbol
        };

        struct TargetDescriptor
        {
            TargetKind kind = TargetKind::GlobalSymbol;
            std::string name;
            std::string qualifiedName;

            // Local variable / parameter
            const analysis::Scope *definingScope = nullptr;
            analysis::LocalDefinition localDef;
            std::string localUri;

            // Class member
            std::string declaringClass;
            std::vector<std::string> relatedClasses;

            // Namespace symbol
            std::string declaringNamespace;
        };

        /**
         * @brief Checks if an AST node is contained within another AST node range.
         */
        bool IsNodeContained(TSNode inner, TSNode outer)
        {
            if (ts_node_is_null(inner) || ts_node_is_null(outer))
            {
                return false;
            }
            return ts_node_start_byte(inner) >= ts_node_start_byte(outer) &&
                   ts_node_end_byte(inner) <= ts_node_end_byte(outer);
        }

        /**
         * @brief Classifies the AST context of an occurrence into Write, Read, or Text highlight kinds.
         */
        lsp::DocumentHighlightKind ClassifyOccurrence(
            const std::string &sourceCode,
            TSTree *tree,
            const lsp::Range &range,
            const analysis::SymbolTable &symbolTable,
            const analysis::ScopeIndex &scopeIndex,
            const std::string &uri)
        {
            if (!tree || sourceCode.empty())
            {
                return lsp::DocumentHighlightKind::Read;
            }

            TSNode rootNode = ts_tree_root_node(tree);
            TSPoint pt = { range.start.line, range.start.character };
            TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);

            if (ts_node_is_null(leaf))
            {
                return lsp::DocumentHighlightKind::Read;
            }

            if (std::string_view(ts_node_type(leaf)) == "scoped_identifier")
            {
                TSNode inner = ts_node_descendant_for_point_range(leaf, pt, pt);
                if (!ts_node_is_null(inner))
                {
                    leaf = inner;
                }
            }

            // Check if leaf is a declaration identifier
            TSNode parent = ts_node_parent(leaf);
            if (!ts_node_is_null(parent))
            {
                std::string_view pType = ts_node_type(parent);

                // Variable declaration: int x = 10;
                if (pType == "variable_declarator")
                {
                    TSNode nameNode = ts_node_child_by_field_name(parent, "name", 4);
                    if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
                    {
                        return lsp::DocumentHighlightKind::Write;
                    }
                }

                // Parameter declaration: void foo(int a)
                if (pType == "parameter" || pType == "lambda_parameter")
                {
                    TSNode nameNode = ts_node_child_by_field_name(parent, "name", 4);
                    if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
                    {
                        return lsp::DocumentHighlightKind::Write;
                    }
                }

                // Foreach variable: foreach(auto item : list)
                if (pType == "foreach_variable")
                {
                    TSNode nameNode = ts_node_child_by_field_name(parent, "name", 4);
                    if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
                    {
                        return lsp::DocumentHighlightKind::Write;
                    }
                }

                // Function / method / funcdef declaration name: void Foo()
                if (pType == "func_declaration" || pType == "interface_method" || pType == "funcdef_declaration")
                {
                    TSNode nameNode = ts_node_child_by_field_name(parent, "name", 4);
                    if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
                    {
                        return lsp::DocumentHighlightKind::Text;
                    }
                }

                // Type declarations (class, interface, mixin, namespace, enum, typedef)
                if (pType == "class_declaration" || pType == "mixin_declaration" ||
                    pType == "interface_declaration" || pType == "namespace_declaration" ||
                    pType == "enum_declaration" || pType == "typedef_declaration")
                {
                    TSNode nameNode = ts_node_child_by_field_name(parent, "name", 4);
                    if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
                    {
                        return lsp::DocumentHighlightKind::Text;
                    }
                }

                // Enum member: State_Idle
                if (pType == "enum_member")
                {
                    TSNode nameNode = ts_node_child_by_field_name(parent, "name", 4);
                    if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
                    {
                        return lsp::DocumentHighlightKind::Text;
                    }
                }
            }

            // Check if leaf is part of a type annotation (e.g. MyClass in `MyClass obj;` or `cast<MyClass>(...)`)
            for (TSNode cur = leaf; !ts_node_is_null(cur); cur = ts_node_parent(cur))
            {
                std::string_view cType = ts_node_type(cur);
                if (cType == "type" || cType == "datatype" || cType == "base_class_list" || cType == "template_type_list")
                {
                    // Verify it's not a constructor call or variable declarator name
                    TSNode curParent = ts_node_parent(cur);
                    if (!ts_node_is_null(curParent))
                    {
                        std::string_view cpType = ts_node_type(curParent);
                        if (cpType != "variable_declarator")
                        {
                            return lsp::DocumentHighlightKind::Text;
                        }
                    }
                    else
                    {
                        return lsp::DocumentHighlightKind::Text;
                    }
                }
                if (cType == "statement_block" || cType == "func_declaration" || cType == "class_body")
                {
                    break;
                }
            }

            // Walk ancestor expressions to check for mutating operations or function arguments
            for (TSNode cur = leaf; !ts_node_is_null(cur); cur = ts_node_parent(cur))
            {
                std::string_view cType = ts_node_type(cur);

                // 1. Postfix increment / decrement: x++ / x--
                if (cType == "postfix_expression")
                {
                    TSNode operand = ts_node_child_by_field_name(cur, "operand", 7);
                    if (!ts_node_is_null(operand) && IsNodeContained(leaf, operand))
                    {
                        return lsp::DocumentHighlightKind::Write;
                    }
                }

                // 2. Unary prefix increment / decrement: ++x / --x
                if (cType == "unary_expression")
                {
                    TSNode opNode = ts_node_child_by_field_name(cur, "operator", 8);
                    if (!ts_node_is_null(opNode))
                    {
                        uint32_t opStart = ts_node_start_byte(opNode);
                        uint32_t opEnd = ts_node_end_byte(opNode);
                        if (opStart < sourceCode.size() && opEnd <= sourceCode.size())
                        {
                            std::string opText = sourceCode.substr(opStart, opEnd - opStart);
                            if (opText == "++" || opText == "--")
                            {
                                TSNode operand = ts_node_child_by_field_name(cur, "operand", 7);
                                if (!ts_node_is_null(operand) && IsNodeContained(leaf, operand))
                                {
                                    return lsp::DocumentHighlightKind::Write;
                                }
                            }
                        }
                    }
                }

                // 3. Assignment expression: left = right, left += right, etc.
                if (cType == "assignment_expression")
                {
                    TSNode leftNode = ts_node_child_by_field_name(cur, "left", 4);
                    if (!ts_node_is_null(leftNode) && IsNodeContained(leaf, leftNode))
                    {
                        // Check if LHS is a member expression: obj.field = value
                        if (std::string_view(ts_node_type(leftNode)) == "member_expression")
                        {
                            TSNode objNode = ts_node_child_by_field_name(leftNode, "object", 6);
                            TSNode memNode = ts_node_child_by_field_name(leftNode, "member", 6);

                            if (!ts_node_is_null(objNode) && IsNodeContained(leaf, objNode))
                            {
                                // Receiver object is Read
                                return lsp::DocumentHighlightKind::Read;
                            }
                            if (!ts_node_is_null(memNode) && IsNodeContained(leaf, memNode))
                            {
                                // Assigned member is Write
                                return lsp::DocumentHighlightKind::Write;
                            }
                        }

                        // Check if LHS is an index expression: arr[i] = value
                        if (std::string_view(ts_node_type(leftNode)) == "index_expression")
                        {
                            TSNode objNode = ts_node_child_by_field_name(leftNode, "object", 6);
                            if (!ts_node_is_null(objNode) && IsNodeContained(leaf, objNode))
                            {
                                // Array receiver is Read (reading handle to update element)
                                return lsp::DocumentHighlightKind::Read;
                            }
                            // Index operand itself is Read
                            return lsp::DocumentHighlightKind::Read;
                        }

                        return lsp::DocumentHighlightKind::Write;
                    }

                    // If inside RHS, it is a Read
                    TSNode rightNode = ts_node_child_by_field_name(cur, "right", 5);
                    if (!ts_node_is_null(rightNode) && IsNodeContained(leaf, rightNode))
                    {
                        return lsp::DocumentHighlightKind::Read;
                    }
                }

                // 4. Function call arguments: check if parameter is out/inout reference
                if (cType == "argument_list")
                {
                    TSNode callNode = ts_node_parent(cur);
                    if (!ts_node_is_null(callNode) && std::string_view(ts_node_type(callNode)) == "call_expression")
                    {
                        // Determine argument index
                        uint32_t childCount = ts_node_child_count(cur);
                        uint32_t argIndex = 0;
                        bool foundArg = false;

                        for (uint32_t i = 0; i < childCount; ++i)
                        {
                            TSNode argChild = ts_node_child(cur, i);
                            std::string_view childType = ts_node_type(argChild);
                            if (childType == "(" || childType == ")" || childType == ",")
                            {
                                continue;
                            }

                            if (IsNodeContained(leaf, argChild))
                            {
                                foundArg = true;
                                break;
                            }
                            argIndex++;
                        }

                        if (foundArg)
                        {
                            TSNode funcNode = ts_node_child_by_field_name(callNode, "function", 8);
                            if (!ts_node_is_null(funcNode))
                            {
                                uint32_t fStart = ts_node_start_byte(funcNode);
                                uint32_t fEnd = ts_node_end_byte(funcNode);
                                if (fStart < sourceCode.size() && fEnd <= sourceCode.size())
                                {
                                    std::string calleeName;
                                    std::string receiverType;

                                    if (std::string_view(ts_node_type(funcNode)) == "member_expression")
                                    {
                                        TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);
                                        TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                                        if (!ts_node_is_null(memNode))
                                        {
                                            uint32_t mStart = ts_node_start_byte(memNode);
                                            uint32_t mEnd = ts_node_end_byte(memNode);
                                            if (mStart < sourceCode.size() && mEnd <= sourceCode.size())
                                            {
                                                calleeName = sourceCode.substr(mStart, mEnd - mStart);
                                            }
                                        }
                                        if (!ts_node_is_null(objNode))
                                        {
                                            uint32_t oStart = ts_node_start_byte(objNode);
                                            uint32_t oEnd = ts_node_end_byte(objNode);
                                            if (oStart < sourceCode.size() && oEnd <= sourceCode.size())
                                            {
                                                std::string oText = sourceCode.substr(oStart, oEnd - oStart);
                                                if (oText == "this")
                                                {
                                                    receiverType = GetEnclosingClassName(symbolTable, uri, range.start.line);
                                                }
                                                else
                                                {
                                                    auto rootScope = scopeIndex.GetRoot(uri);
                                                    if (rootScope)
                                                    {
                                                        const analysis::Scope *s = FindInnermostScope(rootScope.get(), range.start.line, range.start.character);
                                                        if (s)
                                                        {
                                                            const analysis::LocalDefinition *oDef = analysis::ResolveInScope(s, oText);
                                                            if (oDef && !oDef->typeName.empty())
                                                            {
                                                                receiverType = analysis::CleanBaseType(oDef->typeName);
                                                            }
                                                        }
                                                    }
                                                    if (receiverType.empty())
                                                    {
                                                        auto gSyms = symbolTable.FindSymbols(oText);
                                                        for (const auto &gs : gSyms)
                                                        {
                                                            if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
                                                            {
                                                                receiverType = analysis::CleanBaseType(gs.GetVariable().typeName);
                                                                break;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    else
                                    {
                                        calleeName = sourceCode.substr(fStart, fEnd - fStart);
                                    }

                                    // Look up callee in symbolTable
                                    std::vector<analysis::Symbol> matches;
                                    if (!receiverType.empty())
                                    {
                                        auto related = analysis::GetAllRelatedClasses(receiverType, symbolTable);
                                        for (const auto &cls : related)
                                        {
                                            auto found = symbolTable.FindSymbols(cls + "::" + calleeName);
                                            matches.insert(matches.end(), found.begin(), found.end());
                                        }
                                    }
                                    else if (!calleeName.empty())
                                    {
                                        matches = symbolTable.FindSymbols(calleeName);
                                    }

                                    for (const auto &sym : matches)
                                    {
                                        if (sym.type == analysis::SymbolType::Function)
                                        {
                                            const auto &sig = sym.GetFunction();
                                            if (argIndex < sig.parameters.size())
                                            {
                                                const auto &param = sig.parameters[argIndex];
                                                if (param.modifier == analysis::ParameterModifier::Out ||
                                                    param.modifier == analysis::ParameterModifier::InOut)
                                                {
                                                    return lsp::DocumentHighlightKind::Write;
                                                }
                                                if (param.isReference && !param.isConst && param.modifier != analysis::ParameterModifier::In)
                                                {
                                                    return lsp::DocumentHighlightKind::Write;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if (cType == "statement_block")
                {
                    break;
                }
            }

            return lsp::DocumentHighlightKind::Read;
        }
    }

    std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest &request)
    {
        TSNode node{};
        std::string nodeText = GetNodeTextAt(request.sourceCode, request.tree, request.position, node);
        if (nodeText.empty() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        if (analysis::IsReservedKeyword(nodeText) || analysis::IsPrimitiveTypeName(nodeText))
        {
            return std::nullopt;
        }

        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        TSNode parent = ts_node_parent(node);

        TargetDescriptor target;
        target.name = nodeText;

        // 1. Context A: Member child of member_expression (obj.member)
        bool isExplicitMemberAccess = false;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
        {
            TSNode memNode = ts_node_child_by_field_name(parent, "member", 6);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
            {
                isExplicitMemberAccess = true;
                TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
                if (!ts_node_is_null(objectNode))
                {
                    uint32_t objStart = ts_node_start_byte(objectNode);
                    uint32_t objEnd = ts_node_end_byte(objectNode);
                    if (objStart < request.sourceCode.size() && objEnd <= request.sourceCode.size())
                    {
                        std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);
                        std::string receiverTypeName;

                        if (objText == "this")
                        {
                            receiverTypeName = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line);
                        }
                        else if (rootScope)
                        {
                            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
                            if (scope)
                            {
                                const analysis::LocalDefinition *objDef = analysis::ResolveInScope(scope, objText);
                                if (objDef && !objDef->typeName.empty())
                                {
                                    receiverTypeName = analysis::CleanBaseType(objDef->typeName);
                                }
                            }
                        }

                        if (receiverTypeName.empty())
                        {
                            auto globSyms = request.symbolTable.FindSymbols(objText);
                            for (const auto &sym : globSyms)
                            {
                                if (sym.type == analysis::SymbolType::Variable)
                                {
                                    const auto &var = sym.GetVariable();
                                    if (!var.typeName.empty())
                                    {
                                        receiverTypeName = analysis::CleanBaseType(var.typeName);
                                        break;
                                    }
                                }
                            }
                        }

                        if (!receiverTypeName.empty())
                        {
                            target.kind = TargetKind::ClassMember;
                            target.declaringClass = receiverTypeName;
                            target.relatedClasses = analysis::GetAllRelatedClasses(receiverTypeName, request.symbolTable);
                        }
                    }
                }
            }
        }

        // 2. Context B: Lexical Scope Definition or Local Variable Reference (in function scope)
        if (!isExplicitMemberAccess && rootScope)
        {
            const analysis::Scope *innerScope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (innerScope)
            {
                const analysis::LocalDefinition *matchedDef = nullptr;
                const analysis::Scope *declScope = nullptr;

                for (const analysis::Scope *cur = innerScope; cur != nullptr; cur = cur->parent)
                {
                    for (const auto &d : cur->definitions)
                    {
                        if (d.name == nodeText &&
                            request.position.line >= d.startLine && request.position.line <= d.endLine)
                        {
                            matchedDef = &d;
                            declScope = cur;
                            break;
                        }
                    }
                    if (matchedDef)
                    {
                        break;
                    }
                }

                if (!matchedDef)
                {
                    matchedDef = analysis::ResolveInScope(innerScope, nodeText);
                    if (matchedDef)
                    {
                        declScope = FindScopeDeclaringDefinition(rootScope.get(), *matchedDef);
                    }
                }

                if (matchedDef && declScope)
                {
                    if (matchedDef->kind == analysis::LocalDefinitionKind::Parameter ||
                        matchedDef->kind == analysis::LocalDefinitionKind::Variable)
                    {
                        bool isFunctionLocal = IsDeclaredInFunctionScope(declScope);
                        if (isFunctionLocal || matchedDef->kind == analysis::LocalDefinitionKind::Parameter)
                        {
                            target.kind = TargetKind::Local;
                            target.definingScope = declScope;
                            target.localDef = *matchedDef;
                            target.localUri = request.uri;
                        }
                        else
                        {
                            std::string enclosingClass = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line, nodeText);
                            if (!enclosingClass.empty())
                            {
                                target.kind = TargetKind::ClassMember;
                                target.declaringClass = enclosingClass;
                                target.relatedClasses = analysis::GetAllRelatedClasses(enclosingClass, request.symbolTable);
                            }
                        }
                    }
                    else if (matchedDef->kind == analysis::LocalDefinitionKind::Field ||
                             matchedDef->kind == analysis::LocalDefinitionKind::Method)
                    {
                        std::string enclosingClass = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line, nodeText);
                        if (!enclosingClass.empty())
                        {
                            target.kind = TargetKind::ClassMember;
                            target.declaringClass = enclosingClass;
                            target.relatedClasses = analysis::GetAllRelatedClasses(enclosingClass, request.symbolTable);
                        }
                    }
                }
            }
        }

        // 3. Context C: Enclosing Container Search (Classes, Interfaces, Namespaces)
        if (target.kind == TargetKind::GlobalSymbol && !isExplicitMemberAccess)
        {
            std::string enclosingClass = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line, nodeText);
            if (!enclosingClass.empty())
            {
                target.kind = TargetKind::ClassMember;
                target.declaringClass = enclosingClass;
                target.relatedClasses = analysis::GetAllRelatedClasses(enclosingClass, request.symbolTable);
            }
            else
            {
                auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
                for (const auto &container : containers)
                {
                    if (container.kind == analysis::ContainerKind::Class || container.kind == analysis::ContainerKind::Interface)
                    {
                        auto hierarchy = analysis::GetAllRelatedClasses(container.qualifiedName, request.symbolTable);
                        for (const auto &cls : hierarchy)
                        {
                            if (request.symbolTable.HasSymbol(cls + "::" + nodeText))
                            {
                                target.kind = TargetKind::ClassMember;
                                target.declaringClass = cls;
                                target.relatedClasses = std::move(hierarchy);
                                break;
                            }
                        }
                        if (target.kind == TargetKind::ClassMember)
                        {
                            break;
                        }
                    }
                    else if (container.kind == analysis::ContainerKind::Namespace)
                    {
                        std::string qName = container.qualifiedName + "::" + nodeText;
                        if (request.symbolTable.HasSymbol(qName))
                        {
                            target.kind = TargetKind::NamespaceSymbol;
                            target.declaringNamespace = container.qualifiedName;
                            target.qualifiedName = qName;
                            break;
                        }
                    }
                }
            }
        }

        // 4. Context D: SymbolTable Symbol Lookup in current document
        if (target.kind == TargetKind::GlobalSymbol)
        {
            request.symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.fileUri == request.uri &&
                            request.position.line >= sym.startLine && request.position.line <= sym.endLine &&
                            sym.name == nodeText)
                        {
                            if (!sym.containerName.empty())
                            {
                                auto containerSyms = request.symbolTable.FindSymbols(sym.containerName);
                                bool isClassContainer = false;
                                bool isNamespaceContainer = false;
                                for (const auto &csym : containerSyms)
                                {
                                    if (csym.type == analysis::SymbolType::Class || csym.type == analysis::SymbolType::Interface)
                                    {
                                        isClassContainer = true;
                                        break;
                                    }
                                    if (csym.type == analysis::SymbolType::Namespace)
                                    {
                                        isNamespaceContainer = true;
                                    }
                                }

                                if (isClassContainer)
                                {
                                    target.kind = TargetKind::ClassMember;
                                    target.declaringClass = sym.containerName;
                                    target.relatedClasses = analysis::GetAllRelatedClasses(sym.containerName, request.symbolTable);
                                    return;
                                }
                                else if (isNamespaceContainer)
                                {
                                    target.kind = TargetKind::NamespaceSymbol;
                                    target.declaringNamespace = sym.containerName;
                                    target.qualifiedName = sym.qualifiedName;
                                    return;
                                }
                            }
                        }
                    }
                });
        }

        // Collect occurrence ranges in current document
        std::vector<lsp::Range> ranges;
        std::set<std::pair<uint32_t, uint32_t>> seen;

        if (target.kind == TargetKind::Local)
        {
            ranges.push_back(lsp::Range{
                lsp::Position{ target.localDef.startLine, target.localDef.startCharacter },
                lsp::Position{ target.localDef.endLine, target.localDef.endCharacter }
            });
            seen.insert({ target.localDef.startLine, target.localDef.startCharacter });

            std::function<void(const analysis::Scope *, bool)> collectLocal =
                [&](const analysis::Scope *scope, bool isRoot)
                {
                    if (!scope)
                    {
                        return;
                    }

                    if (!isRoot)
                    {
                        for (const auto &def : scope->definitions)
                        {
                            if (def.name == target.name)
                            {
                                return;
                            }
                        }
                    }

                    for (const auto &ref : scope->references)
                    {
                        if (ref.name == target.name && !ref.isMemberAccess)
                        {
                            if (seen.insert({ ref.startLine, ref.startCharacter }).second)
                            {
                                ranges.push_back(lsp::Range{
                                    lsp::Position{ ref.startLine, ref.startCharacter },
                                    lsp::Position{ ref.endLine, ref.endCharacter }
                                });
                            }
                        }
                    }

                    for (const auto &child : scope->children)
                    {
                        collectLocal(child.get(), false);
                    }
                };

            collectLocal(target.definingScope, true);
        }
        else if (target.kind == TargetKind::ClassMember)
        {
            std::unordered_set<std::string> relatedSet(target.relatedClasses.begin(), target.relatedClasses.end());
            if (!target.declaringClass.empty())
            {
                relatedSet.insert(target.declaringClass);
            }

            // Collect declarations in current document
            for (const auto &clsName : relatedSet)
            {
                std::string qualifiedName = clsName + "::" + target.name;
                auto syms = request.symbolTable.FindSymbols(qualifiedName);
                for (const auto &sym : syms)
                {
                    if (sym.fileUri == request.uri && sym.type != analysis::SymbolType::CallReference)
                    {
                        uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                        uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                        uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                        uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                        if (seen.insert({ sL, sC }).second)
                        {
                            ranges.push_back(lsp::Range{
                                lsp::Position{ sL, sC },
                                lsp::Position{ eL, eC }
                            });
                        }
                    }
                }
            }

            // Scan current document scope tree
            if (rootScope)
            {
                std::function<void(const analysis::Scope *)> scanScopes =
                    [&](const analysis::Scope *s)
                    {
                        if (!s)
                        {
                            return;
                        }

                        for (const auto &ref : s->references)
                        {
                            if (ref.name != target.name)
                            {
                                continue;
                            }

                            bool isMatch = false;
                            const analysis::Scope *activeScope = FindInnermostScope(rootScope.get(), ref.startLine, ref.startCharacter);
                            if (!activeScope) activeScope = s;

                            bool isMember = ref.isMemberAccess;
                            if (!isMember && request.tree)
                            {
                                TSNode rootNode = ts_tree_root_node(request.tree);
                                TSPoint pt = { ref.startLine, ref.startCharacter };
                                TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                                if (!ts_node_is_null(refNode))
                                {
                                    TSNode p = ts_node_parent(refNode);
                                    if (!ts_node_is_null(p) && std::string_view(ts_node_type(p)) == "member_expression")
                                    {
                                        isMember = true;
                                    }
                                }
                            }

                            if (isMember)
                            {
                                if (request.tree)
                                {
                                    TSNode rootNode = ts_tree_root_node(request.tree);
                                    TSPoint pt = { ref.startLine, ref.startCharacter };
                                    TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                                    if (!ts_node_is_null(refNode))
                                    {
                                        if (std::string_view(ts_node_type(refNode)) != "identifier")
                                        {
                                            uint32_t cCount = ts_node_child_count(refNode);
                                            for (uint32_t c = 0; c < cCount; ++c)
                                            {
                                                TSNode ch = ts_node_child(refNode, c);
                                                if (std::string_view(ts_node_type(ch)) == "identifier" && GetNodeText(ch, request.sourceCode) == ref.name)
                                                {
                                                    refNode = ch;
                                                    break;
                                                }
                                            }
                                        }

                                        TSNode exprParent = ts_node_parent(refNode);
                                        while (!ts_node_is_null(exprParent) && std::string_view(ts_node_type(exprParent)) != "member_expression")
                                        {
                                            if (std::string_view(ts_node_type(exprParent)) == "class_declaration" ||
                                                std::string_view(ts_node_type(exprParent)) == "function_declaration")
                                            {
                                                exprParent = TSNode{};
                                                break;
                                            }
                                            exprParent = ts_node_parent(exprParent);
                                        }

                                        if (!ts_node_is_null(exprParent))
                                        {
                                            TSNode objNode = ts_node_child_by_field_name(exprParent, "object", 6);
                                            if (ts_node_is_null(objNode) && ts_node_named_child_count(exprParent) > 0)
                                            {
                                                objNode = ts_node_named_child(exprParent, 0);
                                            }
                                            if (!ts_node_is_null(objNode))
                                            {
                                                std::string rType = analysis::ResolveExpressionType(objNode, s, request.symbolTable, request.sourceCode, request.uri);
                                                if (rType.empty() && activeScope && activeScope != s)
                                                {
                                                    rType = analysis::ResolveExpressionType(objNode, activeScope, request.symbolTable, request.sourceCode, request.uri);
                                                }
                                                if (rType.empty())
                                                {
                                                    uint32_t oStart = ts_node_start_byte(objNode);
                                                    uint32_t oEnd = ts_node_end_byte(objNode);
                                                    if (oStart < request.sourceCode.size() && oEnd <= request.sourceCode.size())
                                                    {
                                                        std::string oText = request.sourceCode.substr(oStart, oEnd - oStart);
                                                        if (oText == "this")
                                                        {
                                                            rType = GetEnclosingClassName(request.symbolTable, request.uri, ref.startLine, ref.name);
                                                        }
                                                        else if (oText.find('.') != std::string::npos)
                                                        {
                                                            std::vector<std::string> parts;
                                                            size_t startPos = 0;
                                                            while (startPos < oText.size())
                                                            {
                                                                size_t dotPos = oText.find('.', startPos);
                                                                if (dotPos == std::string::npos)
                                                                {
                                                                    parts.push_back(oText.substr(startPos));
                                                                    break;
                                                                }
                                                                parts.push_back(oText.substr(startPos, dotPos - startPos));
                                                                startPos = dotPos + 1;
                                                            }

                                                            std::string curType;
                                                            for (size_t p = 0; p < parts.size(); ++p)
                                                            {
                                                                const auto &part = parts[p];
                                                                if (p == 0)
                                                                {
                                                                    const analysis::LocalDefinition *pDef = analysis::ResolveInScope(s, part);
                                                                    if (!pDef && activeScope) pDef = analysis::ResolveInScope(activeScope, part);
                                                                    if (pDef && !pDef->typeName.empty())
                                                                    {
                                                                        curType = analysis::CleanBaseType(pDef->typeName);
                                                                    }
                                                                    else
                                                                    {
                                                                        auto pSyms = request.symbolTable.FindSymbols(part);
                                                                        for (const auto &ps : pSyms)
                                                                        {
                                                                            if (ps.type == analysis::SymbolType::Variable && !ps.GetVariable().typeName.empty())
                                                                            {
                                                                                curType = analysis::CleanBaseType(ps.GetVariable().typeName);
                                                                                break;
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                                else if (!curType.empty())
                                                                {
                                                                    std::string qMem = curType + "::" + part;
                                                                    auto mSyms = request.symbolTable.FindSymbols(qMem);
                                                                    if (mSyms.empty()) mSyms = request.symbolTable.FindSymbols(part);
                                                                    std::string nextType;
                                                                    for (const auto &ms : mSyms)
                                                                    {
                                                                        if ((ms.type == analysis::SymbolType::Variable || ms.type == analysis::SymbolType::Property) && !ms.GetVariable().typeName.empty())
                                                                        {
                                                                            nextType = analysis::CleanBaseType(ms.GetVariable().typeName);
                                                                            break;
                                                                        }
                                                                        else if (ms.type == analysis::SymbolType::Function && !ms.GetFunction().returnType.empty())
                                                                        {
                                                                            nextType = analysis::CleanBaseType(ms.GetFunction().returnType);
                                                                            break;
                                                                        }
                                                                    }
                                                                    curType = nextType;
                                                                }
                                                            }
                                                            rType = curType;
                                                        }
                                                        else
                                                        {
                                                            const analysis::LocalDefinition *oDef = analysis::ResolveInScope(s, oText);
                                                            if (!oDef && activeScope)
                                                            {
                                                                oDef = analysis::ResolveInScope(activeScope, oText);
                                                            }
                                                            if (oDef && !oDef->typeName.empty())
                                                            {
                                                                rType = analysis::CleanBaseType(oDef->typeName);
                                                            }
                                                            else
                                                            {
                                                                auto gSyms = request.symbolTable.FindSymbols(oText);
                                                                for (const auto &gs : gSyms)
                                                                {
                                                                    if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
                                                                    {
                                                                        rType = analysis::CleanBaseType(gs.GetVariable().typeName);
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }

                                                rType = analysis::CleanBaseType(rType);
                                                if (!rType.empty() && relatedSet.contains(rType))
                                                {
                                                    isMatch = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            else
                            {
                                std::string encClass = GetEnclosingClassName(request.symbolTable, request.uri, ref.startLine, ref.name);
                                if (relatedSet.contains(encClass))
                                {
                                    const analysis::LocalDefinition *localShadow = analysis::ResolveInScope(activeScope, target.name);
                                    if (!localShadow || localShadow->kind == analysis::LocalDefinitionKind::Field || localShadow->kind == analysis::LocalDefinitionKind::Method)
                                    {
                                        isMatch = true;
                                    }
                                }
                            }

                            if (isMatch)
                            {
                                if (seen.insert({ ref.startLine, ref.startCharacter }).second)
                                {
                                    ranges.push_back(lsp::Range{
                                        lsp::Position{ ref.startLine, ref.startCharacter },
                                        lsp::Position{ ref.endLine, ref.endCharacter }
                                    });
                                }
                            }
                        }

                        for (const auto &child : s->children)
                        {
                            scanScopes(child.get());
                        }
                    };

                scanScopes(rootScope.get());
            }
        }
        else if (target.kind == TargetKind::NamespaceSymbol)
        {
            auto syms = request.symbolTable.FindSymbols(target.qualifiedName);
            for (const auto &sym : syms)
            {
                if (sym.fileUri == request.uri && sym.type != analysis::SymbolType::CallReference)
                {
                    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                    uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                    uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                    if (seen.insert({ sL, sC }).second)
                    {
                        ranges.push_back(lsp::Range{
                            lsp::Position{ sL, sC },
                            lsp::Position{ eL, eC }
                        });
                    }
                }
            }

            if (rootScope)
            {
                std::vector<std::pair<uint32_t, uint32_t>> nsRanges;
                request.symbolTable.ForEachSymbol(
                    [&](const std::string &, const std::vector<analysis::Symbol> &sList)
                    {
                        for (const auto &s : sList)
                        {
                            if (s.type == analysis::SymbolType::Namespace && s.fileUri == request.uri &&
                                (s.name == target.declaringNamespace || s.qualifiedName == target.declaringNamespace))
                            {
                                nsRanges.push_back({ s.startLine, s.endLine });
                            }
                        }
                    });

                std::function<void(const analysis::Scope *)> scanScopes =
                    [&](const analysis::Scope *s)
                    {
                        if (!s)
                        {
                            return;
                        }

                        for (const auto &ref : s->references)
                        {
                            if (ref.name != target.name || ref.isMemberAccess)
                            {
                                continue;
                            }

                            bool isInsideNamespace = false;
                            for (const auto &nr : nsRanges)
                            {
                                if (ref.startLine >= nr.first && ref.startLine <= nr.second)
                                {
                                    isInsideNamespace = true;
                                    break;
                                }
                            }

                            if (!isInsideNamespace && request.tree)
                            {
                                TSNode rootNode = ts_tree_root_node(request.tree);
                                TSPoint pt = { ref.startLine, ref.startCharacter };
                                TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                                if (!ts_node_is_null(refNode))
                                {
                                    TSNode pNode = ts_node_parent(refNode);
                                    if (!ts_node_is_null(pNode) && std::string_view(ts_node_type(pNode)) == "scoped_identifier")
                                    {
                                        uint32_t pStart = ts_node_start_byte(pNode);
                                        uint32_t pEnd = ts_node_end_byte(pNode);
                                        if (pStart < request.sourceCode.size() && pEnd <= request.sourceCode.size())
                                        {
                                            std::string scoped = request.sourceCode.substr(pStart, pEnd - pStart);
                                            if (scoped == target.qualifiedName)
                                            {
                                                isInsideNamespace = true;
                                            }
                                        }
                                    }
                                }
                            }

                            if (!isInsideNamespace)
                            {
                                continue;
                            }

                            const analysis::LocalDefinition *localDef = analysis::ResolveInScope(s, target.name);
                            if (localDef)
                            {
                                if (localDef->kind == analysis::LocalDefinitionKind::Parameter ||
                                    localDef->kind == analysis::LocalDefinitionKind::Variable)
                                {
                                    const analysis::Scope *defScope = FindScopeDeclaringDefinition(rootScope.get(), *localDef);
                                    if (defScope && IsDeclaredInFunctionScope(defScope))
                                    {
                                        continue;
                                    }
                                }
                            }

                            if (seen.insert({ ref.startLine, ref.startCharacter }).second)
                            {
                                ranges.push_back(lsp::Range{
                                    lsp::Position{ ref.startLine, ref.startCharacter },
                                    lsp::Position{ ref.endLine, ref.endCharacter }
                                });
                            }
                        }

                        for (const auto &child : s->children)
                        {
                            scanScopes(child.get());
                        }
                    };

                scanScopes(rootScope.get());
            }
        }
        else // TargetKind::GlobalSymbol
        {
            auto syms = request.symbolTable.FindSymbols(target.name);
            for (const auto &sym : syms)
            {
                if (sym.fileUri == request.uri && sym.type != analysis::SymbolType::CallReference)
                {
                    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                    uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                    uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                    if (seen.insert({ sL, sC }).second)
                    {
                        ranges.push_back(lsp::Range{
                            lsp::Position{ sL, sC },
                            lsp::Position{ eL, eC }
                        });
                    }
                }
            }

            if (rootScope)
            {
                std::function<void(const analysis::Scope *)> scanScopes =
                    [&](const analysis::Scope *s)
                    {
                        if (!s)
                        {
                            return;
                        }

                        for (const auto &ref : s->references)
                        {
                            if (ref.name != target.name || ref.isMemberAccess)
                            {
                                continue;
                            }

                            const analysis::LocalDefinition *localDef = analysis::ResolveInScope(s, target.name);
                            if (localDef)
                            {
                                if (localDef->kind == analysis::LocalDefinitionKind::Parameter ||
                                    localDef->kind == analysis::LocalDefinitionKind::Variable)
                                {
                                    const analysis::Scope *defScope = FindScopeDeclaringDefinition(rootScope.get(), *localDef);
                                    if (defScope && IsDeclaredInFunctionScope(defScope))
                                    {
                                        continue;
                                    }
                                }
                            }

                            if (seen.insert({ ref.startLine, ref.startCharacter }).second)
                            {
                                ranges.push_back(lsp::Range{
                                    lsp::Position{ ref.startLine, ref.startCharacter },
                                    lsp::Position{ ref.endLine, ref.endCharacter }
                                });
                            }
                        }

                        for (const auto &child : s->children)
                        {
                            scanScopes(child.get());
                        }
                    };

                scanScopes(rootScope.get());
            }
        }

        if (ranges.empty())
        {
            return std::nullopt;
        }

        // Classify each occurrence into DocumentHighlight
        DocumentHighlightResult results;
        results.reserve(ranges.size());

        for (const auto &r : ranges)
        {
            lsp::DocumentHighlight hl;
            hl.range = r;
            hl.kind = ClassifyOccurrence(request.sourceCode, request.tree, r, request.symbolTable, request.scopeIndex, request.uri);
            results.push_back(hl);
        }

        std::sort(results.begin(), results.end(),
                  [](const lsp::DocumentHighlight &a, const lsp::DocumentHighlight &b)
                  {
                      if (a.range.start.line != b.range.start.line)
                      {
                          return a.range.start.line < b.range.start.line;
                      }
                      return a.range.start.character < b.range.start.character;
                  });

        return results;
    }
}
