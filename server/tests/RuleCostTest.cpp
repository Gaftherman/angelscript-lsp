#include <doctest/doctest.h>

#include "analysis/AccessChecker.h"
#include "analysis/ConstChecker.h"
#include "analysis/CallGraph.h"
#include "analysis/ControlFlowChecker.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/rules/TypeRules.h"
#include "analysis/rules/VariableRules.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    constexpr size_t k_fileCount = 300;

    struct Sample
    {
        std::string fileUri;
        std::string sourceCode;
    };

    std::vector<Sample> LoadSamples()
    {
        namespace fs = std::filesystem;

        std::vector<fs::path> paths;
        for (const auto &entry : fs::directory_iterator(ANGELSCRIPT_CORPUS_DIR))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".as")
            {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        if (paths.size() > k_fileCount)
        {
            paths.resize(k_fileCount);
        }

        std::vector<Sample> samples;
        for (const auto &path : paths)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                continue;
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            if (buffer.str().empty())
            {
                continue;
            }
            samples.push_back({ "file:///" + path.filename().string(), buffer.str() });
        }
        return samples;
    }

    double Milliseconds(const std::chrono::steady_clock::time_point &start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
}

// =====================================================================================
// Rule cost measurement (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Rule Cost*"`)
//
// Answers the one question a new rule set has to answer before it ships: analysis runs debounced
// on a background thread, but 300 files' worth of it still has to be affordable. Each pass is timed
// on its own, over the same already-parsed trees and symbol tables, so the numbers say what the
// rules cost rather than what parsing costs.
// =====================================================================================

