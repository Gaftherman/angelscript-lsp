#include "analysis/IsolationChecker.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "utils/Utils.h"
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
    namespace
    {
        void EmitAtNode(TSNode node, DiagnosticContext &ctx, std::string_view code, const std::string &arg)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            ctx.EmitAtRange(start.row, start.column, end.row, end.column, code, arg);
        }

        const Scope *FindInnermostScope(const Scope *root, uint32_t line, uint32_t character)
        {
            if (!root)
            {
                return nullptr;
            }

            const Scope *current = root;
            while (true)
            {
                const Scope *narrower = nullptr;
                for (const auto &child : current->children)
                {
                    if (child->startLine < line || (child->startLine == line && child->startCharacter <= character))
                    {
                        if (child->endLine > line || (child->endLine == line && child->endCharacter >= character))
                        {
                            narrower = child.get();
                            break;
                        }
                    }
                }

                if (!narrower)
                {
                    return current;
                }
                current = narrower;
            }
        }

        bool IsLocalVariableOrParameter(const Scope *startScope, std::string_view name)
        {
            const Scope *cur = startScope;
            while (cur)
            {
                for (const auto &def : cur->definitions)
                {
                    if (def.name == name)
                    {
                        const Scope *check = cur;
                        while (check)
                        {
                            if (check->isFunctionScope)
                            {
                                return true;
                            }
                            check = check->parent;
                        }
                        return false;
                    }
                }
                cur = cur->parent;
            }
            return false;
        }

        bool NodeHasModifierToken(TSNode node, std::string_view modifierName, std::string_view sourceCode)
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                std::string text = GetNodeText(child, sourceCode);
                if (text == modifierName)
                {
                    return true;
                }
                uint32_t subCount = ts_node_child_count(child);
                for (uint32_t j = 0; j < subCount; ++j)
                {
                    if (GetNodeText(ts_node_child(child, j), sourceCode) == modifierName)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        void CheckSharedEntityEligibility(std::string_view sourceCode, DiagnosticContext &ctx)
        {
            size_t startPos = 0;
            uint32_t lineNum = 0;

            while (startPos < sourceCode.size())
            {
                size_t endPos = sourceCode.find('\n', startPos);
                if (endPos == std::string_view::npos)
                {
                    endPos = sourceCode.size();
                }

                std::string_view line = sourceCode.substr(startPos, endPos - startPos);
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }

                size_t col = 0;
                while (col < line.size() && (line[col] == ' ' || line[col] == '\t'))
                {
                    ++col;
                }

                std::string_view trimmed = line.substr(col);
                if (trimmed.starts_with("//") || trimmed.starts_with("/*"))
                {
                    // Comment line
                }
                else if (trimmed.starts_with("shared ") || trimmed.starts_with("shared\t"))
                {
                    std::string codePart(trimmed);
                    size_t commentIdx = codePart.find("//");
                    if (commentIdx != std::string::npos)
                    {
                        codePart = codePart.substr(0, commentIdx);
                    }

                    std::string afterShared = codePart.substr(6);
                    while (!afterShared.empty() && (afterShared.front() == ' ' || afterShared.front() == '\t'))
                    {
                        afterShared.erase(afterShared.begin());
                    }

                    bool isAllowed = false;
                    if (afterShared.starts_with("class ") || afterShared.starts_with("class\t") ||
                        afterShared.starts_with("interface ") || afterShared.starts_with("interface\t") ||
                        afterShared.starts_with("enum ") || afterShared.starts_with("enum\t") ||
                        afterShared.starts_with("funcdef ") || afterShared.starts_with("funcdef\t") ||
                        afterShared.starts_with("external "))
                    {
                        isAllowed = true;
                    }
                    else
                    {
                        size_t parenPos = afterShared.find('(');
                        size_t semiPos = afterShared.find(';');
                        size_t eqPos = afterShared.find('=');

                        if (parenPos != std::string::npos &&
                            (semiPos == std::string::npos || parenPos < semiPos) &&
                            (eqPos == std::string::npos || parenPos < eqPos))
                        {
                            isAllowed = true;
                        }
                    }

                    if (!isAllowed)
                    {
                        uint32_t startCol = static_cast<uint32_t>(col);
                        uint32_t endCol = static_cast<uint32_t>(col + 6);
                        ctx.LogRule("CheckSharedEntityEligibility", "as-err-shared-not-allowed-on-entity", {});
                        ctx.EmitAtRange(lineNum, startCol, lineNum, endCol, "as-err-shared-not-allowed-on-entity");
                    }
                }

                startPos = endPos + 1;
                ++lineNum;
            }
        }

        struct IsolationVisitor
        {
            const IsolationCheckRequest &request;
            DiagnosticContext &ctx;
            std::string currentClassName;
            bool isCurrentClassShared = false;

            void Visit(TSNode node, bool inSharedContext)
            {
                if (ts_node_is_null(node))
                {
                    return;
                }

                std::string_view nodeType = ts_node_type(node);

                if (nodeType == "class_declaration")
                {
                    TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
                    std::string className = GetNodeText(nameNode, request.sourceCode);
                    bool classShared = NodeHasModifierToken(node, "shared", request.sourceCode);
                    if (!classShared && !className.empty())
                    {
                        if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(className))
                        {
                            for (const auto &s : *syms)
                            {
                                if (s.type == SymbolType::Class && s.fileUri == ctx.request.fileUri && s.GetClass().modifiers.isShared)
                                {
                                    classShared = true;
                                    break;
                                }
                            }
                        }
                    }

                    std::string oldClassName = currentClassName;
                    bool oldClassShared = isCurrentClassShared;
                    currentClassName = className;
                    isCurrentClassShared = classShared;

                    uint32_t count = ts_node_child_count(node);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        Visit(ts_node_child(node, i), classShared);
                    }

                    currentClassName = oldClassName;
                    isCurrentClassShared = oldClassShared;
                    return;
                }

                if (nodeType == "func_declaration" || nodeType == "function_definition")
                {
                    bool funcShared = inSharedContext || NodeHasModifierToken(node, "shared", request.sourceCode);
                    if (!funcShared)
                    {
                        TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
                        std::string funcName = GetNodeText(nameNode, request.sourceCode);
                        if (!funcName.empty())
                        {
                            std::string searchName = currentClassName.empty() ? funcName : currentClassName + "::" + funcName;
                            if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(searchName))
                            {
                                for (const auto &s : *syms)
                                {
                                    if (s.type == SymbolType::Function && s.fileUri == ctx.request.fileUri && s.GetFunction().modifiers.isShared)
                                    {
                                        funcShared = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    uint32_t count = ts_node_child_count(node);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        Visit(ts_node_child(node, i), funcShared);
                    }
                    return;
                }

                if (inSharedContext)
                {
                    // 1. Check type references (e.g. NonSharedClass obj;)
                    if (nodeType == "datatype")
                    {
                        std::string typeName = GetNodeText(node, request.sourceCode);
                        while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t')) typeName.erase(typeName.begin());
                        while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t' || typeName.back() == '@' || typeName.back() == '&')) typeName.pop_back();

                        if (!typeName.empty() && !IsCorePrimitive(typeName) &&
                            typeName != ctx.request.GetStringTypeName() &&
                            typeName != ctx.request.GetArrayTypeName() &&
                            !ctx.request.IsRegisteredSymbol(typeName))
                        {
                            if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(typeName))
                            {
                                for (const auto &s : *syms)
                                {
                                    if (s.type == SymbolType::Class || s.type == SymbolType::Interface ||
                                        s.type == SymbolType::Enum || s.type == SymbolType::Funcdef)
                                    {
                                        bool isShared = false;
                                        if (s.type == SymbolType::Class) isShared = s.GetClass().modifiers.isShared;
                                        else if (s.type == SymbolType::Interface) isShared = s.GetInterface().modifiers.isShared;
                                        else if (s.type == SymbolType::Enum) isShared = s.GetEnum().modifiers.isShared;
                                        else if (s.type == SymbolType::Funcdef) isShared = s.GetFuncdef().modifiers.isShared;

                                        if (!isShared && !IsFromPredefinedStub(s, ctx))
                                        {
                                            ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
                                            EmitAtNode(node, ctx, "as-err-shared-cannot-access-non-shared", typeName);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 2. Check direct function calls (e.g. NonSharedFunction())
                    if (nodeType == "call_expression")
                    {
                        TSNode funcNode = ts_node_child_by_field_name(node, "function", 8);
                        if (ts_node_is_null(funcNode))
                        {
                            funcNode = ts_node_child(node, 0);
                        }
                        if (!ts_node_is_null(funcNode))
                        {
                            std::string_view funcNodeType = ts_node_type(funcNode);
                            if (funcNodeType == "scoped_identifier" || funcNodeType == "identifier")
                            {
                                std::string calleeName = GetNodeText(funcNode, request.sourceCode);
                                while (!calleeName.empty() && (calleeName.front() == ' ' || calleeName.front() == '\t')) calleeName.erase(calleeName.begin());
                                while (!calleeName.empty() && (calleeName.back() == ' ' || calleeName.back() == '\t')) calleeName.pop_back();

                                bool isCurrentClassMethod = false;
                                if (!currentClassName.empty())
                                {
                                    std::string methodQual = currentClassName + "::" + calleeName;
                                    if (ctx.request.symbolTable.HasSymbol(methodQual))
                                    {
                                        isCurrentClassMethod = true;
                                    }
                                }

                                if (!isCurrentClassMethod && !ctx.request.IsRegisteredSymbol(calleeName))
                                {
                                    if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(calleeName))
                                    {
                                        for (const auto &s : *syms)
                                        {
                                            if (s.type == SymbolType::Function && s.containerName.empty())
                                            {
                                                if (!s.GetFunction().modifiers.isShared && !IsFromPredefinedStub(s, ctx))
                                                {
                                                    ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
                                                    EmitAtNode(funcNode, ctx, "as-err-shared-cannot-access-non-shared", calleeName);
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // 3. Check global variable access
                    if (nodeType == "identifier")
                    {
                        TSNode p = ts_node_parent(node);
                        bool isCallFunc = false;
                        bool isType = false;
                        bool isDeclName = false;
                        bool isMemberProp = false;

                        while (!ts_node_is_null(p))
                        {
                            std::string_view pType = ts_node_type(p);
                            if (pType == "call_expression")
                            {
                                TSNode fn = ts_node_child_by_field_name(p, "function", 8);
                                if (ts_node_is_null(fn)) fn = ts_node_child(p, 0);
                                if (!ts_node_is_null(fn) && (fn.id == node.id || ts_node_parent(node).id == fn.id))
                                {
                                    isCallFunc = true;
                                    break;
                                }
                            }
                            if (pType == "datatype" || pType == "type_identifier" || pType == "type" || pType == "base_class_list")
                            {
                                isType = true;
                                break;
                            }
                            if (pType == "variable_declarator")
                            {
                                TSNode val = ts_node_child_by_field_name(p, "value", 5);
                                if (ts_node_is_null(val) || ts_node_start_byte(node) < ts_node_start_byte(val))
                                {
                                    isDeclName = true;
                                    break;
                                }
                            }
                            if (pType == "func_declaration" || pType == "function_definition" ||
                                pType == "class_declaration" || pType == "parameter" || pType == "enum_member")
                            {
                                TSNode nameNode = ts_node_child_by_field_name(p, "name", 4);
                                if (!ts_node_is_null(nameNode) && (nameNode.id == node.id || ts_node_start_byte(node) == ts_node_start_byte(nameNode)))
                                {
                                    isDeclName = true;
                                    break;
                                }
                            }
                            if (pType == "member_expression" || pType == "field_expression")
                            {
                                TSNode propNode = ts_node_child_by_field_name(p, "property", 8);
                                if (!ts_node_is_null(propNode) && propNode.id == node.id)
                                {
                                    isMemberProp = true;
                                    break;
                                }
                            }
                            if (pType == "statement_block" || pType == "compound_statement" || pType == "func_declaration" || pType == "class_declaration")
                            {
                                break;
                            }
                            p = ts_node_parent(p);
                        }

                        if (!isCallFunc && !isMemberProp && !isType && !isDeclName)
                        {
                            std::string varName = GetNodeText(node, request.sourceCode);
                            while (!varName.empty() && (varName.front() == ' ' || varName.front() == '\t')) varName.erase(varName.begin());
                            while (!varName.empty() && (varName.back() == ' ' || varName.back() == '\t')) varName.pop_back();

                            TSPoint pt = ts_node_start_point(node);
                            const Scope *scope = request.scopeRoot ? FindInnermostScope(request.scopeRoot, pt.row, pt.column) : nullptr;
                            bool isLocal = scope ? IsLocalVariableOrParameter(scope, varName) : false;

                            if (!isLocal)
                            {
                                bool isClassMember = false;
                                if (!currentClassName.empty())
                                {
                                    std::string memberQual = currentClassName + "::" + varName;
                                    if (ctx.request.symbolTable.HasSymbol(memberQual))
                                    {
                                        isClassMember = true;
                                    }
                                }

                                if (!isClassMember && !ctx.request.IsRegisteredSymbol(varName))
                                {
                                    if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(varName))
                                    {
                                        for (const auto &s : *syms)
                                        {
                                            if (s.type == SymbolType::Variable && s.containerName.empty())
                                            {
                                                if (!s.GetVariable().modifiers.isShared && !IsFromPredefinedStub(s, ctx))
                                                {
                                                    ctx.LogRule("CheckSharedIsolation", "as-err-shared-cannot-access-non-shared", s);
                                                    EmitAtNode(node, ctx, "as-err-shared-cannot-access-non-shared", varName);
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; ++i)
                {
                    Visit(ts_node_child(node, i), inSharedContext);
                }
            }
        };
    }

    void CheckSharedIsolation(const IsolationCheckRequest &request, DiagnosticContext &ctx)
    {
        if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
        {
            return;
        }

        CheckSharedEntityEligibility(request.sourceCode, ctx);

        IsolationVisitor visitor{request, ctx, "", false};
        visitor.Visit(request.rootNode, false);
    }
}
