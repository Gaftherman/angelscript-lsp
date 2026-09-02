#include "features/signature_help/SignatureHelpHandler.h"
#include "analysis/SemanticHelpers.h"
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_set>

namespace angel_lsp::features
{
    namespace
    {

        /**
         * @brief Recursively collects the class and interface inheritance hierarchy for a type.
         * @param symbolTable The symbol table to look up class and interface definitions.
         * @param initialTypeName The starting type name.
         * @return Vector of type names in the hierarchy including initialTypeName and its transitive bases.
         */
        std::vector<std::string> GetInheritedTypeHierarchy(const analysis::SymbolTable &symbolTable, const std::string &initialTypeName)
        {
            std::vector<std::string> hierarchy;
            std::unordered_set<std::string> visited;
            std::vector<std::string> queue;

            // MemberOwnerType, not CleanBaseType - the same choice AccessChecker records at its
            // own call site. A `.` on an array reaches the ARRAY's members, and CleanBaseType
            // answers the ELEMENT type: it reduces `array<Item>` to `Item`. This file used to
            // define its own weaker CleanBaseType that shadowed the analysis:: one and stripped
            // neither `[]` nor `array<>`, so `array<Item>` arrived here with its brackets on and
            // matched nothing at all - a template class is registered under its bare name, so the
            // key is `array::insertLast`. Signature help on any array member call returned no
            // signatures at all, in either spelling of the type.
            std::string rootType = analysis::MemberOwnerType(initialTypeName);
            if (rootType.empty())
            {
                return hierarchy;
            }

            visited.insert(rootType);
            queue.push_back(rootType);

            size_t head = 0;
            while (head < queue.size())
            {
                std::string curType = queue[head++];
                hierarchy.push_back(curType);

                auto symbols = symbolTable.FindSymbols(curType);
                for (const auto &sym : symbols)
                {
                    if (sym.type == analysis::SymbolType::Class)
                    {
                        const auto &cls = sym.GetClass();
                        for (const auto &base : cls.bases)
                        {
                            std::string cleanBase = analysis::MemberOwnerType(base);
                            if (!cleanBase.empty() && visited.insert(cleanBase).second)
                            {
                                queue.push_back(cleanBase);
                            }
                        }
                    }
                    else if (sym.type == analysis::SymbolType::Interface)
                    {
                        const auto &iface = sym.GetInterface();
                        for (const auto &base : iface.inheritedInterfaces)
                        {
                            std::string cleanBase = analysis::MemberOwnerType(base);
                            if (!cleanBase.empty() && visited.insert(cleanBase).second)
                            {
                                queue.push_back(cleanBase);
                            }
                        }
                    }
                }
            }

            return hierarchy;
        }

        uint32_t CalculateActiveParameter(const std::string &sourceCode, uint32_t argStartByte, uint32_t cursorByte)
        {
            if (cursorByte <= argStartByte || cursorByte > sourceCode.size())
            {
                return 0;
            }

            uint32_t activeParam = 0;
            int parenDepth = 0;
            int bracketDepth = 0;
            int braceDepth = 0;
            int angleDepth = 0;
            bool inString = false;
            char stringChar = '\0';
            bool inLineComment = false;
            bool inBlockComment = false;

            for (size_t i = argStartByte; i < cursorByte; ++i)
            {
                char c = sourceCode[i];

                if (inLineComment)
                {
                    if (c == '\n')
                    {
                        inLineComment = false;
                    }
                    continue;
                }

                if (inBlockComment)
                {
                    if (c == '*' && i + 1 < cursorByte && sourceCode[i + 1] == '/')
                    {
                        inBlockComment = false;
                        i++; // skip '/'
                    }
                    continue;
                }

                if (inString)
                {
                    if (c == '\\' && i + 1 < cursorByte)
                    {
                        i++;
                    }
                    else if (c == stringChar)
                    {
                        inString = false;
                    }
                    continue;
                }

                // Check for comment starts
                if (c == '/' && i + 1 < cursorByte)
                {
                    if (sourceCode[i + 1] == '/')
                    {
                        inLineComment = true;
                        i++;
                        continue;
                    }
                    else if (sourceCode[i + 1] == '*')
                    {
                        inBlockComment = true;
                        i++;
                        continue;
                    }
                }

                // Check for string starts
                if (c == '"' || c == '\'')
                {
                    inString = true;
                    stringChar = c;
                    continue;
                }

                if (c == '(') parenDepth++;
                else if (c == ')' && parenDepth > 0) parenDepth--;
                else if (c == '[') bracketDepth++;
                else if (c == ']' && bracketDepth > 0) bracketDepth--;
                else if (c == '{') braceDepth++;
                else if (c == '}' && braceDepth > 0) braceDepth--;
                else if (c == '<') angleDepth++;
                else if (c == '>' && angleDepth > 0) angleDepth--;
                else if (c == ',' && parenDepth == 0 && bracketDepth == 0 && braceDepth == 0 && angleDepth == 0)
                {
                    activeParam++;
                }
            }

            return activeParam;
        }

