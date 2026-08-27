#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticHelpers.h"
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
            while (!ts_node_is_null(node) && std::string_view(ts_node_type(node)) != "call_expression")
            {
                node = ts_node_parent(node);
            }

            if (ts_node_is_null(node))
            {
                return std::nullopt;
            }

            TSNode funcNode = ts_node_child_by_field_name(node, "function", 8);
            if (ts_node_is_null(funcNode) && ts_node_child_count(node) > 0)
            {
                funcNode = ts_node_child(node, 0);
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

            auto funcSymbols = m_symbolTable.FindSymbols(calleeText);
            for (const auto &s : funcSymbols)
            {
                if (s.type == analysis::SymbolType::Function)
                {
                    const auto &fn = s.GetFunction();
                    std::string sig = s.name + "(";
                    for (size_t i = 0; i < fn.parameters.size(); ++i)
                    {
                        if (i > 0) sig += ", ";
                        sig += fn.parameters[i].typeName;
                    }
                    sig += ")";
                    return ResolvedCallInfo{ sig };
                }
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
