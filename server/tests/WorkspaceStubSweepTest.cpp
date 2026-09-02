#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Every predefined stub in this checkout has to parse clean.
//
// A stub is not an ordinary file. It is the only place the host's API is written down, everything
// the analyzer knows about host types comes from one, and the server loads every stub it finds the
// moment a workspace is opened - `as.predefined` is matched by name, not only by suffix, precisely
// so a stub dropped at a project root is picked up.
//
// Which is the gap this closes. PredefinedFixtureTest covers the two stubs committed under
// tests/fixtures, and PredefinedStubAuditTest reports on one named by ANGELSCRIPT_STUB_PATH and
// asserts nothing. A stub sitting in a developer's checkout - the exact thing the name rule exists
// to pick up - was read by the server and validated by nothing. If it fails to parse, every
// declaration in it is invisible and every host type silently stops resolving, with no diagnostic
// anywhere to say why.
//
// So this sweeps the whole repository rather than a fixed list, and requires each stub it finds to
// parse and analyse without errors. In CI that is the committed fixtures; in a working checkout it
// is those plus whatever the developer put there.
// =====================================================================================

namespace
{
    std::string ReadWholeFile(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    /** @brief Every predefined stub under the repository root, by the server's own name rule. */
    std::vector<std::filesystem::path> FindStubs()
    {
        std::vector<std::filesystem::path> stubs;

        // The same walk and the same exclusions the server uses, so this cannot disagree with it
        // about which files are stubs or which trees are skipped.
        angel_lsp::utils::ForEachWorkspaceFile(
            { std::string(ANGELSCRIPT_REPO_ROOT) },
            { "**/.git/**", "**/build*/**", "**/node_modules/**", "**/out/**" },
            {},
            [&stubs](const std::filesystem::directory_entry &entry) {
                if (angel_lsp::utils::IsPredefinedFile(entry.path().generic_string(), ".as.predefined"))
                    stubs.push_back(entry.path());
            });

        std::sort(stubs.begin(), stubs.end());
        return stubs;
    }
}

TEST_CASE("Every predefined stub in this checkout parses and analyses clean")
{
    const auto stubs = FindStubs();

    // Not a vacuous pass. The two fixtures are committed, so finding none means the sweep itself
    // is broken - a wrong root, an exclusion that swallowed tests/fixtures - rather than that the
    // repository is clean.
    REQUIRE_MESSAGE(!stubs.empty(),
                    "No predefined stub found under " << ANGELSCRIPT_REPO_ROOT
                        << ". Two are committed under server/tests/fixtures, so this means the "
                           "sweep is looking in the wrong place.");

    static angel_lsp::i18n::I18n i18n;

    for (const auto &stub : stubs)
    {
        const std::string source = ReadWholeFile(stub);
        const std::string uri = "file:///" + stub.generic_string();

        INFO("stub: " << stub.generic_string() << " (" << source.size() << " bytes)");
        REQUIRE_FALSE(source.empty());

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        auto diagnostics = collector.CollectSymbols(uri, source, parser, table, &i18n);

        SemanticAnalysisRequest request{ table, uri, ".as.predefined", &i18n };
        request.sourceCode = source;
        request.scopeRoot = scopes.CollectScopes(source, parser);

        TSTree *tree = parser.Parse(source);
        request.tree = tree;

        SemanticAnalyzer analyzer(nullptr);
        for (auto &diagnostic : analyzer.Analyze(request))
            diagnostics.push_back(std::move(diagnostic));

        if (tree)
            ts_tree_delete(tree);

        std::vector<std::string> errors;
        for (const auto &diagnostic : diagnostics)
        {
            if (diagnostic.severity != DiagnosticSeverity::Error)
                continue;

            errors.push_back("line " + std::to_string(diagnostic.range.start.line + 1) + "  " +
                             diagnostic.code + "  " + diagnostic.message);
        }

        // Errors only. A stub legitimately draws warnings and hints - unused declarations are the
        // whole point of one - and failing on those would make every stub in the world unusable.
        std::string report;
        for (const auto &error : errors)
            report += "\n    " + error;

        INFO(report);
        CHECK_MESSAGE(errors.empty(),
                      "This stub does not analyse clean, so every type it declares is invisible to "
                      "the server that reads it: " << stub.filename().generic_string());

        // A stub that produced no symbols parsed as something, but not as declarations - the shape
        // an unrecognised stub takes, and the one no error would report.
        size_t declared = 0;
        table.ForEachSymbol([&declared](const std::string &, const std::vector<Symbol> &symbols) {
            declared += symbols.size();
        });

        CHECK_MESSAGE(declared > 0,
                      "This stub declares nothing the analyzer could see: "
                          << stub.filename().generic_string());
    }

    MESSAGE("Stubs swept: " << stubs.size());
}