TEST_CASE("Analysis - Rule Cost Over 300 Corpus Files" * doctest::skip(true))
{
    const auto samples = LoadSamples();
    REQUIRE(!samples.empty());

    angel_lsp::i18n::I18n i18n;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopeCollector(nullptr);

    SymbolTable table;
    std::vector<TSTree *> trees;
    std::vector<std::shared_ptr<const Scope>> scopes;

    // Front-end cost, measured but not attributed to any rule: this has to happen whether or not a
    // single rule exists.
    const auto frontEndStart = std::chrono::steady_clock::now();
    for (const auto &sample : samples)
    {
        collector.CollectSymbols(sample.fileUri, sample.sourceCode, parser, table);
        trees.push_back(parser.Parse(sample.sourceCode));
        scopes.push_back(std::shared_ptr<const Scope>(scopeCollector.CollectScopes(sample.sourceCode, parser)));
    }
    const double frontEndMs = Milliseconds(frontEndStart);

    const auto makeRequest = [&](size_t index)
    {
        SemanticAnalysisRequest request{ table, samples[index].fileUri, "", &i18n };
        request.scopeRoot = scopes[index];
        request.sourceCode = samples[index].sourceCode;
        request.tree = trees[index];
        return request;
    };

    // One timed pass per module. Timing each rule call individually was tried first and measured
    // the clock rather than the rules: at fifteen million calls, two steady_clock reads apiece cost
    // more than the work between them.
    const auto timeModule = [&](auto &&validate)
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            std::vector<Diagnostic> diagnostics;
            SemanticAnalysisRequest request = makeRequest(i);
            DiagnosticContext ctx{ request, diagnostics, nullptr };

            table.ForEachSymbolInFile(
                request.fileUri,
                [&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    validate(symbols, request, ctx);
                });
        }
        return Milliseconds(start);
    };

    const auto perSymbol = [](auto &&rule)
    {
        return [rule](const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &request,
                      const DiagnosticContext &ctx)
        {
            for (const auto &sym : symbols)
            {
                if (sym.fileUri == request.fileUri)
                {
                    rule(sym, ctx);
                }
            }
        };
    };

    const double duplicateMs = timeModule(
        [](const std::vector<Symbol> &symbols, const SemanticAnalysisRequest &, const DiagnosticContext &ctx)
        { rules::ValidateDuplicates(symbols, ctx); });
    const double classMs = timeModule(perSymbol([](const Symbol &sym, const DiagnosticContext &ctx)
        { if (sym.type == SymbolType::Class) rules::ValidateClass(sym, ctx); }));
    const double variableMs = timeModule(perSymbol([](const Symbol &sym, const DiagnosticContext &ctx)
        { if (sym.type == SymbolType::Variable || sym.type == SymbolType::Property) rules::ValidateVariable(sym, ctx); }));
    const double functionMs = timeModule(perSymbol([](const Symbol &sym, const DiagnosticContext &ctx)
        { if (sym.type == SymbolType::Function) rules::ValidateFunction(sym, ctx); }));
    const double operatorMs = timeModule(perSymbol([](const Symbol &sym, const DiagnosticContext &ctx)
        { if (sym.type == SymbolType::Function) rules::ValidateOperator(sym, ctx); }));

    // Declaration rules, driven directly rather than through Analyze so the symbol-table walk is
    // the only thing shared with the other passes.
    double declarationMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            std::vector<Diagnostic> diagnostics;
            SemanticAnalysisRequest request = makeRequest(i);
            DiagnosticContext ctx{ request, diagnostics, nullptr };

            table.ForEachSymbolInFile(
                request.fileUri,
                [&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    rules::ValidateDuplicates(symbols, ctx);
                    for (const auto &sym : symbols)
                    {
                        if (sym.fileUri != request.fileUri)
                        {
                            continue;
                        }
                        switch (sym.type)
                        {
                        case SymbolType::Class:     rules::ValidateClass(sym, ctx); break;
                        case SymbolType::Interface: rules::ValidateInterfaceMembers(sym, ctx); break;
                        case SymbolType::Typedef:   rules::ValidateTypedef(sym, ctx); break;
                        case SymbolType::Funcdef:   rules::ValidateFuncdef(sym, ctx); break;
                        case SymbolType::Enum:      rules::ValidateEnum(sym, ctx); break;
                        case SymbolType::Variable:
                        case SymbolType::Property:  rules::ValidateVariable(sym, ctx); break;
                        case SymbolType::Function:
                            rules::ValidateFunction(sym, ctx);
                            rules::ValidateOperator(sym, ctx);
                            break;
                        default: break;
                        }
                    }
                });
        }
        declarationMs = Milliseconds(start);
    }

    double controlFlowMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            std::vector<Diagnostic> diagnostics;
            SemanticAnalysisRequest request = makeRequest(i);
            DiagnosticContext ctx{ request, diagnostics, nullptr };
            CheckControlFlow({ ts_tree_root_node(trees[i]), samples[i].sourceCode }, ctx);
        }
        controlFlowMs = Milliseconds(start);
    }

    double conversionMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            std::vector<Diagnostic> diagnostics;
            SemanticAnalysisRequest request = makeRequest(i);
            DiagnosticContext ctx{ request, diagnostics, nullptr };
            CheckTypeConversions({ ts_tree_root_node(trees[i]), samples[i].sourceCode, scopes[i].get() }, ctx);
        }
        conversionMs = Milliseconds(start);
    }

    double accessMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            std::vector<Diagnostic> diagnostics;
            SemanticAnalysisRequest request = makeRequest(i);
            DiagnosticContext ctx{ request, diagnostics, nullptr };
            CheckMemberAccess({ ts_tree_root_node(trees[i]), samples[i].sourceCode, scopes[i].get() }, ctx);
        }
        accessMs = Milliseconds(start);
    }

    double constMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            std::vector<Diagnostic> diagnostics;
            SemanticAnalysisRequest request = makeRequest(i);
            DiagnosticContext ctx{ request, diagnostics, nullptr };
            CheckConstCorrectness({ ts_tree_root_node(trees[i]), samples[i].sourceCode, scopes[i].get() }, ctx);
        }
        constMs = Milliseconds(start);
    }

    // Not part of Analyze(): the call index is built beside the scope tree on the same edit, and
    // it is the one structure a hierarchy feature added to the per-keystroke path - so what it
    // costs belongs on this list whether or not the analyzer runs it.
    double callGraphMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            volatile size_t sink = CollectCalls(ts_tree_root_node(trees[i]), samples[i].sourceCode).size();
            (void)sink;
        }
        callGraphMs = Milliseconds(start);
    }

    double wholeAnalysisMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < samples.size(); ++i)
        {
            SemanticAnalyzer analyzer(nullptr);
            analyzer.Analyze(makeRequest(i));
        }
        wholeAnalysisMs = Milliseconds(start);
    }

    const double files = static_cast<double>(samples.size());
    MESSAGE("Rule cost over " << samples.size() << " files (totals, then per file):");
    MESSAGE("  parse + collect + scopes : " << frontEndMs << " ms  (" << frontEndMs / files << " ms/file)");
    MESSAGE("  declaration rules        : " << declarationMs << " ms  (" << declarationMs / files << " ms/file)");
    MESSAGE("  control flow             : " << controlFlowMs << " ms  (" << controlFlowMs / files << " ms/file)");
    MESSAGE("  type conversions         : " << conversionMs << " ms  (" << conversionMs / files << " ms/file)");
    MESSAGE("  member access            : " << accessMs << " ms  (" << accessMs / files << " ms/file)");
    MESSAGE("  const correctness        : " << constMs << " ms  (" << constMs / files << " ms/file)");
    MESSAGE("  call index (not in Analyze): " << callGraphMs << " ms  (" << callGraphMs / files << " ms/file)");
    MESSAGE("  Analyze() as a whole     : " << wholeAnalysisMs << " ms  (" << wholeAnalysisMs / files << " ms/file)");
    MESSAGE("  by declaration module (each includes the per-file bucket walk it shares):");
    MESSAGE("    duplicates : " << duplicateMs << " ms");
    MESSAGE("    class      : " << classMs << " ms");
    MESSAGE("    variable   : " << variableMs << " ms");
    MESSAGE("    function   : " << functionMs << " ms");
    MESSAGE("    operator   : " << operatorMs << " ms");

    for (TSTree *tree : trees)
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }

    CHECK(samples.size() > 0);
}
