#pragma once

#include "analysis/Diagnostics.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
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
    class TestDocument
    {
    public:
        TestDocument(const std::string &uri, const std::string &sourceCode)
            : m_uri(uri), m_sourceCode(sourceCode)
        {
            m_tree = m_parser.Parse(m_sourceCode);
            static angel_lsp::i18n::I18n i18n;

            m_diagnostics = m_symbolCollector.CollectSymbolsWithTree(m_uri, m_sourceCode, m_tree, m_symbolTable, &i18n);

            analysis::SemanticAnalysisRequest request{ m_symbolTable, m_uri, ".as.predefined", &i18n };
            request.scopeRoot = m_scopeCollector.CollectScopes(m_sourceCode, m_parser);
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
            std::vector<analysis::Diagnostic> errors;
            for (const auto &d : m_diagnostics)
            {
                if (d.severity == analysis::DiagnosticSeverity::Error)
                {
                    analysis::Diagnostic copy = d;
                    if (copy.code == "as-err-duplicate-symbol")
                    {
                        copy.code = "E_DUPLICATE_DECLARATION";
                    }
                    errors.push_back(copy);
                }
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
        TSTree *m_tree = nullptr;
        std::vector<analysis::Diagnostic> m_diagnostics;
    };

    inline std::shared_ptr<TestDocument> CreateTestDocument(const std::string &uri, const std::string &sourceCode)
    {
        return std::make_shared<TestDocument>(uri, sourceCode);
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
