#include "helpers/TestUtils.h"
#include "lsp/DiagnosticsPipeline.h"
#include "lsp/LspSessionCoordinator.h"
#include "lsp/WorkspaceStateStore.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace angel_lsp;

TEST_SUITE("ServerDomainServices")
{
    TEST_CASE("WorkspaceStateStore manages snapshots and defined words")
    {
        WorkspaceStateStore store;
        const std::string rootA = "file:///workspace/" + test::GenerateRandomSymbolName("rootA");
        const std::string rootB = "file:///workspace/" + test::GenerateRandomSymbolName("rootB");
        const std::string dirA = "/path/to/" + test::GenerateRandomSymbolName("incA");

        store.SetWorkspaceRoots({rootA, rootB});
        store.SetSearchDirectories({dirA});
        store.SetEngineProfile("SvenCoop");

        WorkspaceSnapshot snapshot = store.CreateSnapshot();
        REQUIRE(snapshot.searchDirectories != nullptr);
        REQUIRE(snapshot.searchDirectories->size() == 1);
        CHECK((*snapshot.searchDirectories)[0] == dirA);
        CHECK(snapshot.workspaceRoots.size() == 2);
        CHECK(snapshot.workspaceRoots[0] == rootA);
        CHECK(store.GetEngineProfile() == "SvenCoop");

        // Defined words contributions
        const std::string stubPath = "/stubs/" + test::GenerateRandomSymbolName("stub") + ".as";
        const std::string word1 = test::GenerateRandomSymbolName("FLAG_A");
        const std::string word2 = test::GenerateRandomSymbolName("FLAG_B");

        CHECK(store.SetDefinedWordsFrom(stubPath, {word1, word2}));
        auto words = store.GetDefinedWords();
        REQUIRE(words != nullptr);
        CHECK(words->contains(word1));
        CHECK(words->contains(word2));

        // Removing contribution removes from merged set
        CHECK(store.SetDefinedWordsFrom(stubPath, {}));
        words = store.GetDefinedWords();
        REQUIRE(words != nullptr);
        CHECK(!words->contains(word1));
        CHECK(!words->contains(word2));

        store.Clear();
        CHECK(store.GetWorkspaceRoots().empty());
        CHECK(store.GetEngineProfile().empty());
    }

    TEST_CASE("DiagnosticsPipeline caches and manages snapshots and revisions")
    {
        DiagnosticsPipeline pipeline;

        const std::string docUri = "file:///workspace/" + test::GenerateRandomSymbolName("doc") + ".as";
        CHECK(!pipeline.GetCachedSnapshot(docUri).has_value());

        DiagnosticsSnapshot snap;
        snap.resultId = "rev_1";
        snap.version = 42;
        snap.generation = 100;
        snap.textHash = 12345;

        pipeline.CacheDiagnosticsSnapshot(docUri, snap);
        auto retrieved = pipeline.GetCachedSnapshot(docUri);
        REQUIRE(retrieved.has_value());
        CHECK(retrieved->resultId == "rev_1");
        CHECK(retrieved->version == 42);
        CHECK(retrieved->generation == 100);

        uint64_t rev1 = pipeline.NextRevision();
        uint64_t rev2 = pipeline.NextRevision();
        CHECK(rev2 > rev1);

        pipeline.InvalidateDiagnostics(docUri);
        CHECK(!pipeline.GetCachedSnapshot(docUri).has_value());

        // Diagnostic severities
        ankerl::unordered_dense::map<std::string, analysis::DiagnosticSeverity> overrides;
        overrides["as-err-test"] = analysis::DiagnosticSeverity::Warning;
        pipeline.SetDiagnosticSeverities(overrides);
        CHECK(pipeline.GetDiagnosticSeverities().size() == 1);
        CHECK(pipeline.GetDiagnosticSeverities().at("as-err-test") == analysis::DiagnosticSeverity::Warning);
    }

    TEST_CASE("LspSessionCoordinator coordinates session state and readiness barriers")
    {
        LspSessionCoordinator session;
        CHECK(!session.IsRunning());
        session.Start();
        CHECK(session.IsRunning());

        CHECK(!session.IsWorkspaceScanComplete());
        session.SetWorkspaceScanComplete(true);
        CHECK(session.IsWorkspaceScanComplete());

        CHECK(!session.IsPredefinedReady());
        session.SetPredefinedReady(true);
        CHECK(session.IsPredefinedReady());
        session.WaitUntilPredefinedReady(); // Should return immediately without blocking

        session.SetPositionEncoding(utils::PositionEncoding::Utf8);
        CHECK(session.GetPositionEncoding() == utils::PositionEncoding::Utf8);

        session.SetFormatBraceStyleKR(true);
        CHECK(session.GetFormatBraceStyleKR());

        uint64_t cRev = session.IncrementConfigRevision();
        CHECK(session.GetConfigRevision() == cRev);

        session.RequestShutdown();
        CHECK(!session.IsRunning());
    }
}
