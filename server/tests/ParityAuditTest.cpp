#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/EngineProfiles.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "utils/PreprocessorRegions.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// =====================================================================================
// Parity against the real AngelScript compiler.
//
// Every other test in this suite asserts what this analyzer *does*. None of them can say whether
// what it does matches the language, because nothing here compiles AngelScript - the SDK is
// deliberately not linked (see server/CMakeLists.txt). AS-Harness does: it is a real
// asIScriptEngine with the standard add-ons registered, and it exits 0 when a script compiles and
// 1 when it does not. That makes it usable as an oracle.
//
// Two things make the comparison honest, and both were learned by trying the naive version first:
//
//  1. It cannot be run over the angelscript/ corpus. Those are Sven Co-op scripts and every one of
//     them names host types - edict_t, entvars_t, CBaseEntity - that live in the game engine, not
//     in the script. asharness rejects 100% of them with "Identifier 'edict_t' is not a data type",
//     which says nothing about either implementation. That result is itself the evidence for this
//     analyzer's central design rule: an unresolved type is assumed engine-registered and is not
//     reported, because in real-world code it nearly always is.
//
//  2. Both sides have to see the same API surface or the comparison is noise. AS-Harness ships
//     as.predefined describing exactly the add-ons it registers; loading that into the symbol table
//     is what puts the two on equal footing.
//
// What is asserted: on scripts the real compiler ACCEPTS, this analyzer must not report an error.
// That direction is the one that matters - the codebase's own stated policy is that a missed error
// costs nothing and a false one costs the user's trust in every other diagnostic on screen. The
// reverse direction (compiler rejects, we stay silent) is counted and printed but not failed on:
// this is not a compiler and does not claim to be one.
//
// Opt-in, because it shells out to a binary built from a different repository:
//
//   set ASHARNESS_EXE=E:\Github\src\AS-Harness\build_release\Release\asharness.exe
//   Debug\angel_lsp_tests.exe --no-skip --test-case="*Parity*"
// =====================================================================================

namespace
{
    namespace fs = std::filesystem;

