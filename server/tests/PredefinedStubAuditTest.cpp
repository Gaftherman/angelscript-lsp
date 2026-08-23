#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Audit of a real predefined stub (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Predefined Stub Audit*"`).
//
// The corpus audits cover scripts. A stub is the other half of the picture and behaves nothing like
// one: its functions have no bodies by design, it declares template classes, and it is the only
// place the engine's own API is written down. Everything the analyzer knows about host types comes
// from a file shaped like this, so how it reads one decides whether every visibility guard in the
// rule set has anything to work with.
//
// Point ANGELSCRIPT_STUB_PATH at a stub to run it; without one the audit reports and passes.
// =====================================================================================

namespace
{
    /** @brief Path of a stub to audit, or empty when none is available on this machine. */
    std::string FindStub()
    {
        if (const char *fromEnv = std::getenv("ANGELSCRIPT_STUB_PATH"))
        {
            if (std::filesystem::exists(fromEnv))
            {
                return fromEnv;
            }
        }
        return "";
    }

    std::string ReadFile(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}

TEST_CASE("Predefined Stub Audit" * doctest::skip(true))
{
    const std::string path = FindStub();
    if (path.empty())
    {
        MESSAGE("No stub available. Set ANGELSCRIPT_STUB_PATH to audit one.");
        return;
    }

    const std::string source = ReadFile(path);
    REQUIRE(!source.empty());

    const std::string fileName = std::filesystem::path(path).filename().string();
    const std::string fileUri = "file:///" + fileName;

    MESSAGE("Stub: " << fileName << "  bytes=" << source.size());

    // ---------------------------------------------------------------------------------
    // 1. Is it even recognised as a stub?
    //
    // Everything downstream depends on this. A stub that is not recognised is analysed as ordinary
    // script, and every body-less declaration in it becomes an error.
    // ---------------------------------------------------------------------------------
    for (const std::string extension : { ".as.predefined", "as.predefined", ".predefined" })
    {
        MESSAGE("  extension \"" << extension << "\" matches this stub: "
                << (angel_lsp::utils::IsPredefinedFile(fileUri, extension) ? std::string("YES") : std::string("no")));
    }

    // ---------------------------------------------------------------------------------
    // 2. What does the parser make of it?
    // ---------------------------------------------------------------------------------
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    angel_lsp::i18n::I18n i18n;

    const auto parseDiagnostics = collector.CollectSymbols(fileUri, source, parser, table, &i18n);

    size_t symbolCount = 0;
    std::map<std::string, size_t> byKind;
    table.ForEachSymbolInFile(fileUri,
        [&](const std::string &, const std::vector<Symbol> &symbols)
        {
            for (const auto &sym : symbols)
            {
                if (sym.fileUri != fileUri)
                {
                    continue;
                }
                ++symbolCount;
                ++byKind[SymbolTypeToString(sym.type)];
            }
        });

    MESSAGE("  parse diagnostics: " << parseDiagnostics.size());
    MESSAGE("  symbols collected: " << symbolCount);
    for (const auto &[kind, count] : byKind)
    {
        MESSAGE("    " << kind << ": " << count);
    }

    for (size_t i = 0; i < parseDiagnostics.size() && i < 25; ++i)
    {
        MESSAGE("    parse error L" << parseDiagnostics[i].range.start.line + 1
                << ": " << parseDiagnostics[i].message);
    }

    // ---------------------------------------------------------------------------------
    // 3. What do the rules say about it, treated as a stub and treated as script?
    //
    // The difference between the two columns is exactly what the stub exemption is worth.
    // ---------------------------------------------------------------------------------
    const auto analyse = [&](const std::string &asFileUri, const std::string &extension)
    {
        SemanticAnalysisRequest request{ table, asFileUri, extension, &i18n };
        request.scopeRoot = scopes.CollectScopes(source, parser);
        request.sourceCode = source;
        request.tree = parser.Parse(source);

        SemanticAnalyzer analyzer(nullptr);
        const auto diagnostics = analyzer.Analyze(request);

        std::map<std::string, size_t> counts;
        for (const auto &diag : diagnostics)
        {
            ++counts[diag.code];
        }

        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return counts;
    };

    // Did the one parse error cost us the class it sits on?
    for (const std::string typeName : { "array", "dictionary", "string", "any", "CBasePlayer" })
    {
        const auto found = table.FindSymbolsPtr(typeName);
        size_t members = 0;
        table.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &symbols)
        {
            for (const auto &sym : symbols)
            {
                if (sym.containerName == typeName) { ++members; }
            }
        });
        MESSAGE("  type \"" << typeName << "\": declared=" << (found ? "yes" : "NO")
                << " members=" << members);
    }

    const auto asStub = analyse(fileUri, ".as.predefined");

    // The same declarations under a script's name, collected into their own table: the rules only
    // report on symbols belonging to the document being analysed, so pointing the request at
    // another URI over the stub's table would report nothing for the wrong reason. Not the same
    // file with the suffix disabled either - there is no such configuration any more, since
    // `as.predefined` is recognised by name whatever the suffix says.
    SymbolTable scriptTable;
    SymbolCollector scriptCollector(nullptr);
    scriptCollector.CollectSymbols("file:///main.as", source, parser, scriptTable, &i18n);

    std::map<std::string, size_t> asScript;
    {
        SemanticAnalysisRequest request{ scriptTable, "file:///main.as", ".as.predefined", &i18n };
        request.scopeRoot = scopes.CollectScopes(source, parser);
        request.sourceCode = source;
        request.tree = parser.Parse(source);

        SemanticAnalyzer analyzer(nullptr);
        for (const auto &diag : analyzer.Analyze(request))
        {
            ++asScript[diag.code];
        }
        if (request.tree) { ts_tree_delete(const_cast<TSTree *>(request.tree)); }
    }

    {
        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.scopeRoot = scopes.CollectScopes(source, parser);
        request.sourceCode = source;
        request.tree = parser.Parse(source);

        SemanticAnalyzer analyzer(nullptr);
        for (const auto &diag : analyzer.Analyze(request))
        {
            MESSAGE("    surviving: L" << diag.range.start.line + 1 << " [" << diag.code << "] "
                    << diag.message);
        }
        if (request.tree) { ts_tree_delete(const_cast<TSTree *>(request.tree)); }
    }

    MESSAGE("  diagnostics when treated as a stub: "
            << std::accumulate(asStub.begin(), asStub.end(), size_t{0},
                               [](size_t total, const auto &entry) { return total + entry.second; }));
    for (const auto &[code, count] : asStub)
    {
        MESSAGE("    " << code << ": " << count);
    }

    MESSAGE("  diagnostics when NOT recognised as a stub: "
            << std::accumulate(asScript.begin(), asScript.end(), size_t{0},
                               [](size_t total, const auto &entry) { return total + entry.second; }));
    for (const auto &[code, count] : asScript)
    {
        MESSAGE("    " << code << ": " << count);
    }

    // A stub the analyzer reads cleanly is the whole point, so these are asserted rather than only
    // reported. Against the Sven Coop stub the numbers were 1 parse error and 8 diagnostics before
    // the grammar gained the template class production - all eight being `Undeclared identifier 'T'`
    // from the `<T>` of `class array<T>` landing in an ERROR node.
    CHECK(parseDiagnostics.empty());
    CHECK(asStub.empty());

    // The engine API is what the visibility guards in every rule are judging against, so losing
    // symbols silently would disarm the whole rule set. 9079 is what this stub yields.
    CHECK(symbolCount > 9000);
    CHECK(byKind["class"] > 100);
    CHECK(byKind["function"] > 4000);

    // The same declarations in a script must still be loud - that is the cost of failing to
    // recognise a stub, and the reason the recognition has its own tests. This stub yields 3144
    // missing-body findings that way, one per body-less declaration.
    CHECK(asScript["as-err-missing-body"] > 3000);
}
