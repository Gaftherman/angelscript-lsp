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