        std::string FormatSignatureLabel(const analysis::Symbol &sym)
        {
            if (sym.type != analysis::SymbolType::Function && sym.type != analysis::SymbolType::Funcdef)
            {
                return "";
            }

            std::ostringstream oss;
            std::string returnType;
            const std::vector<analysis::ParameterInformation> *parameters = nullptr;

            if (sym.type == analysis::SymbolType::Funcdef)
            {
                oss << "funcdef ";
                const auto &sig = sym.GetFuncdef();
                returnType = sig.returnType;
                parameters = &sig.parameters;
            }
            else
            {
                const auto &sig = sym.GetFunction();
                returnType = sig.returnType;
                parameters = &sig.parameters;
            }

            if (!returnType.empty())
            {
                oss << returnType << " ";
            }
            else
            {
                oss << "void ";
            }

            if (!sym.containerName.empty())
            {
                oss << sym.containerName << "::";
            }
            oss << sym.name << "(";

            if (parameters)
            {
                for (size_t i = 0; i < parameters->size(); ++i)
                {
                    if (i > 0)
                    {
                        oss << ", ";
                    }
                    const auto &param = (*parameters)[i];
                    if (!param.typeName.empty())
                    {
                        oss << param.typeName;
                    }
                    if (!param.name.empty())
                    {
                        oss << " " << param.name;
                    }
                    if (!param.defaultValue.empty())
                    {
                        oss << " = " << param.defaultValue;
                    }
                }
            }

            oss << ")";
            return oss.str();
        }
    }

    std::optional<lsp::SignatureHelp> GetSignatureHelp(const SignatureHelpRequest &request)
    {
        if (!request.tree || request.sourceCode.empty())
        {
            return std::nullopt;
        }

        TSNode rootNode = ts_tree_root_node(request.tree);
        TSPoint point = { request.position.line, request.position.character };
        TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

        if (ts_node_is_null(node))
        {
            return std::nullopt;
        }

        // Find enclosing call_expression or argument_list
        TSNode callNode{};
        TSNode argListNode{};

        for (TSNode cur = node; !ts_node_is_null(cur); cur = ts_node_parent(cur))
        {
            std::string_view type = ts_node_type(cur);
            if (type == "argument_list")
            {
                argListNode = cur;
                TSNode parent = ts_node_parent(cur);
                if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "call_expression")
                {
                    callNode = parent;
                    break;
                }
            }
            else if (type == "call_expression")
            {
                callNode = cur;
                break;
            }
        }

        if (ts_node_is_null(callNode))
        {
            return std::nullopt;
        }

        if (ts_node_is_null(argListNode))
        {
            argListNode = ts_node_child_by_field_name(callNode, "arguments", 9);
            if (ts_node_is_null(argListNode))
            {
                uint32_t childCount = ts_node_child_count(callNode);
                for (uint32_t i = 0; i < childCount; ++i)
                {
                    TSNode child = ts_node_child(callNode, i);
                    if (std::string_view(ts_node_type(child)) == "argument_list")
                    {
                        argListNode = child;
                        break;
                    }
                }
            }
        }

        // Determine active parameter
        uint32_t activeParameter = 0;
        if (!ts_node_is_null(argListNode))
        {
            uint32_t argStart = ts_node_start_byte(argListNode);
            // Skip the opening '('
            if (argStart < request.sourceCode.size() && request.sourceCode[argStart] == '(')
            {
                argStart++;
            }

            // Find cursor byte offset
            size_t cursorByte = 0;
            size_t curLine = 0;
            for (size_t i = 0; i < request.sourceCode.size(); ++i)
            {
                if (curLine == request.position.line)
                {
                    cursorByte = i + request.position.character;
                    break;
                }
                if (request.sourceCode[i] == '\n')
                {
                    curLine++;
                }
            }

            if (cursorByte > request.sourceCode.size())
            {
                cursorByte = request.sourceCode.size();
            }

            activeParameter = CalculateActiveParameter(request.sourceCode, argStart, static_cast<uint32_t>(cursorByte));
        }

