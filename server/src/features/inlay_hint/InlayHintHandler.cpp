#include "features/inlay_hint/InlayHintHandler.h"
#include "analysis/SemanticHelpers.h"
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <unordered_set>

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
         * @brief Checks if a given line and character position falls within a scope's range.
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
         * @brief Checks if a position is within the requested range (or if range is unbounded).
         */
        bool IsPositionInRange(const lsp::Position &pos, const lsp::Range &range)
        {
            if (range.start.line == 0 && range.start.character == 0 &&
                range.end.line == 0 && range.end.character == 0)
            {
                return true;
            }
            if (pos.line < range.start.line || pos.line > range.end.line)
            {
                return false;
            }
            if (pos.line == range.start.line && pos.character < range.start.character)
            {
                return false;
            }
            if (pos.line == range.end.line && pos.character > range.end.character)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief Checks if an AST node's line range overlaps with the requested range.
         */
        bool IsNodeOverlappingRange(TSNode node, const lsp::Range &range)
        {
            if (range.start.line == 0 && range.start.character == 0 &&
                range.end.line == 0 && range.end.character == 0)
            {
                return true;
            }
            TSPoint startPoint = ts_node_start_point(node);
            TSPoint endPoint = ts_node_end_point(node);
            if (endPoint.row < range.start.line || startPoint.row > range.end.line)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief Resolves parameters for a function, method, or constructor called by a call_expression node.
         */
        std::vector<analysis::ParameterInformation> ResolveCalleeParameters(
            TSNode callNode,
            const InlayHintRequest &request,
            size_t numArgs)
        {
            TSNode funcNode = ts_node_child_by_field_name(callNode, "function", 8);
            if (ts_node_is_null(funcNode))
            {
                uint32_t childCount = ts_node_child_count(callNode);
                if (childCount > 0)
                {
                    funcNode = ts_node_child(callNode, 0);
                }
            }

            if (ts_node_is_null(funcNode))
            {
                return {};
            }

            std::string_view funcType = ts_node_type(funcNode);
            std::vector<analysis::Symbol> candidateSymbols;

            if (funcType == "member_expression")
            {
                TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);

                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    std::string objText = GetNodeText(objNode, request.sourceCode);
                    std::string memText = GetNodeText(memNode, request.sourceCode);
                    std::string receiverTypeName;

                    if (objText == "this")
                    {
                        auto containers = analysis::GetEnclosingContainers(callNode, request.sourceCode);
                        for (const auto &c : containers)
                        {
                            if (c.kind == analysis::ContainerKind::Class)
                            {
                                receiverTypeName = c.name;
                                break;
                            }
                        }
                    }
                    else
                    {
                        auto rootScope = request.scopeIndex.GetRoot(request.uri);
                        if (rootScope)
                        {
                            TSPoint objPoint = ts_node_start_point(objNode);
                            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), objPoint.row, objPoint.column);
                            if (scope)
                            {
                                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, objText);
                                if (def && !def->typeName.empty())
                                {
                                    receiverTypeName = analysis::CleanBaseType(def->typeName);
                                }
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
                        auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, request.symbolTable);
                        for (const auto &typeName : hierarchy)
                        {
                            std::string qualifiedName = typeName + "::" + memText;
                            auto found = request.symbolTable.FindSymbols(qualifiedName);
                            if (!found.empty())
                            {
                                candidateSymbols = std::move(found);
                                break;
                            }
                        }
                    }
                }
            }
            else
            {
                std::string calleeName = GetNodeText(funcNode, request.sourceCode);
                candidateSymbols = analysis::FindSymbolsInScope(calleeName, callNode, request.sourceCode, request.symbolTable);
                if (candidateSymbols.empty())
                {
                    candidateSymbols = request.symbolTable.FindSymbols(calleeName);
                }

                // If symbol is a Class, look for its constructor
                for (const auto &sym : candidateSymbols)
                {
                    if (sym.type == analysis::SymbolType::Class)
                    {
                        std::string ctorName = sym.name + "::" + sym.name;
                        auto ctorSyms = request.symbolTable.FindSymbols(ctorName);
                        if (!ctorSyms.empty())
                        {
                            candidateSymbols = std::move(ctorSyms);
                            break;
                        }
                    }
                }
            }

            const analysis::Symbol *bestSym = nullptr;
            for (const auto &sym : candidateSymbols)
            {
                if (sym.type == analysis::SymbolType::Function)
                {
                    const auto &fn = sym.GetFunction();
                    if (fn.parameters.size() >= numArgs)
                    {
                        bestSym = &sym;
                        if (fn.parameters.size() == numArgs)
                        {
                            break;
                        }
                    }
                }
                else if (sym.type == analysis::SymbolType::Funcdef)
                {
                    const auto &fn = sym.GetFuncdef();
                    if (fn.parameters.size() >= numArgs)
                    {
                        bestSym = &sym;
                        if (fn.parameters.size() == numArgs)
                        {
                            break;
                        }
                    }
                }
            }

            if (!bestSym && !candidateSymbols.empty())
            {
                for (const auto &sym : candidateSymbols)
                {
                    if (sym.type == analysis::SymbolType::Function || sym.type == analysis::SymbolType::Funcdef)
                    {
                        bestSym = &sym;
                        break;
                    }
                }
            }

            if (bestSym)
            {
                if (bestSym->type == analysis::SymbolType::Function)
                {
                    return bestSym->GetFunction().parameters;
                }
                else if (bestSym->type == analysis::SymbolType::Funcdef)
                {
                    return bestSym->GetFuncdef().parameters;
                }
            }

            return {};
        }

        struct ArgInfo
        {
            TSNode exprNode;
            bool isNamed = false;
            std::string argName;
            lsp::Position hintPosition;
            std::string text;
        };

        /**
         * @brief Parses an argument_list node into structured ArgInfo items.
         */
        std::vector<ArgInfo> ParseArguments(TSNode argListNode, std::string_view sourceCode)
        {
            std::vector<ArgInfo> args;
            uint32_t childCount = ts_node_child_count(argListNode);
            bool currentIsNamed = false;
            std::string currentArgName;

            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(argListNode, i);
                std::string_view type = ts_node_type(child);

                if (type == "(" || type == ")" || type == ",")
                {
                    if (type == ",")
                    {
                        currentIsNamed = false;
                        currentArgName.clear();
                    }
                    continue;
                }

                if (type == ":")
                {
                    continue;
                }

                const char *fieldName = ts_node_field_name_for_child(argListNode, i);
                if (fieldName && std::string_view(fieldName) == "arg_name")
                {
                    currentIsNamed = true;
                    currentArgName = GetNodeText(child, sourceCode);
                    continue;
                }

                ArgInfo arg;
                arg.exprNode = child;
                arg.isNamed = currentIsNamed;
                arg.argName = currentArgName;
                TSPoint startPoint = ts_node_start_point(child);
                arg.hintPosition = lsp::Position{ startPoint.row, startPoint.column };
                arg.text = GetNodeText(child, sourceCode);
                args.push_back(std::move(arg));

                currentIsNamed = false;
                currentArgName.clear();
            }

            return args;
        }

        /**
         * @brief Forward declaration for type deduction helper.
         */
        std::string DeduceExpressionType(TSNode exprNode, const InlayHintRequest &request);

        /**
         * @brief Recursively deduces the type string for an expression AST node.
         */
        std::string DeduceExpressionType(TSNode exprNode, const InlayHintRequest &request)
        {
            if (ts_node_is_null(exprNode))
            {
                return "";
            }

            std::string_view type = ts_node_type(exprNode);

            if (type == "parenthesized_expression")
            {
                uint32_t count = ts_node_child_count(exprNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_child(exprNode, i);
                    std::string_view cType = ts_node_type(child);
                    if (cType != "(" && cType != ")")
                    {
                        return DeduceExpressionType(child, request);
                    }
                }
                return "";
            }

            std::string nodeTxt = GetNodeText(exprNode, request.sourceCode);
            while (!nodeTxt.empty() && (nodeTxt.front() == ' ' || nodeTxt.front() == '\t' || nodeTxt.front() == '('))
            {
                nodeTxt.erase(nodeTxt.begin());
            }
            while (!nodeTxt.empty() && (nodeTxt.back() == ' ' || nodeTxt.back() == '\t' || nodeTxt.back() == ')' || nodeTxt.back() == ';'))
            {
                nodeTxt.pop_back();
            }

            if (!nodeTxt.empty() && (isdigit(static_cast<unsigned char>(nodeTxt[0])) ||
                (nodeTxt.size() > 1 && (nodeTxt[0] == '-' || nodeTxt[0] == '+') && isdigit(static_cast<unsigned char>(nodeTxt[1]))) ||
                nodeTxt.starts_with("0x") || nodeTxt.starts_with("0X") ||
                nodeTxt.starts_with("0b") || nodeTxt.starts_with("0B") ||
                nodeTxt.starts_with("0o") || nodeTxt.starts_with("0O") ||
                type == "number_literal" || type == "integer_literal" || type == "real_literal" || type == "float_literal"))
            {
                if (nodeTxt.find('.') != std::string::npos || nodeTxt.find('e') != std::string::npos || nodeTxt.find('E') != std::string::npos)
                {
                    if (nodeTxt.back() == 'f' || nodeTxt.back() == 'F') return "float";
                    if (nodeTxt.back() == 'd' || nodeTxt.back() == 'D') return "double";
                    return "double";
                }
                // AngelScript only defines f/F and d/D suffixes, and only on floating-point
                // literals. The C-style integer suffixes this used to sniff for (42u, 1000L,
                // 2000u64) are not part of the language: the grammar tokenises decimal_int as
                // plain /[0-9]+/, so "42u" never reaches here as one literal - it parses as 42
                // followed by a stray identifier. Guessing a type from them invented information.
                if (nodeTxt.back() == 'f' || nodeTxt.back() == 'F') return "float";
                if (nodeTxt.back() == 'd' || nodeTxt.back() == 'D') return "double";
                return "int";
            }

            if (type == "string_literal" || type == "concatenated_string")
            {
                return "string";
            }

            if (type == "boolean_literal")
            {
                return "bool";
            }

            if (type == "null_literal")
            {
                return "";
            }

            if (type == "functional_cast_expression")
            {
                TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
                if (!ts_node_is_null(typeNode))
                {
                    return GetNodeText(typeNode, request.sourceCode);
                }
            }

            if (type == "cast_expression")
            {
                TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
                if (!ts_node_is_null(typeNode))
                {
                    return GetNodeText(typeNode, request.sourceCode);
                }
            }

            if (type == "construct_call_expression")
            {
                TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
                if (!ts_node_is_null(typeNode))
                {
                    std::string cType = GetNodeText(typeNode, request.sourceCode);
                    uint32_t count = ts_node_child_count(exprNode);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_child(exprNode, i);
                        if (std::string_view(ts_node_type(child)) == "template_type_list")
                        {
                            cType += GetNodeText(child, request.sourceCode);
                            break;
                        }
                    }
                    return cType;
                }
            }

            if (type == "call_expression")
            {
                TSNode funcNode = ts_node_child_by_field_name(exprNode, "function", 8);
                if (ts_node_is_null(funcNode))
                {
                    uint32_t childCount = ts_node_child_count(exprNode);
                    if (childCount > 0)
                    {
                        funcNode = ts_node_child(exprNode, 0);
                    }
                }

                if (!ts_node_is_null(funcNode))
                {
                    std::string_view fType = ts_node_type(funcNode);
                    if (fType == "member_expression")
                    {
                        TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                        TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);
                        if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                        {
                            std::string objText = GetNodeText(objNode, request.sourceCode);
                            std::string memText = GetNodeText(memNode, request.sourceCode);
                            std::string receiverType;

                            if (objText == "this")
                            {
                                auto containers = analysis::GetEnclosingContainers(exprNode, request.sourceCode);
                                for (const auto &c : containers)
                                {
                                    if (c.kind == analysis::ContainerKind::Class)
                                    {
                                        receiverType = c.name;
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                auto rootScope = request.scopeIndex.GetRoot(request.uri);
                                if (rootScope)
                                {
                                    TSPoint objPoint = ts_node_start_point(objNode);
                                    const analysis::Scope *scope = FindInnermostScope(rootScope.get(), objPoint.row, objPoint.column);
                                    if (scope)
                                    {
                                        const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, objText);
                                        if (def && !def->typeName.empty())
                                        {
                                            receiverType = analysis::CleanBaseType(def->typeName);
                                        }
                                    }
                                }
                            }

                            if (!receiverType.empty())
                            {
                                auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverType, request.symbolTable);
                                for (const auto &typeName : hierarchy)
                                {
                                    std::string qualifiedName = typeName + "::" + memText;
                                    auto found = request.symbolTable.FindSymbols(qualifiedName);
                                    for (const auto &sym : found)
                                    {
                                        if (sym.type == analysis::SymbolType::Function)
                                        {
                                            return sym.GetFunction().returnType;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        std::string fName = GetNodeText(funcNode, request.sourceCode);
                        auto candidates = analysis::FindSymbolsInScope(fName, exprNode, request.sourceCode, request.symbolTable);
                        if (candidates.empty())
                        {
                            candidates = request.symbolTable.FindSymbols(fName);
                        }

                        for (const auto &sym : candidates)
                        {
                            if (sym.type == analysis::SymbolType::Function)
                            {
                                return sym.GetFunction().returnType;
                            }
                            else if (sym.type == analysis::SymbolType::Funcdef)
                            {
                                return sym.GetFuncdef().returnType;
                            }
                            else if (sym.type == analysis::SymbolType::Class)
                            {
                                return sym.name;
                            }
                        }
                    }
                }
            }

            if (type == "member_expression")
            {
                TSNode objNode = ts_node_child_by_field_name(exprNode, "object", 6);
                TSNode memNode = ts_node_child_by_field_name(exprNode, "member", 6);
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    std::string objText = GetNodeText(objNode, request.sourceCode);
                    std::string memText = GetNodeText(memNode, request.sourceCode);
                    std::string receiverType;

                    auto rootScope = request.scopeIndex.GetRoot(request.uri);
                    if (rootScope)
                    {
                        TSPoint objPoint = ts_node_start_point(objNode);
                        const analysis::Scope *scope = FindInnermostScope(rootScope.get(), objPoint.row, objPoint.column);
                        if (scope)
                        {
                            const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, objText);
                            if (def && !def->typeName.empty())
                            {
                                receiverType = analysis::CleanBaseType(def->typeName);
                            }
                        }
                    }

                    if (!receiverType.empty())
                    {
                        auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverType, request.symbolTable);
                        for (const auto &typeName : hierarchy)
                        {
                            std::string qualifiedName = typeName + "::" + memText;
                            auto found = request.symbolTable.FindSymbols(qualifiedName);
                            for (const auto &sym : found)
                            {
                                if (sym.type == analysis::SymbolType::Variable)
                                {
                                    return sym.GetVariable().typeName;
                                }
                                else if (sym.type == analysis::SymbolType::Function)
                                {
                                    return sym.GetFunction().returnType;
                                }
                            }
                        }
                    }
                }
            }

            if (type == "identifier" || type == "scoped_identifier")
            {
                std::string name = GetNodeText(exprNode, request.sourceCode);
                auto rootScope = request.scopeIndex.GetRoot(request.uri);
                if (rootScope)
                {
                    TSPoint point = ts_node_start_point(exprNode);
                    const analysis::Scope *scope = FindInnermostScope(rootScope.get(), point.row, point.column);
                    if (scope)
                    {
                        const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, name);
                        if (def && !def->typeName.empty())
                        {
                            return def->typeName;
                        }
                    }
                }

                auto symbols = request.symbolTable.FindSymbols(name);
                for (const auto &sym : symbols)
                {
                    if (sym.type == analysis::SymbolType::Variable)
                    {
                        return sym.GetVariable().typeName;
                    }
                }
            }

            if (type == "binary_expression")
            {
                TSNode opNode = ts_node_child_by_field_name(exprNode, "operator", 8);
                std::string op = GetNodeText(opNode, request.sourceCode);

                if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
                    op == "&&" || op == "||" || op == "and" || op == "or" || op == "xor" || op == "^^" ||
                    op == "is" || op == "!is")
                {
                    return "bool";
                }

                TSNode left = ts_node_child_by_field_name(exprNode, "left", 4);
                TSNode right = ts_node_child_by_field_name(exprNode, "right", 5);
                std::string leftT = DeduceExpressionType(left, request);
                std::string rightT = DeduceExpressionType(right, request);

                if (leftT == "double" || rightT == "double") return "double";
                if (leftT == "float" || rightT == "float") return "float";
                if (leftT == "string" || rightT == "string") return "string";
                if (leftT == "int64" || rightT == "int64") return "int64";
                if (leftT == "uint" || rightT == "uint") return "uint";
                if (!leftT.empty()) return leftT;
                if (!rightT.empty()) return rightT;
                return "int";
            }

            if (type == "unary_expression")
            {
                TSNode opNode = ts_node_child_by_field_name(exprNode, "operator", 8);
                std::string op = GetNodeText(opNode, request.sourceCode);
                TSNode operand = ts_node_child_by_field_name(exprNode, "operand", 7);

                if (op == "!" || op == "not")
                {
                    return "bool";
                }
                if (op == "@")
                {
                    std::string opT = DeduceExpressionType(operand, request);
                    if (!opT.empty() && !opT.ends_with("@"))
                    {
                        return opT + "@";
                    }
                    return opT;
                }
                return DeduceExpressionType(operand, request);
            }

            if (type == "postfix_expression")
            {
                TSNode operand = ts_node_child_by_field_name(exprNode, "operand", 7);
                return DeduceExpressionType(operand, request);
            }

            return "";
        }

        /**
         * @brief Recursively traverses the AST and collects inlay hints.
         */
        void CollectInlayHintsFromNode(
            TSNode node,
            const InlayHintRequest &request,
            std::vector<lsp::InlayHint> &hints)
        {
            if (ts_node_is_null(node))
            {
                return;
            }

            if (!IsNodeOverlappingRange(node, request.range))
            {
                return;
            }

            std::string_view nodeType = ts_node_type(node);

            // 1. Process call_expression for parameter name hints
            if (nodeType == "call_expression")
            {
                TSNode argListNode = ts_node_child_by_field_name(node, "arguments", 9);
                if (ts_node_is_null(argListNode))
                {
                    uint32_t childCount = ts_node_child_count(node);
                    for (uint32_t i = 0; i < childCount; ++i)
                    {
                        TSNode child = ts_node_child(node, i);
                        if (std::string_view(ts_node_type(child)) == "argument_list")
                        {
                            argListNode = child;
                            break;
                        }
                    }
                }

                if (!ts_node_is_null(argListNode))
                {
                    auto args = ParseArguments(argListNode, request.sourceCode);
                    auto parameters = ResolveCalleeParameters(node, request, args.size());

                    for (size_t i = 0; i < args.size() && i < parameters.size(); ++i)
                    {
                        const auto &param = parameters[i];
                        const auto &arg = args[i];

                        // Exclusion Rule 1: Already named in syntax
                        if (arg.isNamed)
                        {
                            continue;
                        }

                        // Exclusion Rule 2: Empty or varargs
                        if (param.name.empty() || param.name == "...")
                        {
                            continue;
                        }

                        // Exclusion Rule 3: Argument variable text matches parameter name exactly
                        if (arg.text == param.name)
                        {
                            continue;
                        }

                        if (IsPositionInRange(arg.hintPosition, request.range))
                        {
                            lsp::InlayHint hint;
                            hint.position = arg.hintPosition;
                            hint.label = param.name + ":";
                            hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Parameter);
                            hint.paddingRight = true;
                            hint.paddingLeft = false;
                            std::string tooltip = "Parameter: " + param.typeName;
                            if (!param.name.empty())
                            {
                                tooltip += " " + param.name;
                            }
                            hint.tooltip = tooltip;
                            hints.push_back(std::move(hint));
                        }
                    }
                }
            }

            // 2. Process variable_declaration for auto type deduction hints
            if (nodeType == "variable_declaration")
            {
                TSNode varTypeNode = ts_node_child_by_field_name(node, "var_type", 8);
                if (ts_node_is_null(varTypeNode))
                {
                    uint32_t count = ts_node_child_count(node);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_child(node, i);
                        if (std::string_view(ts_node_type(child)) == "type")
                        {
                            varTypeNode = child;
                            break;
                        }
                    }
                }

                if (!ts_node_is_null(varTypeNode))
                {
                    std::string typeText = GetNodeText(varTypeNode, request.sourceCode);
                    if (typeText == "auto")
                    {
                        uint32_t count = ts_node_child_count(node);
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            TSNode child = ts_node_child(node, i);
                            if (std::string_view(ts_node_type(child)) == "variable_declarator")
                            {
                                TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                                if (ts_node_is_null(nameNode))
                                {
                                    continue;
                                }

                                // The grammar names both initialiser shapes: "value" for "= expr"
                                // and "arguments" for a constructor call such as "Player p(1, 2)".
                                // This used to be a manual child walk hunting for the "=" token,
                                // which is what an unnamed initialiser forced.
                                TSNode initExpr = ts_node_child_by_field_name(child, "value", 5);
                                if (ts_node_is_null(initExpr))
                                {
                                    initExpr = ts_node_child_by_field_name(child, "arguments", 9);
                                }


                                if (!ts_node_is_null(initExpr))
                                {
                                    // The initialiser type comes from the AST alone. This used to be
                                    // followed by a second pass that re-read the declaration as raw
                                    // text, hunting for the '=' with substr and sniffing C-style
                                    // integer suffixes off the tail - suffixes AngelScript does not
                                    // have, on text that does not parse as one literal when present.
                                    std::string deduced = DeduceExpressionType(initExpr, request);

                                    if (!deduced.empty() && deduced != "auto")
                                    {
                                        TSPoint endPoint = ts_node_end_point(nameNode);
                                        lsp::Position hintPos{ endPoint.row, endPoint.column };
                                        if (IsPositionInRange(hintPos, request.range))
                                        {
                                            lsp::InlayHint hint;
                                            hint.position = hintPos;
                                            hint.label = ": " + deduced;
                                            hint.kind = lsp::InlayHintKindEnum(lsp::InlayHintKind::Type);
                                            hint.paddingLeft = true;
                                            hint.paddingRight = false;
                                            hint.tooltip = "Deduced type: " + deduced;
                                            hints.push_back(std::move(hint));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Recurse on children
            uint32_t childCount = ts_node_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                CollectInlayHintsFromNode(ts_node_child(node, i), request, hints);
            }
        }
    }

    std::optional<InlayHintResult> GetInlayHints(const InlayHintRequest &request)
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

        std::vector<lsp::InlayHint> hints;
        CollectInlayHintsFromNode(rootNode, request, hints);

        // Sort hints by source position
        std::sort(hints.begin(), hints.end(), [](const lsp::InlayHint &a, const lsp::InlayHint &b)
        {
            if (a.position.line != b.position.line)
            {
                return a.position.line < b.position.line;
            }
            return a.position.character < b.position.character;
        });

        return hints;
    }
}
