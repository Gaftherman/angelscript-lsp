#include "analysis/index/SymbolTable.h"
#include "helpers/TestUtils.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace angel_lsp::analysis;

/**
 * @brief Builds a class or interface symbol spanning the given lines.
 * @param[in] name Unqualified class name.
 * @param[in] fileUri Owning document.
 * @param[in] lines First and last line of the class body.
 * @return Symbol ready for SymbolTable::AddSymbol.
 */
static Symbol MakeClassSpan(const std::string& name, const std::string& fileUri, std::pair<uint32_t, uint32_t> lines)
{
    Symbol sym;
    sym.type = SymbolType::Class;
    sym.name = name;
    sym.qualifiedName = name;
    sym.fileUri = fileUri;
    sym.startLine = lines.first;
    sym.endLine = lines.second;
    return sym;
}

/**
 * @brief Reference copy of the linear scan FindEnclosingClassName used before the span index.
 * @param[in] table Table searched.
 * @param[in] uri Document URI.
 * @param[in] line 0-based line.
 * @param[in] excludeName Class name to skip.
 * @return Narrowest enclosing class, with the same tie rule as the table.
 */
static std::string ReferenceEnclosingClass(const SymbolTable& table, std::string_view uri, uint32_t line,
                                           std::string_view excludeName)
{
    std::string result;
    uint32_t bestSpan = UINT32_MAX;
    table.ForEachSymbolInFile(uri,
                              [&](const std::string&, const std::vector<Symbol>& symbols)
                              {
                                  for (const auto& sym : symbols)
                                  {
                                      if ((sym.type != SymbolType::Class && sym.type != SymbolType::Interface) ||
                                          sym.fileUri != uri || (!excludeName.empty() && sym.name == excludeName) ||
                                          line < sym.startLine || line > sym.endLine)
                                      {
                                          continue;
                                      }
                                      const uint32_t span = sym.endLine - sym.startLine;
                                      if (span < bestSpan)
                                      {
                                          bestSpan = span;
                                          result = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                      }
                                  }
                              });
    return result;
}

TEST_SUITE("EnclosingClassIndex")
{
    TEST_CASE("Span index agrees with the linear scan on generated nested classes")
    {
        constexpr uint32_t kSeed = 20261118;
        MESSAGE("Seed: " << kSeed);
        std::mt19937 rng(kSeed);
        const std::string uri = "file:///" + angel_lsp::test::GenerateRandomSymbolName("enclosing") + ".as";

        SymbolTable table;
        for (int i = 0; i < 40; ++i)
        {
            const uint32_t start = rng() % 400;
            const uint32_t end = start + 1 + static_cast<uint32_t>(i); // distinct widths: ties are order-dependent
            table.AddSymbol(MakeClassSpan(angel_lsp::test::GenerateRandomSymbolName("Cls"), uri, {start, end}));
        }

        for (uint32_t line = 0; line < 500; ++line)
        {
            CHECK(table.FindEnclosingClassName(uri, line) == ReferenceEnclosingClass(table, uri, line, ""));
        }
    }

    TEST_CASE("Exclusion filter skips the named class")
    {
        const std::string uri = "file:///" + angel_lsp::test::GenerateRandomSymbolName("excl") + ".as";
        const std::string outer = angel_lsp::test::GenerateRandomSymbolName("Outer");
        const std::string inner = angel_lsp::test::GenerateRandomSymbolName("Inner");

        SymbolTable table;
        table.AddSymbol(MakeClassSpan(outer, uri, {0, 100}));
        table.AddSymbol(MakeClassSpan(inner, uri, {10, 20}));

        CHECK(table.FindEnclosingClassName(uri, 15) == inner);
        CHECK(table.FindEnclosingClassName(uri, 15, inner) == outer);
        CHECK(table.FindEnclosingClassName(uri, 150).empty());
    }

    TEST_CASE("Adding a class after a lookup is visible to the next lookup")
    {
        const std::string uri = "file:///" + angel_lsp::test::GenerateRandomSymbolName("fresh") + ".as";
        const std::string first = angel_lsp::test::GenerateRandomSymbolName("First");
        const std::string second = angel_lsp::test::GenerateRandomSymbolName("Second");

        SymbolTable table;
        table.AddSymbol(MakeClassSpan(first, uri, {0, 100}));
        CHECK(table.FindEnclosingClassName(uri, 50) == first);

        table.AddSymbol(MakeClassSpan(second, uri, {40, 60}));
        CHECK(table.FindEnclosingClassName(uri, 50) == second);
        CHECK(table.FindEnclosingClassName(uri, 70) == first);
    }
}
