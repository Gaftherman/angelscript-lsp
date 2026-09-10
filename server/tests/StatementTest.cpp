#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "config/ServerConfig.h"

#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    /**
     * @brief Analyzes source code and returns all compiler/analyzer diagnostics.
     * @param sourceCode AngelScript script code.
     * @return Collection of diagnostics emitted by the pipeline.
     */
    std::vector<Diagnostic> AnalyzeStatementSnippet(const std::string &sourceCode)
    {
        SymbolTable table;
        angel_lsp::i18n::I18n i18n;
        const std::string fileUri = "file:///test_statement.as";

        AngelScriptParser symbolParser;
        SymbolCollector symbolCollector(nullptr);
        auto collectorDiagnostics = symbolCollector.CollectSymbols(fileUri, sourceCode, symbolParser, table);

        AngelScriptParser scopeParser;
        LocalScopeCollector scopeCollector(nullptr);

        SemanticAnalysisRequest req{ table, fileUri, "", &i18n };
        angel_lsp::config::DiagnosticsConfig diagConfig;
        req.diagnostics = &diagConfig;
        req.scopeRoot = scopeCollector.CollectScopes(sourceCode, scopeParser);

        AngelScriptParser treeParser;
        req.sourceCode = sourceCode;
        req.tree = treeParser.Parse(sourceCode);

        SemanticAnalyzer analyzer(nullptr);
        auto diagnostics = analyzer.Analyze(req);

        diagnostics.insert(diagnostics.end(), collectorDiagnostics.begin(), collectorDiagnostics.end());

        if (req.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(req.tree));
        }

        return diagnostics;
    }
}

TEST_CASE("Statement - Standalone null statement is accepted with 0 errors (asharness parity)")
{
    // Ground truth: asharness.exe accepts `void main() { null; }` with status ACEPTADO and 0 messages.
    const std::string code =
        "void main() {\n"
        "    null;\n"
        "}\n";

    auto diagnostics = AnalyzeStatementSnippet(code);
    size_t errorCount = 0;
    for (const auto &d : diagnostics)
    {
        if (d.severity == DiagnosticSeverity::Error)
        {
            ++errorCount;
        }
    }

    CHECK(errorCount == 0);
    CHECK(diagnostics.empty());
}

TEST_CASE("Statement - Standalone literal statements are accepted with 0 errors (asharness parity)")
{
    // Ground truth: asharness.exe accepts integer, string, boolean literals as statements.
    const std::string code =
        "void main() {\n"
        "    123;\n"
        "    \"string\";\n"
        "    true;\n"
        "    3.14f;\n"
        "}\n";

    auto diagnostics = AnalyzeStatementSnippet(code);
    size_t errorCount = 0;
    for (const auto &d : diagnostics)
    {
        if (d.severity == DiagnosticSeverity::Error)
        {
            ++errorCount;
        }
    }

    CHECK(errorCount == 0);
    CHECK(diagnostics.empty());
}
