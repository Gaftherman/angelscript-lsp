#include "helpers/TestUtils.h"
#include "utils/RegexCache.h"
#include "utils/StringPool.h"

#include <doctest/doctest.h>
#include <string>
#include <string_view>

TEST_CASE("StringPool - Interned views outlive their source and equal texts share storage")
{
    angel_lsp::utils::StringPool pool;
    const std::string name = angel_lsp::test::GenerateRandomSymbolName("pooled");
    std::string_view first;
    {
        std::string temporary = name;
        first = pool.Intern(temporary);
        temporary.assign(temporary.size(), '#');
    }
    CHECK(first == name);
    const std::string_view second = pool.Intern(std::string(name));
    CHECK(second.data() == first.data());
    CHECK(pool.Intern("") == std::string_view{});
}

TEST_CASE("RegexCache - Whole-match semantics, invalid patterns and empty input")
{
    const std::string stem = angel_lsp::test::GenerateRandomSymbolName("hook");
    const std::string pattern = stem + "_[0-9]+";
    CHECK(angel_lsp::utils::MatchesCachedRegex(stem + "_42", pattern));
    CHECK(angel_lsp::utils::MatchesCachedRegex(stem + "_42", pattern));
    CHECK_FALSE(angel_lsp::utils::MatchesCachedRegex(stem + "_42x", pattern));
    CHECK_FALSE(angel_lsp::utils::MatchesCachedRegex(stem, "(unclosed"));
    CHECK_FALSE(angel_lsp::utils::MatchesCachedRegex("", pattern));
    CHECK_FALSE(angel_lsp::utils::MatchesCachedRegex(stem, ""));
}
