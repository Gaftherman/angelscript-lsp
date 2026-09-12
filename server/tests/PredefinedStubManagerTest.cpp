#include <doctest/doctest.h>
#include "lsp/PredefinedStubManager.h"

using namespace angel_lsp;

TEST_CASE("PredefinedStubManager - Register, Lookup, and Contributes")
{
    PredefinedStubManager mgr;

    const std::string path = "C:/game/api.as.predefined";
    const std::string uri = "file:///C:/game/api.as.predefined";
    const std::string content = "class GameEngine { void Tick(); }";

    CHECK_FALSE(mgr.HasStub(uri));
    CHECK(mgr.Size() == 0);

    mgr.RegisterStub(path, uri, content);
    CHECK(mgr.HasStub(uri));
    CHECK(mgr.Size() == 1);

    auto text = mgr.GetDocumentText(uri);
    REQUIRE(text.has_value());
    CHECK(*text == content);

    auto lookedUpUri = mgr.GetUriByPath(path);
    REQUIRE(lookedUpUri.has_value());
    CHECK(*lookedUpUri == uri);

    CHECK(mgr.Contributes(uri, ""));
    CHECK(mgr.Contributes(uri, "api.as.predefined"));

    mgr.RemoveStub(uri);
    CHECK_FALSE(mgr.HasStub(uri));
    CHECK(mgr.Size() == 0);
    CHECK(mgr.GetUriByPath(path) == std::nullopt);
}

TEST_CASE("PredefinedStubManager - ClaimFile and UnloadUri")
{
    PredefinedStubManager mgr;

    const std::string path = "C:/game/api.as.predefined";
    const std::string uri1 = "file:///C:/game/api.as.predefined";
    const std::string uri2 = "file:///c%3A/game/api.as.predefined";

    std::string previous;
    bool claimed = mgr.ClaimFile(uri1, path, false, &previous);
    CHECK(claimed);
    CHECK(previous.empty());
    CHECK(mgr.HasStub(uri1));
    CHECK(mgr.GetUriByPath(path) == uri1);

    // Re-claiming with same URI returns false if forceReload=false
    claimed = mgr.ClaimFile(uri1, path, false, &previous);
    CHECK_FALSE(claimed);

    // Re-claiming with new URI displaces previous URI
    claimed = mgr.ClaimFile(uri2, path, false, &previous);
    CHECK(claimed);
    CHECK(previous == uri1);
    CHECK_FALSE(mgr.HasStub(uri1));
    CHECK(mgr.HasStub(uri2));
    CHECK(mgr.GetUriByPath(path) == uri2);

    auto all = mgr.GetAllUriByPath();
    REQUIRE(all.size() == 1);
    CHECK(all[0].first == path);
    CHECK(all[0].second == uri2);

    std::string unloadedPath;
    bool unloaded = mgr.UnloadUri(uri2, &unloadedPath);
    CHECK(unloaded);
    CHECK(unloadedPath == path);
    CHECK_FALSE(mgr.HasStub(uri2));
    CHECK(mgr.GetUriByPath(path) == std::nullopt);
}

