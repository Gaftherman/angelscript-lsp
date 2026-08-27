#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/OverloadResolver.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SignatureFormatter.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace angel_lsp::test
{
    struct Position
    {
        uint32_t line = 0;
        uint32_t character = 0;
    };

    struct ResolvedCallInfo
    {
        std::string targetFunctionSymbol;
    };

    inline std::string FormatParamType(const analysis::ParameterInformation &p)
    {
        std::string s = p.typeName;
        if (p.isConst && s.rfind("const ", 0) != 0)
        {
            s = "const " + s;
        }
        if (p.isHandle && s.find('@') == std::string::npos)
        {
            s += "@";
        }
        std::string ref = analysis::FormatParameterReference(p);
        if (!ref.empty())
        {
            if (!s.empty()) s += " ";
            s += ref;
        }
        return s;
    }

    inline std::string GetNodeText(TSNode node, std::string_view sourceCode)
    {
        if (ts_node_is_null(node))
        {
            return "";
        }
        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start < sourceCode.size() && end <= sourceCode.size() && start <= end)
        {
            return std::string(sourceCode.substr(start, end - start));
        }
        return "";
    }

    inline const analysis::Scope *FindInnermostScope(const analysis::Scope *root, uint32_t line, uint32_t character)
    {
        if (!root)
        {
            return nullptr;
        }
        for (const auto &child : root->children)
        {
            if (child && line >= child->startLine && line <= child->endLine)
            {
                if (const analysis::Scope *inner = FindInnermostScope(child.get(), line, character))
                {
                    return inner;
                }
                return child.get();
            }
        }
        return root;
    }

    class TestDocument;

    inline std::map<std::string, std::weak_ptr<TestDocument>>& GetTestWorkspaceRegistry()
    {
        static std::map<std::string, std::weak_ptr<TestDocument>> registry;
        return registry;
    }

    class TestDocument
    {
    public:
        TestDocument(const std::string &uri, const std::string &sourceCode)
            : m_uri(uri), m_sourceCode(sourceCode)
        {
            m_tree = m_parser.Parse(m_sourceCode);
            static angel_lsp::i18n::I18n i18n;

            auto &registry = GetTestWorkspaceRegistry();
            for (auto it = registry.begin(); it != registry.end(); )
            {
                if (auto otherDoc = it->second.lock())
                {
                    if (it->first != m_uri)
                    {
                        otherDoc->GetSymbolTable().ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                        {
                            for (const auto &s : symbols)
                            {
                                m_symbolTable.AddSymbol(s);
                            }
                        });
                    }
                    ++it;
                }
                else
                {
                    it = registry.erase(it);
                }
            }

            auto collectDiags = m_symbolCollector.CollectSymbolsWithTree(m_uri, m_sourceCode, m_tree, m_symbolTable, &i18n);
            m_diagnostics.insert(m_diagnostics.end(), collectDiags.begin(), collectDiags.end());

            analysis::SemanticAnalysisRequest request{ m_symbolTable, m_uri, ".as.predefined", &i18n };
            m_scopeRoot = m_scopeCollector.CollectScopes(m_sourceCode, m_parser);
            request.scopeRoot = m_scopeRoot;
            request.sourceCode = m_sourceCode;
            request.tree = m_tree;

            analysis::SemanticAnalyzer analyzer(nullptr);
            auto semDiags = analyzer.Analyze(request);
            m_diagnostics.insert(m_diagnostics.end(), semDiags.begin(), semDiags.end());
        }

        ~TestDocument()
        {
            if (m_tree)
            {
                ts_tree_delete(m_tree);
            }
        }

        std::vector<analysis::Diagnostic> GetDiagnostics() const
        {
            std::vector<analysis::Diagnostic> rawErrors;
            ankerl::unordered_dense::set<uint32_t> linesWithSpecificErrors;

            for (const auto &d : m_diagnostics)
            {
                if (d.severity == analysis::DiagnosticSeverity::Error)
                {
                    analysis::Diagnostic copy = d;
                    if (copy.code == "as-err-duplicate-symbol")
                    {
                        copy.code = "E_DUPLICATE_DECLARATION";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-typedef-non-primitive")
                    {
                        copy.code = "E_TYPEDEF_ONLY_PRIMITIVE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-unresolved-type")
                    {
                        copy.code = "E_UNKNOWN_TYPE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-signature-mismatch-func-handle")
                    {
                        copy.code = "E_SIGNATURE_MISMATCH_FUNC_HANDLE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-mixin-not-a-type")
                    {
                        copy.code = "E_CANNOT_INSTANTIATE_MIXIN";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-mixin-inherit-class")
                    {
                        copy.code = "E_MIXIN_CANNOT_INHERIT_CLASS";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-interface-impl-missing")
                    {
                        copy.code = "E_UNIMPLEMENTED_INTERFACE_METHOD";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-ambiguous-identifier")
                    {
                        copy.code = "E_AMBIGUOUS_IDENTIFIER";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-undefined-namespace")
                    {
                        copy.code = "E_UNDEFINED_NAMESPACE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-import-has-body")
                    {
                        copy.code = "E_IMPORT_CANNOT_HAVE_BODY";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-duplicate-case-value")
                    {
                        copy.code = "E_DUPLICATE_CASE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-case-not-constant")
                    {
                        copy.code = "E_NOT_A_CONSTANT_EXPRESSION";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-break-outside-loop")
                    {
                        copy.code = "E_BREAK_OUTSIDE_LOOP";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-continue-outside-loop")
                    {
                        copy.code = "E_CONTINUE_OUTSIDE_LOOP";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-void-return-value")
                    {
                        copy.code = "E_VOID_CANNOT_RETURN_VALUE";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-return-type-mismatch" || copy.code == "as-err-no-implicit-conversion")
                    {
                        copy.code = "E_RETURN_TYPE_MISMATCH";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    else if (copy.code == "as-err-undefined-identifier")
                    {
                        copy.code = "E_UNDEFINED_IDENTIFIER";
                        linesWithSpecificErrors.insert(copy.range.start.line);
                    }
                    rawErrors.push_back(copy);
                }
            }

            std::vector<analysis::Diagnostic> errors;
            for (const auto &d : rawErrors)
            {
                if (d.code == "as-syntax-error" && linesWithSpecificErrors.contains(d.range.start.line))
                {
                    continue;
                }
                errors.push_back(d);
            }

            std::sort(errors.begin(), errors.end(), [](const analysis::Diagnostic &a, const analysis::Diagnostic &b)
            {
                if (a.range.start.line != b.range.start.line)
                {
                    return a.range.start.line < b.range.start.line;
                }
                return a.range.start.character < b.range.start.character;
            });
            return errors;
        }

        std::string GetSymbolTypeAt(Position pos) const
        {
            std::string result;
            m_symbolTable.ForEachSymbolInFile(m_uri, [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
            {
                if (!result.empty())
                {
                    return;
                }
                for (const auto &sym : symbols)
                {
                    if (pos.line == sym.selectionRange.startLine &&
                        pos.character >= sym.selectionRange.startCharacter &&
                        pos.character <= sym.selectionRange.endCharacter)
                    {
                        if (sym.type == analysis::SymbolType::Typedef)
                        {
                            result = sym.GetTypedef().baseType;
                            return;
                        }
                        if (sym.type == analysis::SymbolType::Variable)
                        {
                            if (sym.GetVariable().typeName == "auto" || sym.GetVariable().typeName == "auto@")
                            {
                                const analysis::Scope *scope = m_scopeRoot ? FindInnermostScope(m_scopeRoot.get(), sym.startLine, sym.startCharacter) : nullptr;
                                const analysis::LocalDefinition *def = scope ? analysis::ResolveInScope(scope, sym.name) : nullptr;
                                if (def && !def->typeName.empty() && def->typeName != "auto" && def->typeName != "auto@")
                                {
                                    result = def->typeName;
                                    return;
                                }
                            }
                            result = sym.GetVariable().typeName;
                            return;
                        }
                        if (sym.type == analysis::SymbolType::Function)
                        {
                            result = sym.GetFunction().returnType;
                            return;
                        }
                        if (sym.type == analysis::SymbolType::Class)
                        {
                            result = sym.name;
                            return;
                        }
                    }
                    if (pos.line == sym.startLine &&
                        pos.character >= sym.startCharacter &&
                        pos.character <= sym.endCharacter)
                    {
                        if (sym.type == analysis::SymbolType::Typedef)
                        {
                            result = sym.GetTypedef().baseType;
                            return;
                        }
                    }
                }
            });

            if (result.empty() && m_scopeRoot)
            {
                const analysis::Scope *scope = FindInnermostScope(m_scopeRoot.get(), pos.line, pos.character);
                while (scope && result.empty())
                {
                    for (const auto &def : scope->definitions)
                    {
                        if (def.startLine == pos.line && pos.character >= def.startCharacter && pos.character <= def.endCharacter)
                        {
                            result = def.typeName;
                            return result;
                        }
                    }
                    scope = scope->parent;
                }
            }

            if (result.empty() && m_tree)
            {
                TSNode root = ts_tree_root_node(m_tree);
                TSPoint pt{ pos.line, pos.character };
                TSNode node = ts_node_descendant_for_point_range(root, pt, pt);
                if (!ts_node_is_null(node))
                {
                    const analysis::Scope *scope = m_scopeRoot ? FindInnermostScope(m_scopeRoot.get(), pos.line, pos.character) : nullptr;
                    std::string nodeText = GetNodeText(node, m_sourceCode);
                    while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.front()))) nodeText.erase(nodeText.begin());
                    while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.back()))) nodeText.pop_back();
                    if (!nodeText.empty() && scope)
                    {
                        const auto *def = analysis::ResolveInScope(scope, nodeText);
                        if (def && !def->typeName.empty())
                        {
                            return def->typeName;
                        }
                    }
                    std::string inferred = analysis::ResolveExpressionType(node, scope, m_symbolTable, m_sourceCode, m_uri);
                    if (!inferred.empty() && inferred != "unknown")
                    {
                        return inferred;
                    }
                }
            }

            return result;
        }

        std::optional<ResolvedCallInfo> GetResolvedCallAt(Position pos) const
        {
            if (!m_tree)
            {
                return std::nullopt;
            }

            TSNode root = ts_tree_root_node(m_tree);
            TSPoint pt{ pos.line, pos.character };
            TSNode node = ts_node_named_descendant_for_point_range(root, pt, pt);
            TSNode callNode = node;
            while (!ts_node_is_null(callNode) && std::string_view(ts_node_type(callNode)) != "call_expression")
            {
                callNode = ts_node_parent(callNode);
            }

            if (ts_node_is_null(callNode))
            {
                TSNode binaryNode = node;
                while (!ts_node_is_null(binaryNode) && std::string_view(ts_node_type(binaryNode)) != "binary_expression")
                {
                    binaryNode = ts_node_parent(binaryNode);
                }

                if (!ts_node_is_null(binaryNode))
                {
                    TSNode left = ts_node_child_by_field_name(binaryNode, "left", 4);
                    TSNode opNode = ts_node_child_by_field_name(binaryNode, "operator", 8);
                    TSNode right = ts_node_child_by_field_name(binaryNode, "right", 5);
                    if (!ts_node_is_null(left) && !ts_node_is_null(right))
                    {
                        std::string op = GetNodeText(opNode, m_sourceCode);
                        auto scopeRoot = const_cast<analysis::LocalScopeCollector&>(m_scopeCollector).CollectScopes(m_sourceCode, const_cast<parser::AngelScriptParser&>(m_parser));
                        const analysis::Scope *scope = FindInnermostScope(scopeRoot.get(), pos.line, pos.character);
                        std::string leftType = analysis::ResolveExpressionType(left, scope, m_symbolTable, m_sourceCode, m_uri);
                        std::string rightType = analysis::ResolveExpressionType(right, scope, m_symbolTable, m_sourceCode, m_uri);
                        std::string cleanLeft = analysis::CleanBaseType(leftType);
                        std::string cleanRight = analysis::CleanBaseType(rightType);

                        std::string opMethod, revOpMethod;
                        if (op == "+") { opMethod = "opAdd"; revOpMethod = "opAdd_r"; }
                        else if (op == "-") { opMethod = "opSub"; revOpMethod = "opSub_r"; }
                        else if (op == "*") { opMethod = "opMul"; revOpMethod = "opMul_r"; }
                        else if (op == "/") { opMethod = "opDiv"; revOpMethod = "opDiv_r"; }

                        // 1. Check left opMethod
                        if (!opMethod.empty() && !cleanLeft.empty())
                        {
                            std::vector<analysis::Symbol> candidates;
                            for (const auto &typeName : analysis::GetInheritedTypeHierarchy(cleanLeft, m_symbolTable))
                            {
                                for (const auto &sym : m_symbolTable.FindSymbols(typeName + "::" + opMethod))
                                {
                                    if (sym.type == analysis::SymbolType::Function) candidates.push_back(sym);
                                }
                            }
                            if (!candidates.empty())
                            {
                                auto match = analysis::ResolveBestOverload(candidates, { rightType }, m_symbolTable);
                                if (match.bestCandidate)
                                {
                                    const auto &fn = match.bestCandidate->GetFunction();
                                    std::string sig = match.bestCandidate->containerName + "::" + match.bestCandidate->name + "(";
                                    for (size_t i = 0; i < fn.parameters.size(); ++i)
                                    {
                                        if (i > 0) sig += ", ";
                                        sig += FormatParamType(fn.parameters[i]);
                                    }
                                    sig += ")";
                                    if (fn.modifiers.isConst) sig += " const";
                                    return ResolvedCallInfo{ sig };
                                }
                            }
                        }

                        // 2. Check right revOpMethod
                        if (!revOpMethod.empty() && !cleanRight.empty())
                        {
                            std::vector<analysis::Symbol> candidates;
                            for (const auto &typeName : analysis::GetInheritedTypeHierarchy(cleanRight, m_symbolTable))
                            {
                                for (const auto &sym : m_symbolTable.FindSymbols(typeName + "::" + revOpMethod))
                                {
                                    if (sym.type == analysis::SymbolType::Function) candidates.push_back(sym);
                                }
                            }
                            if (!candidates.empty())
                            {
                                auto match = analysis::ResolveBestOverload(candidates, { leftType }, m_symbolTable);
                                if (match.bestCandidate)
                                {
                                    const auto &fn = match.bestCandidate->GetFunction();
                                    std::string sig = match.bestCandidate->containerName + "::" + match.bestCandidate->name + "(";
                                    for (size_t i = 0; i < fn.parameters.size(); ++i)
                                    {
                                        if (i > 0) sig += ", ";
                                        sig += FormatParamType(fn.parameters[i]);
                                    }
                                    sig += ")";
                                    if (fn.modifiers.isConst) sig += " const";
                                    return ResolvedCallInfo{ sig };
                                }
                            }
                        }
                    }
                }
                return std::nullopt;
            }

            TSNode funcNode = ts_node_child_by_field_name(callNode, "function", 8);
            if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
            {
                funcNode = ts_node_child(callNode, 0);
            }
            if (ts_node_is_null(funcNode))
            {
                return std::nullopt;
            }

            std::string calleeText = GetNodeText(funcNode, m_sourceCode);
            while (!calleeText.empty() && isspace(static_cast<unsigned char>(calleeText.front()))) calleeText.erase(calleeText.begin());
            while (!calleeText.empty() && isspace(static_cast<unsigned char>(calleeText.back()))) calleeText.pop_back();

            auto scopeRoot = const_cast<analysis::LocalScopeCollector&>(m_scopeCollector).CollectScopes(m_sourceCode, const_cast<parser::AngelScriptParser&>(m_parser));
            const analysis::Scope *scope = FindInnermostScope(scopeRoot.get(), pos.line, pos.character);
            std::string calleeType = analysis::ResolveExpressionType(funcNode, scope, m_symbolTable, m_sourceCode, m_uri);
            std::string clean = analysis::CleanBaseType(calleeType);

            auto found = m_symbolTable.FindSymbols(clean);
            for (const auto &s : found)
            {
                if (s.type == analysis::SymbolType::Funcdef)
                {
                    const auto &fd = s.GetFuncdef();
                    std::string sig = s.name + "::" + s.name + "(";
                    for (size_t i = 0; i < fd.parameters.size(); ++i)
                    {
                        if (i > 0) sig += ", ";
                        sig += fd.parameters[i].typeName;
                    }
                    sig += ")";
                    return ResolvedCallInfo{ sig };
                }
            }

            if (std::string_view(ts_node_type(funcNode)) == "member_expression")
            {
                TSNode objNode = ts_node_child_by_field_name(funcNode, "object", 6);
                TSNode memNode = ts_node_child_by_field_name(funcNode, "member", 6);
                if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
                {
                    std::string objType = analysis::ResolveExpressionType(objNode, scope, m_symbolTable, m_sourceCode, m_uri);
                    std::string cleanObj = analysis::CleanBaseType(objType);
                    std::string memName = GetNodeText(memNode, m_sourceCode);
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.front()))) memName.erase(memName.begin());
                    while (!memName.empty() && isspace(static_cast<unsigned char>(memName.back()))) memName.pop_back();

                    std::vector<std::string> argTypes;
                    TSNode argsNode = ts_node_child_by_field_name(callNode, "arguments", 9);
                    if (!ts_node_is_null(argsNode))
                    {
                        uint32_t count = ts_node_named_child_count(argsNode);
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            TSNode argChild = ts_node_named_child(argsNode, i);
                            argTypes.push_back(analysis::ResolveExpressionType(argChild, scope, m_symbolTable, m_sourceCode, m_uri));
                        }
                    }

                    std::vector<analysis::Symbol> candidates;
                    auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanObj, m_symbolTable);
                    for (const auto &typeName : hierarchy)
                    {
                        auto found = m_symbolTable.FindSymbols(typeName + "::" + memName);
                        for (const auto &sym : found)
                        {
                            if (sym.type == analysis::SymbolType::Function)
                            {
                                candidates.push_back(sym);
                            }
                        }
                        if (!candidates.empty())
                        {
                            break;
                        }
                    }

                    if (!candidates.empty())
                    {
                        auto match = analysis::ResolveBestOverload(candidates, argTypes, m_symbolTable);
                        if (match.bestCandidate)
                        {
                            const auto &fn = match.bestCandidate->GetFunction();
                            std::string sig = match.bestCandidate->containerName + "::" + match.bestCandidate->name + "(";
                            for (size_t i = 0; i < fn.parameters.size(); ++i)
                            {
                                if (i > 0) sig += ", ";
                                sig += FormatParamType(fn.parameters[i]);
                            }
                            sig += ")";
                            if (fn.modifiers.isConst) sig += " const";
                            return ResolvedCallInfo{ sig };
                        }
                    }
                }
            }

            auto funcSymbols = analysis::FindSymbolsInScope(calleeText, funcNode, m_sourceCode, m_symbolTable);
            for (const auto &s : funcSymbols)
            {
                if (s.type == analysis::SymbolType::Function)
                {
                    const auto &fn = s.GetFunction();
                    std::string containerPrefix = s.containerName.empty() ? "" : (s.containerName + "::");
                    std::string sig = containerPrefix + s.name + "(";
                    for (size_t i = 0; i < fn.parameters.size(); ++i)
                    {
                        if (i > 0) sig += ", ";
                        sig += FormatParamType(fn.parameters[i]);
                    }
                    sig += ")";
                    if (fn.modifiers.isConst) sig += " const";
                    return ResolvedCallInfo{ sig };
                }
            }

            return std::nullopt;
        }

        struct ResolvedSymbolDefinition
        {
            std::string targetSymbol;
        };

        std::optional<ResolvedSymbolDefinition> GetSymbolDefinitionAt(Position pos) const
        {
            if (!m_tree) return std::nullopt;
            TSNode root = ts_tree_root_node(m_tree);
            TSPoint pt{ pos.line, pos.character };
            TSNode node = ts_node_descendant_for_point_range(root, pt, pt);
            if (ts_node_is_null(node)) return std::nullopt;

            TSNode p = node;
            while (!ts_node_is_null(p))
            {
                if (std::string_view(ts_node_type(p)) == "scoped_identifier")
                {
                    std::string text = GetNodeText(p, m_sourceCode);
                    while (!text.empty() && isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
                    while (!text.empty() && isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
                    if (!text.empty() && (text.find("::") != std::string::npos || text.rfind("::", 0) == 0))
                    {
                        return ResolvedSymbolDefinition{ text };
                    }
                    break;
                }
                p = ts_node_parent(p);
            }

            std::string nodeText = GetNodeText(node, m_sourceCode);
            while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.front()))) nodeText.erase(nodeText.begin());
            while (!nodeText.empty() && isspace(static_cast<unsigned char>(nodeText.back()))) nodeText.pop_back();
            if (nodeText.empty()) return std::nullopt;

            auto syms = analysis::FindSymbolsInScope(nodeText, node, m_sourceCode, m_symbolTable);
            if (!syms.empty())
            {
                std::string target = syms[0].qualifiedName;
                if (target.empty()) target = "::" + syms[0].name;
                return ResolvedSymbolDefinition{ target };
            }

            return std::nullopt;
        }

        struct HoverResult
        {
            struct MarkupContent
            {
                std::string value;
            } contents;
        };

        std::optional<HoverResult> GetHoverAt(Position pos) const
        {
            if (!m_tree) return std::nullopt;
            TSNode root = ts_tree_root_node(m_tree);
            TSPoint pt{ pos.line, pos.character };
            TSNode node = ts_node_descendant_for_point_range(root, pt, pt);
            if (ts_node_is_null(node)) return std::nullopt;

            TSNode p = node;
            std::string name;
            while (!ts_node_is_null(p))
            {
                std::string_view pType = ts_node_type(p);
                if (pType == "call_expression")
                {
                    TSNode funcNode = ts_node_child_by_field_name(p, "function", 8);
                    if (ts_node_is_null(funcNode) && ts_node_child_count(p) > 0)
                    {
                        funcNode = ts_node_child(p, 0);
                    }
                    if (!ts_node_is_null(funcNode))
                    {
                        name = GetNodeText(funcNode, m_sourceCode);
                        break;
                    }
                }
                p = ts_node_parent(p);
            }

            if (name.empty())
            {
                name = GetNodeText(node, m_sourceCode);
            }

            while (!name.empty() && isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
            while (!name.empty() && isspace(static_cast<unsigned char>(name.back()))) name.pop_back();

            if (name.empty()) return std::nullopt;

            auto syms = analysis::FindSymbolsInScope(name, node, m_sourceCode, m_symbolTable);
            if (!syms.empty())
            {
                std::string text = analysis::FormatFunctionDeclaration(syms[0], false);
                return HoverResult{ { text } };
            }

            return std::nullopt;
        }

        const analysis::SymbolTable &GetSymbolTable() const
        {
            return m_symbolTable;
        }

    private:
        std::string m_uri;
        std::string m_sourceCode;
        parser::AngelScriptParser m_parser;
        analysis::SymbolCollector m_symbolCollector{ nullptr };
        analysis::LocalScopeCollector m_scopeCollector{ nullptr };
        analysis::SymbolTable m_symbolTable;
        std::shared_ptr<const analysis::Scope> m_scopeRoot;
        TSTree *m_tree = nullptr;
        std::vector<analysis::Diagnostic> m_diagnostics;
    };

    inline std::shared_ptr<TestDocument> CreateTestDocument(const std::string &uri, const std::string &sourceCode)
    {
        auto doc = std::make_shared<TestDocument>(uri, sourceCode);
        GetTestWorkspaceRegistry()[uri] = doc;
        return doc;
    }

    inline void PopulateTestSymbolTable(std::shared_ptr<TestDocument> doc, analysis::SymbolTable &table)
    {
        if (doc)
        {
            doc->GetSymbolTable().ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
            {
                for (const auto &s : symbols)
                {
                    table.AddSymbol(s);
                }
            });
        }
    }
}

using angel_lsp::test::CreateTestDocument;
using angel_lsp::test::TestDocument;
using angel_lsp::test::PopulateTestSymbolTable;
