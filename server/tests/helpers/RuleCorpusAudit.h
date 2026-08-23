#pragma once

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace angel_lsp::test
{
    /** @brief One diagnostic a rule produced somewhere in the corpus, with enough context to judge it. */
    struct CorpusHit
    {
        std::string fileName;
        uint32_t line = 0;
        std::string code;
        std::string message;
    };

    /** @brief What a corpus run found, ready to be reported and asserted on. */
    struct CorpusResult
    {
        size_t filesAnalysed = 0;
        double seconds = 0.0;
        std::map<std::string, size_t> countByCode;
        std::vector<CorpusHit> hits;

        size_t Total() const
        {
            size_t total = 0;
            for (const auto &[code, count] : countByCode)
            {
                total += count;
            }
            return total;
        }
    };

    /**
     * @brief Runs the analyzer over the whole angelscript/ corpus and collects matching diagnostics.
     *
     * Grouped by project prefix with a shared SymbolTable, which is the configuration a rule can
     * actually be judged in: a class declared in a sibling file is only visible that way, and
     * visibility is what decides whether a rule says anything at all. Per-file runs would audit
     * the silent path and prove nothing.
     *
     * Every corpus file is working AngelScript, so a hit is a false positive until the specific
     * case is read and shown to be a genuine error.
     *
     * @param interesting Predicate selecting the diagnostic codes this audit is about.
     * @param maxHits Cap on retained examples, so a flooding rule does not exhaust memory.
     */
    inline CorpusResult RunCorpusAudit(const std::function<bool(const std::string &)> &interesting,
                                       size_t maxHits = 80)
    {
        namespace fs = std::filesystem;
        using namespace angel_lsp::analysis;
        using namespace angel_lsp::parser;

        CorpusResult result;

        std::vector<fs::path> files;
        for (const auto &entry : fs::directory_iterator(ANGELSCRIPT_CORPUS_DIR))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".as")
            {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        std::unordered_map<std::string, std::vector<fs::path>> groups;
        for (const auto &path : files)
        {
            const std::string name = path.filename().string();
            const size_t underscore = name.find('_');
            groups[underscore == std::string::npos ? name : name.substr(0, underscore)].push_back(path);
        }

        angel_lsp::i18n::I18n i18n;

        for (auto &[groupName, groupFiles] : groups)
        {
            SymbolTable sharedTable;
            std::unordered_map<std::string, std::string> sources;

            for (const auto &path : groupFiles)
            {
                std::ifstream file(path, std::ios::binary);
                if (!file)
                {
                    continue;
                }
                std::ostringstream buffer;
                buffer << file.rdbuf();
                std::string sourceCode = buffer.str();
                if (sourceCode.empty())
                {
                    continue;
                }

                const std::string fileUri = "file:///" + path.filename().string();
                sources[fileUri] = sourceCode;

                AngelScriptParser parser;
                SymbolCollector collector(nullptr);
                collector.CollectSymbols(fileUri, sourceCode, parser, sharedTable);
            }

            for (const auto &[fileUri, sourceCode] : sources)
            {
                ++result.filesAnalysed;

                AngelScriptParser parser;
                LocalScopeCollector scopeCollector(nullptr);

                const auto start = std::chrono::steady_clock::now();
                TSTree *tree = parser.Parse(sourceCode);

                SemanticAnalysisRequest request{ sharedTable, fileUri, "", &i18n };
                request.scopeRoot = scopeCollector.CollectScopes(sourceCode, parser);
                request.sourceCode = sourceCode;
                request.tree = tree;

                SemanticAnalyzer analyzer(nullptr);
                const auto diagnostics = analyzer.Analyze(request);
                result.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

                for (const auto &diag : diagnostics)
                {
                    if (!interesting(diag.code))
                    {
                        continue;
                    }
                    ++result.countByCode[diag.code];
                    if (result.hits.size() < maxHits)
                    {
                        result.hits.push_back({ fileUri.substr(8), diag.range.start.line + 1, diag.code, diag.message });
                    }
                }

                if (tree)
                {
                    ts_tree_delete(tree);
                }
            }
        }

        return result;
    }
}