    std::string EnvVar(const char *name)
    {
#if defined(_WIN32)
        char *value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
            return {};
        std::string result(value);
        free(value);
        return result;
#else
        const char *value = std::getenv(name);
        return value ? std::string(value) : std::string();
#endif
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /** @brief True when the real AngelScript compiler accepts the script (asharness exits 0). */
    bool RealCompilerAccepts(const std::string &harnessExe, const fs::path &script)
    {
        // Wrapped in an extra pair of quotes: cmd.exe strips the outermost pair, and both the
        // harness path and the script path can contain spaces.
        std::string command = "\"\"" + harnessExe + "\" \"" + script.string() + "\" --json --no-pause\"";
#if defined(_WIN32)
        command += " > NUL 2>&1";
#else
        command += " > /dev/null 2>&1";
#endif
        return std::system(command.c_str()) == 0;
    }

    struct ParityCase
    {
        fs::path path;
        bool realAccepts = false;
        std::vector<angel_lsp::analysis::Diagnostic> ourErrors;
    };

    /**
     * @brief A file this analyzer is known to disagree with the compiler on, and why.
     *
     * Every entry was traced to a specific cause before being added. The test checks this list in
     * both directions: an unlisted file producing false positives is a regression, and a listed
     * file that stops producing them means the gap is closed and the entry must be removed.
     * Without that second direction the list would quietly become a place to hide failures.
     */
    struct KnownGap
    {
        std::string fileName;
        std::string reason;
    };

    const std::vector<KnownGap> &KnownGaps()
    {
        static const std::vector<KnownGap> gaps = {
            { "json.as",
              "AS-Harness registers JSON's constructors in C++ but as.predefined declares none, so "
              "JSON(1) genuinely has no visible constructor for any analyzer reading that stub to "
              "find. Stub gap, not an analyzer defect. "
              "This entry used to carry a second, real cause: lines 37-50 sit inside `#if FALSE`, "
              "which CScriptBuilder strips before the compiler sees them, and this analyzer had no "
              "conditional-compilation handling and analysed the dead block. That is fixed - see "
              "utils/PreprocessorRegions.h - and the two error_handler diagnostics it produced are "
              "gone, which is why the count here dropped from 12 to 10." },

            { "optional.as",
              "as.predefined declares optional<T>::opAssign but no constructor, so the declaration "
              "behind optional<string> o(...) is not visible to any analyzer reading that stub. "
              "The engine registers one in C++. Stub gap, not an analyzer defect." },

            // The two below are not stub gaps. They are grammar gaps, and both are already fixed -
            // on tree-sitter-angelscript's feat/metadata-and-list-holes branch, which
            // cmake/TreeSitter.cmake cannot pin until that branch is pushed. Building with
            // -DANGELLSP_TREE_SITTER_ANGELSCRIPT_SOURCE=<checkout> closes them today; the pinned
            // fetch still carries the old grammar, so the entries stay until the pin moves.
            //
            // They are inert once the pin catches up: FindKnownGap is only consulted for a file
            // that actually came back a false positive, so an entry whose gap is closed costs
            // nothing. Delete them with the pin bump anyway - a list of gaps that are not gaps is
            // how the next reader learns to distrust the list.
            { "doc_g02_metadata.as",
              "PARSER-09. CScriptBuilder strips a [Property, Category=\"x\"] block before the "
              "compiler sees the declaration, so metadata compiles. The grammar has no rule for it "
              "and the block turns its whole declaration into an ERROR node, which costs the symbol "
              "rather than just the annotation." },

            { "doc_g03_omitted_initlist_element.as",
              "PARSER-05. `{ 0, 1, , 4, 5 }` gives the third element the type's default. The "
              "grammar's initializer_list uses commaSep, which has no empty alternative, so the "
              "hole makes the declaration an ERROR node." },
        };
        return gaps;
    }

    const KnownGap *FindKnownGap(const fs::path &path)
    {
        for (const auto &gap : KnownGaps())
        {
            if (gap.fileName == path.filename().string())
                return &gap;
        }
        return nullptr;
    }
}

TEST_CASE("Parity - No errors on scripts the real AngelScript compiler accepts"
          * doctest::skip(true))
{
    using namespace angel_lsp::analysis;
    using namespace angel_lsp::parser;

    const std::string harnessExe = EnvVar("ASHARNESS_EXE");
    if (harnessExe.empty() || !fs::exists(harnessExe))
    {
        MESSAGE("ASHARNESS_EXE is unset or does not exist - parity audit skipped.");
        return;
    }

    // Defaults to the sibling checkout layout; override for a different one.
    std::string harnessRoot = EnvVar("ASHARNESS_ROOT");
    if (harnessRoot.empty())
        harnessRoot = (fs::path(ANGELSCRIPT_CORPUS_DIR) / ".." / ".." / "AS-Harness").string();

    if (!fs::exists(harnessRoot))
    {
        MESSAGE("AS-Harness root not found - parity audit skipped.");
        return;
    }

    // PARITY_SCRIPT_DIR points the audit at any directory of .as files instead of the harness's
    // own. That is what lets the same machinery be run over the snippets extracted from this
    // suite's own tests - a far larger oracle corpus than the ten files AS-Harness ships, and the
    // one that actually says whether the expectations encoded in these tests match the language.
    std::vector<fs::path> scripts;
    const std::string overrideDir = EnvVar("PARITY_SCRIPT_DIR");

    if (!overrideDir.empty() && fs::exists(overrideDir))
    {
        for (const auto &entry : fs::directory_iterator(overrideDir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".as")
                scripts.push_back(entry.path());
        }
    }
    else
    {
        for (const char *sub : { "Tests", "examples" })
        {
            const fs::path dir = fs::path(harnessRoot) / sub;
            if (!fs::exists(dir))
                continue;

            for (const auto &entry : fs::directory_iterator(dir))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".as")
                    scripts.push_back(entry.path());
            }
        }
    }

    REQUIRE_MESSAGE(!scripts.empty(), "No parity scripts found under AS-Harness/Tests or /examples.");
    std::sort(scripts.begin(), scripts.end());

    static angel_lsp::i18n::I18n i18n;
    const fs::path predefinedPath = fs::path(harnessRoot) / "as.predefined";
    const std::string predefinedStub = fs::exists(predefinedPath) ? ReadFile(predefinedPath) : std::string();

    // Each corpus is paired with the stub it was actually written against, and only that one.
    //
    // AS-Harness's own scripts target the add-ons asharness registers, so as.predefined describes
    // them exactly. This suite's snippets were written against the server's built-in Standard
    // profile, which is what the server ships. Loading both at once is wrong in a way that is easy
    // to miss: they declare the same standard library, so every `array<T>` method arrives twice and
    // resolution had two identical candidates to choose between.
    // PARITY_PREDEFINED names the stub explicitly, and takes precedence over both defaults. It is
    // what pairs a corpus with an oracle that is not AS-Harness: server/tools/oracle registers the
    // SDK add-ons and nothing else, and tests/fixtures/sdk-addons.as.predefined is generated to
    // describe exactly that set. Pointing one at the other is what makes the comparison honest -
    // an oracle and a stub that disagree produce findings about the disagreement.
    const std::string explicitStubPath = EnvVar("PARITY_PREDEFINED");
    const std::string explicitStub =
        explicitStubPath.empty() ? std::string()
                                 : ReadFile(fs::path(explicitStubPath));

    if (!explicitStubPath.empty())
    {
        REQUIRE_MESSAGE(!explicitStub.empty(),
                        "PARITY_PREDEFINED is set but names a file that could not be read: "
                            << explicitStubPath);
    }

    const bool useBuiltinProfile = !overrideDir.empty() && explicitStubPath.empty();

    const std::string_view standardStub =
        useBuiltinProfile
            ? angel_lsp::analysis::GetProfileStubSource(angel_lsp::analysis::EngineProfileKind::Standard)
            : std::string_view();

    size_t agreeAccept = 0;
    size_t agreeReject = 0;
    size_t falseNegatives = 0;
    size_t analysed = 0;
    std::vector<ParityCase> falsePositives;
    std::vector<std::string> falseNegativeFiles;

    for (const auto &script : scripts)
    {
        const std::string source = ReadFile(script);
        if (source.empty())
            continue;

        ++analysed;

        ParityCase current;
        current.path = script;
        current.realAccepts = RealCompilerAccepts(harnessExe, script);

        // Fresh table per script, seeded with the stub describing exactly the add-ons asharness
        // registers - otherwise the analyzer is judging a different language than the compiler is.
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        if (!standardStub.empty())
            collector.CollectSymbols("file:///standard.as.predefined", std::string(standardStub), parser, table, &i18n);

        if (!explicitStub.empty())
        {
            collector.CollectSymbols("file:///parity.as.predefined", explicitStub, parser, table, &i18n);
        }
        else if (!useBuiltinProfile && !predefinedStub.empty())
        {
            // AS-Harness's stub predates the `@listpattern` convention, so the two add-on types it
            // declares are annotated here with the patterns their own registrations carry - see
            // scriptarray.cpp / scriptdictionary.cpp. Without them the harness would be compiling a
            // language this analyzer cannot see the list factories of.
            std::string annotated = predefinedStub;
            const auto annotate = [&annotated](const std::string &declaration, const std::string &pattern)
            {
                const size_t at = annotated.find(declaration);
                if (at != std::string::npos)
                {
                    annotated.insert(at, "/// @listpattern " + pattern + "\n");
                }
            };
            annotate("class array<T>", "{repeat T}");
            annotate("class dictionary", "{repeat {string, ?}}");

            collector.CollectSymbols("file:///as.predefined", annotated, parser, table, &i18n);
        }

        const std::string uri = "file:///" + script.filename().string();

        // The collector's return value carries the parse errors, and the server publishes them
        // alongside the analyzer's (Server.cpp: CollectSymbolsWithTree -> the same diagnostic
        // vector the client receives). Dropping them here made this test blind to every syntax
        // error: `Matrix m.Matrix();` produced a tree-sitter ERROR node, the collector reported it,
        // and the parity run scored the file as a miss because it only ever looked at Analyze().
        const auto collectorDiagnostics = collector.CollectSymbols(uri, source, parser, table, &i18n);

        // The server always analyses with a TypeConfig; running without one here made this harness
        // model something the server never is. It decides, among other things, which template is
        // the engine's default array - and therefore whether the initializer-list rule can say
        // anything at all about `array<int> a = {1, {2}}`.
        angel_lsp::config::TypeConfig types;

        SemanticAnalysisRequest request{ table, uri, ".as.predefined", &i18n };
        request.typeConfig = &types;

        std::shared_ptr<Scope> scopeRoot = scopes.CollectScopes(source, parser);
        request.scopeRoot = scopeRoot;
        request.mutableScopeRoot = scopeRoot.get();
        request.sourceCode = source;
        // Same `#if` exclusion the server applies - see utils/PreprocessorRegions.h.
        request.excludedLineRanges = angel_lsp::utils::FindExcludedLineRanges(source);
        request.tree = parser.Parse(source);

        SemanticAnalyzer analyzer(nullptr);
        auto ours = collectorDiagnostics;
        for (const auto &diagnostic : analyzer.Analyze(request))
            ours.push_back(diagnostic);

        for (const auto &diagnostic : ours)
        {
            if (diagnostic.severity == DiagnosticSeverity::Error)
                current.ourErrors.push_back(diagnostic);
        }

        if (request.tree)
            ts_tree_delete(const_cast<TSTree *>(request.tree));

        if (current.realAccepts)
        {
            if (current.ourErrors.empty())
                ++agreeAccept;
            else
                falsePositives.push_back(std::move(current));
        }
        else if (current.ourErrors.empty())
        {
            ++falseNegatives;
            falseNegativeFiles.push_back(current.path.filename().string());
        }
        else
        {
            ++agreeReject;
        }
    }

    std::vector<ParityCase> unexplained;
    std::vector<std::string> explained;

    std::vector<ParityCase> explainedCases;

    for (auto &c : falsePositives)
    {
        if (const KnownGap *gap = FindKnownGap(c.path))
        {
            explained.push_back(gap->fileName);
            explainedCases.push_back(std::move(c));
        }
        else
        {
            unexplained.push_back(std::move(c));
        }
    }

    std::cout << "\n=== AngelScript parity audit (" << analysed << " scripts) ===\n"
              << "  agree, both accept         : " << agreeAccept << "\n"
              << "  agree, both reject         : " << agreeReject << "\n"
              << "  known gaps (documented)    : " << explained.size() << "\n"
              << "  UNEXPLAINED FALSE POSITIVES: " << unexplained.size() << "   <- must be zero\n"
              << "  false negatives (by design): " << falseNegatives << "\n";

    // Listed, not just counted. A false negative is acceptable by design - this is not a compiler
    // - but this list is the only place a genuinely *missing rule* would show up, so it is worth
    // being able to read rather than merely tally.
    //
    // Every one of the twelve on the current snippet corpus was checked and none is a missing rule.
    // They split two ways: scripts naming a host type the stub does not declare ("Identifier
    // 'EHandle' is not a data type", "Missing definition of 'Vector3'"), where staying silent is the
    // deliberate policy; and scripts where asharness's own mixin-instantiation transform injects
    // synthetic code (_AutoMixinInstantiator_N) and then rejects it, which is an artefact of the
    // oracle rather than anything about the script.
    for (const auto &name : falseNegativeFiles)
        std::cout << "      [missed] " << name << "\n";

    for (const auto &c : unexplained)
    {
        std::cout << "\n  [UNEXPLAINED] " << c.path.filename().string()
                  << " - the real compiler accepts this file, we report "
                  << c.ourErrors.size() << " error(s):\n";

        for (const auto &d : c.ourErrors)
        {
            std::cout << "      line " << (d.range.start.line + 1) << "  "
                      << d.code << "  " << d.message << "\n";
        }
    }
    // Known gaps are printed too, not just counted. A gap that shrinks - because part of its cause
    // was fixed - is only visible if the remaining diagnostics are on screen, and that is exactly
    // the moment its recorded reason has gone stale.
    for (const auto &c : explainedCases)
    {
        std::cout << "\n  [known gap] " << c.path.filename().string()
                  << " - " << c.ourErrors.size() << " error(s) still reported:\n";

        for (const auto &d : c.ourErrors)
        {
            std::cout << "      line " << (d.range.start.line + 1) << "  "
                      << d.code << "  " << d.message << "\n";
        }
    }
    std::cout << std::endl;

    CHECK_MESSAGE(unexplained.empty(),
                  "This analyzer reports errors on scripts the real AngelScript compiler accepts, "
                  "for a reason not recorded in KnownGaps().");

    // The other direction: a documented gap that no longer reproduces has been fixed, and leaving
    // its entry in place would let a future regression hide behind it. Only meaningful against the
    // harness's own corpus - an overridden directory need not contain those files at all.
    if (!overrideDir.empty())
        return;

    for (const auto &gap : KnownGaps())
    {
        const bool stillFailing = std::any_of(explained.begin(), explained.end(),
                                              [&gap](const std::string &name) { return name == gap.fileName; });

        INFO("Known gap for " << gap.fileName << " no longer reproduces - remove it from KnownGaps().");
        CHECK(stillFailing);
    }
}
