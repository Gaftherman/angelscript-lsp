#include "analysis/DiagnosticSuppression.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NodeIndex.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include <doctest/doctest.h>
#include <ostream>
#include <string>

using namespace angel_lsp::analysis;
namespace codes = angel_lsp::diagnostics::codes;

TEST_CASE("DiagnosticSuppression - Alias lookup parity")
{
    CHECK(GetDiagnosticAlias(codes::ListPatternUnknown) == "W156");
    CHECK(GetDiagnosticAlias(codes::UnusedVariable) == "W103");
    CHECK(GetDiagnosticAlias(codes::ShadowGlobal) == "W104");
    CHECK(GetDiagnosticAlias(codes::BaseNotFound) == "E244");
    CHECK(GetDiagnosticAlias(codes::InvalidReferenceReturn) == "E245");
    CHECK(GetDiagnosticAlias(codes::ReadonlyHandle) == "E246");
    CHECK(GetDiagnosticAlias(codes::StandaloneReference) == "E247");
    CHECK(GetDiagnosticAlias(codes::UnknownType) == "E103");

    CHECK(GetCanonicalDiagnosticCode("W156") == codes::ListPatternUnknown);
    CHECK(GetCanonicalDiagnosticCode("w156") == codes::ListPatternUnknown);
    CHECK(GetCanonicalDiagnosticCode("W103") == codes::UnusedVariable);
    CHECK(GetCanonicalDiagnosticCode("W104") == codes::ShadowGlobal);
    CHECK(GetCanonicalDiagnosticCode("E244") == codes::BaseNotFound);
    CHECK(GetCanonicalDiagnosticCode("E245") == codes::InvalidReferenceReturn);
    CHECK(GetCanonicalDiagnosticCode("E246") == codes::ReadonlyHandle);
    CHECK(GetCanonicalDiagnosticCode("E247") == codes::StandaloneReference);
    CHECK(GetCanonicalDiagnosticCode("E103") == codes::UnknownType);
    CHECK(GetCanonicalDiagnosticCode("e103") == codes::UnknownType);

    CHECK(FormatDiagnosticDisplayCode(codes::ListPatternUnknown) == "W156");
    CHECK(FormatDiagnosticDisplayCode(codes::ShadowGlobal) == "W104");
    CHECK(FormatDiagnosticDisplayCode(codes::BaseNotFound) == "E244");
    CHECK(FormatDiagnosticDisplayCode("non-existent-code") == "non-existent-code");
}

TEST_CASE("DiagnosticSuppression - Disable line comment directive")
{
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("var");
    std::string source = "void main()\n"
                         "{\n"
                         "    int " +
                         varName +
                         "; // disable-line W103\n"
                         "    int other;\n"
                         "}\n";

    auto map = ParseDiagnosticSuppressions(source);
    CHECK(map.IsSuppressed(codes::UnusedVariable, 2));
    CHECK_FALSE(map.IsSuppressed(codes::UnusedVariable, 3));
    CHECK_FALSE(map.IsSuppressed(codes::UnknownType, 2));
}

TEST_CASE("DiagnosticSuppression - Disable range and re-enable")
{
    const std::string var1 = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string var2 = angel_lsp::test::GenerateRandomSymbolName("b");
    const std::string var3 = angel_lsp::test::GenerateRandomSymbolName("c");

    std::string source = "void main()\n"
                         "{\n"
                         "    // disable W103\n"
                         "    int " +
                         var1 +
                         ";\n"
                         "    int " +
                         var2 +
                         ";\n"
                         "    // enable W103\n"
                         "    int " +
                         var3 +
                         ";\n"
                         "}\n";

    auto map = ParseDiagnosticSuppressions(source);
    CHECK(map.IsSuppressed(codes::UnusedVariable, 3));
    CHECK(map.IsSuppressed(codes::UnusedVariable, 4));
    CHECK_FALSE(map.IsSuppressed(codes::UnusedVariable, 6));
}

TEST_CASE("DiagnosticSuppression - Disable all and multiple codes")
{
    std::string source = "// disable all\n"
                         "void foo() {}\n";

    auto map = ParseDiagnosticSuppressions(source);
    CHECK(map.IsSuppressed(codes::UnusedVariable, 1));
    CHECK(map.IsSuppressed(codes::UnknownType, 1));
    CHECK(map.IsSuppressed("any-random-code", 1));

    std::string multiSource = "int x; // disable-line W103, E103\n";

    auto multiMap = ParseDiagnosticSuppressions(multiSource);
    CHECK(multiMap.IsSuppressed(codes::UnusedVariable, 0));
    CHECK(multiMap.IsSuppressed(codes::UnknownType, 0));
}

TEST_CASE("DiagnosticSuppression - AST NodeIndex accelerated parsing")
{
    std::string source = "// disable W156\n"
                         "void test() {}\n"
                         "// enable W156\n";

    angel_lsp::parser::AngelScriptParser parser;
    TSTree* tree = parser.Parse(source);
    REQUIRE(tree != nullptr);

    NodeIndex nodeIndex(ts_tree_root_node(tree));
    auto map = ParseDiagnosticSuppressions(source, &nodeIndex);

    CHECK(map.IsSuppressed(codes::ListPatternUnknown, 1));
    CHECK_FALSE(map.IsSuppressed(codes::ListPatternUnknown, 3));
}
