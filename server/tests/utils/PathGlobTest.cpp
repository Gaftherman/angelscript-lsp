#include "utils/PathGlob.h"

#include "helpers/TestUtils.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

TEST_CASE("PathGlob - Workspace-relative glob boundaries and directory depth")
{
    const auto folder = angel_lsp::test::GenerateRandomSymbolName("generated");
    const auto file = angel_lsp::test::GenerateRandomSymbolName("script") + ".as";
    const auto nested = folder + "/sub/" + file;
    const auto root = "/workspace/" + angel_lsp::test::GenerateRandomSymbolName("root");
    const auto nestedRoot = root + "/" + folder;
    const std::vector<std::string> roots{root, nestedRoot};
    const auto path = root + "/" + nested;
    CHECK(angel_lsp::utils::RelativeToWorkspaceRoot(path, roots) == "sub/" + file);
    CHECK_FALSE(angel_lsp::utils::RelativeToWorkspaceRoot(root + "other/" + nested, roots));

    CHECK_FALSE(angel_lsp::utils::PathGlob(folder + "/*.as").Matches(nested));
    CHECK(angel_lsp::utils::PathGlob(folder + "/**/*.as").Matches(nested));
    CHECK(angel_lsp::utils::PathGlob(folder + "/**/*.as").Matches(folder + "/" + file));
    CHECK_FALSE(angel_lsp::utils::PathGlob(folder + "/**/" + file).Matches(folder + "/extra" + file));
    CHECK(angel_lsp::utils::PathGlob(folder + "/").Matches(nested));
    CHECK_FALSE(angel_lsp::utils::PathGlob(folder + "/").Matches(folder + "other/" + file));
    CHECK(angel_lsp::utils::PathGlob(file).Matches(nested));
    CHECK(angel_lsp::utils::PathGlob(folder + "\\?ub\\*.as").Matches(nested));
}
