#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/TypeSanitization.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{
TEST_SUITE("analysis/types/VarTypeToken")
{
    TEST_CASE("VarTypeToken - IsVariableTypeToken matches ? with optional reference decoration")
    {
        struct Case
        {
            std::string_view input;
            bool expected;
        };

        constexpr Case k_cases[] = {
            {"?", true},     {"?&", true},       {"?&in", true},        {"?&out", true},      {"?&inout", true},
            {"? &in", true}, {" ? &out ", true}, {"...?&in", true},     {"... ?", true},      {"...int", false},
            {"...", false},  {"int", false},     {"array<int>", false}, {"T", false},         {"", false},
            {"??", false},   {"?foo", false},    {"array<?>", false},   {"const int", false},
        };

        for (const Case& c : k_cases)
        {
            CAPTURE(c.input);
            CHECK(IsVariableTypeToken(c.input) == c.expected);
        }
    }

    TEST_CASE("VarTypeToken - Predefined ?&in parameter parses as TypeKind::VarType while int does not")
    {
        const std::string varFn = angel_lsp::test::GenerateRandomSymbolName("varFn");
        const std::string intFn = angel_lsp::test::GenerateRandomSymbolName("intFn");
        const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("v");

        const std::string code =
            "void " + varFn + "(?&in " + paramName + ");\n" + "void " + intFn + "(int " + paramName + ");\n";

        angel_lsp::parser::AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        SymbolTable table;
        collector.CollectSymbols("file:///engine.as.predefined", code, parser, table);

        const auto varSyms = table.FindSymbols(varFn);
        REQUIRE(varSyms.size() == 1);
        REQUIRE(varSyms[0].type == SymbolType::Function);
        const auto& varParams = varSyms[0].GetFunction().parameters;
        REQUIRE(varParams.size() == 1);
        CHECK(varParams[0].typeKind == TypeKind::VarType);

        const auto intSyms = table.FindSymbols(intFn);
        REQUIRE(intSyms.size() == 1);
        REQUIRE(intSyms[0].type == SymbolType::Function);
        const auto& intParams = intSyms[0].GetFunction().parameters;
        REQUIRE(intParams.size() == 1);
        CHECK(intParams[0].typeKind != TypeKind::VarType);
        CHECK(intParams[0].typeKind == TypeKind::Int32);
    }
}
} // namespace angel_lsp::analysis