        // Extract function node
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
            return std::nullopt;
        }

        std::string_view funcType = ts_node_type(funcNode);
        uint32_t fStart = ts_node_start_byte(funcNode);
        uint32_t fEnd = ts_node_end_byte(funcNode);
        if (fStart >= request.sourceCode.size() || fEnd > request.sourceCode.size() || fStart >= fEnd)
        {
            return std::nullopt;
        }

        std::vector<analysis::Symbol> candidateSymbols;

        if (funcType == "member_expression")
        {
            TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
            TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);

            if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
            {
                std::string objText = request.sourceCode.substr(ts_node_start_byte(objNode), ts_node_end_byte(objNode) - ts_node_start_byte(objNode));
                std::string memText = request.sourceCode.substr(ts_node_start_byte(memNode), ts_node_end_byte(memNode) - ts_node_start_byte(memNode));
                std::string receiverTypeName;

                if (objText == "this")
                {
                    request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                    {
                        for (const auto &sym : symbols)
                        {
                            if (sym.type == analysis::SymbolType::Class && sym.fileUri == request.uri)
                            {
                                if (request.position.line >= sym.startLine && request.position.line <= sym.endLine)
                                {
                                    receiverTypeName = sym.name;
                                }
                            }
                        }
                    });
                }
                else
                {
                    auto rootScope = request.scopeIndex.GetRoot(request.uri);
                    if (rootScope)
                    {
                        const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
                        if (scope)
                        {
                            const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, objText);
                            if (def && !def->typeName.empty())
                            {
                                receiverTypeName = analysis::MemberOwnerType(def->typeName);
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
                                receiverTypeName = analysis::MemberOwnerType(var.typeName);
                                break;
                            }
                        }
                    }
                }

                if (!receiverTypeName.empty())
                {
                    auto hierarchy = GetInheritedTypeHierarchy(request.symbolTable, receiverTypeName);
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
            std::string calleeName = request.sourceCode.substr(fStart, fEnd - fStart);
            candidateSymbols = request.symbolTable.FindSymbols(calleeName);
        }

        std::vector<lsp::SignatureInformation> signatures;

        for (const auto &sym : candidateSymbols)
        {
            if (sym.type != analysis::SymbolType::Function && sym.type != analysis::SymbolType::Funcdef)
            {
                continue;
            }

            lsp::SignatureInformation sigInfo;
            sigInfo.label = FormatSignatureLabel(sym);
            const std::vector<analysis::ParameterInformation> *parameters = nullptr;
            if (sym.type == analysis::SymbolType::Function)
            {
                parameters = &sym.GetFunction().parameters;
            }
            else if (sym.type == analysis::SymbolType::Funcdef)
            {
                parameters = &sym.GetFuncdef().parameters;
            }

            std::vector<lsp::ParameterInformation> params;
            if (parameters)
            {
                for (const auto &param : *parameters)
                {
                    lsp::ParameterInformation pInfo;
                    std::string pLabel;
                    if (!param.typeName.empty())
                    {
                        pLabel += param.typeName;
                    }
                    if (!param.name.empty())
                    {
                        if (!pLabel.empty()) pLabel += " ";
                        pLabel += param.name;
                    }
                    if (!param.defaultValue.empty())
                    {
                        pLabel += " = " + param.defaultValue;
                    }
                    pInfo.label = pLabel;
                    params.push_back(std::move(pInfo));
                }
            }

            if (!params.empty())
            {
                sigInfo.parameters = std::move(params);
            }

            signatures.push_back(std::move(sigInfo));
        }

        if (signatures.empty())
        {
            return std::nullopt;
        }

        uint32_t activeSignature = 0;
        for (size_t i = 0; i < signatures.size(); ++i)
        {
            if (signatures[i].parameters.has_value() &&
                activeParameter < signatures[i].parameters->size())
            {
                activeSignature = static_cast<uint32_t>(i);
                break;
            }
        }

        lsp::SignatureHelp result;
        result.signatures = std::move(signatures);
        result.activeSignature = activeSignature;
        result.activeParameter = activeParameter;

        return result;
    }
}
