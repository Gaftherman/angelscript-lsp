#include <doctest/doctest.h>

#include "helpers/TestUtils.h"
#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace angel_lsp::test;

namespace
{
/**
 * @brief Asserts that analyzing a mutated script completes gracefully without throwing or crashing.
 * @param[in] uri File URI identifying the test document.
 * @param[in] script Mutated source code string.
 */
void AssertGracefulAnalysis(const std::string& uri, const std::string& script)
{
    auto doc = CreateTestDocument(uri, script);
    REQUIRE(static_cast<bool>(doc));

    const auto diagnostics = doc->GetDiagnostics();
    // Semantic analysis must produce a bounded diagnostic collection without exploding memory
    CHECK(diagnostics.size() < 10000);
}

/**
 * @brief Fuzzes progressive source truncation simulating in-progress typing at random offsets.
 * @param[in] base Valid base script to truncate.
 * @param[in] iterations Number of random truncation samples to test.
 * @param[in,out] rng Seeded random engine.
 */
void FuzzTruncations(const std::string& base, size_t iterations, std::mt19937_64& rng)
{
    if (base.empty())
    {
        return;
    }
    std::uniform_int_distribution<size_t> dist(0, base.size());
    for (size_t i = 0; i < iterations; ++i)
    {
        const size_t cut = dist(rng);
        const std::string truncated = base.substr(0, cut);
        const std::string uri = "file:///fuzz_trunc_" + std::to_string(rng()) + ".as";
        AssertGracefulAnalysis(uri, truncated);
    }
}

/**
 * @brief Injects random delimiter mutations (mismatched braces, parentheses, quotes).
 * @param[in] base Valid base script.
 * @param[in] iterations Number of random delimiter corruptions to test.
 * @param[in,out] rng Seeded random engine.
 */
void FuzzDelimiters(const std::string& base, size_t iterations, std::mt19937_64& rng)
{
    static constexpr std::string_view kCorruptions[] = {
        "{", "}", "(", ")", "[", "]", "<", ">", "\"", "'", "/*", "*/", ";;", ":::", "@@"
    };
    std::uniform_int_distribution<size_t> posDist(0, base.size());
    std::uniform_int_distribution<size_t> itemDist(0, std::size(kCorruptions) - 1);

    for (size_t i = 0; i < iterations; ++i)
    {
        std::string mutated = base;
        const size_t pos = posDist(rng);
        const std::string_view token = kCorruptions[itemDist(rng)];
        mutated.insert(pos, token);

        const std::string uri = "file:///fuzz_delim_" + std::to_string(rng()) + ".as";
        AssertGracefulAnalysis(uri, mutated);
    }
}

/**
 * @brief Injects dangling operators and access chains at identifier boundaries.
 * @param[in] base Valid base script.
 * @param[in] iterations Number of random dangling operators to test.
 * @param[in,out] rng Seeded random engine.
 */
void FuzzDanglingOperators(const std::string& base, size_t iterations, std::mt19937_64& rng)
{
    static constexpr std::string_view kDangling[] = {
        ".", "->", "::", "+", "-", "*", "/", "==", "!=", "<", ">=", "?", "??", "[", "(", "="
    };
    std::uniform_int_distribution<size_t> posDist(0, base.size());
    std::uniform_int_distribution<size_t> itemDist(0, std::size(kDangling) - 1);

    for (size_t i = 0; i < iterations; ++i)
    {
        std::string mutated = base;
        const size_t pos = posDist(rng);
        const std::string_view op = kDangling[itemDist(rng)];
        mutated.insert(pos, op);

        const std::string uri = "file:///fuzz_op_" + std::to_string(rng()) + ".as";
        AssertGracefulAnalysis(uri, mutated);
    }
}

/**
 * @brief Constructs a deeply nested expression string to test depth bounding.
 * @param[in] depth Nesting depth.
 * @return Formatted script containing deeply nested syntax.
 */
std::string BuildNestedParensScript(size_t depth)
{
    std::string script = "void Main() {\n    int x = ";
    script.reserve(script.size() + depth * 2 + 16);
    for (size_t i = 0; i < depth; ++i)
    {
        script += "(";
    }
    script += "1";
    for (size_t i = 0; i < depth; ++i)
    {
        script += ")";
    }
    script += ";\n}\n";
    return script;
}
} // namespace

TEST_CASE("LSP Resilience Fuzzer - Progressive truncation simulating live keystrokes")
{
    std::mt19937_64 rng{20260926};
    const std::string className = GenerateRandomSymbolName("Weapon");
    const std::string methodName = GenerateRandomSymbolName("Fire");
    const std::string fieldName = GenerateRandomSymbolName("ammo");

    const std::string script =
        "class " + className + " {\n"
        "    int " + fieldName + " = 100;\n"
        "    void " + methodName + "(int count) {\n"
        "        if (count > 0 && " + fieldName + " >= count) {\n"
        "            " + fieldName + " -= count;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "void Execute() {\n"
        "    " + className + " obj;\n"
        "    obj." + methodName + "(10);\n"
        "}\n";

    FuzzTruncations(script, 32, rng);
}

TEST_CASE("LSP Resilience Fuzzer - Delimiter corruption and mismatched brackets")
{
    std::mt19937_64 rng{133742};
    const std::string cls = GenerateRandomSymbolName("Entity");
    const std::string script =
        "class " + cls + " {\n"
        "    int hp;\n"
        "    " + cls + "() { hp = 100; }\n"
        "    bool IsAlive() const { return hp > 0; }\n"
        "}\n"
        "void Update() {\n"
        "    " + cls + "@ e = " + cls + "();\n"
        "    if (e !is null && e.IsAlive()) { int x = 1; }\n"
        "}\n";

    FuzzDelimiters(script, 32, rng);
}

TEST_CASE("LSP Resilience Fuzzer - Dangling access operators and incomplete chains")
{
    std::mt19937_64 rng{987654};
    const std::string script =
        "void RunLoop() {\n"
        "    int val = 42;\n"
        "    string msg = \"status\";\n"
        "    for (int i = 0; i < 10; ++i) {\n"
        "        val += i * 2;\n"
        "    }\n"
        "}\n";

    FuzzDanglingOperators(script, 32, rng);
}

TEST_CASE("LSP Resilience Fuzzer - Deep recursion cap enforces bounded AST navigation")
{
    // Depth exceeding k_maxAstDepth = 64
    const std::string script = BuildNestedParensScript(256);
    const std::string uri = "file:///fuzz_deep_nesting_" + GenerateRandomSymbolName() + ".as";
    AssertGracefulAnalysis(uri, script);
}
