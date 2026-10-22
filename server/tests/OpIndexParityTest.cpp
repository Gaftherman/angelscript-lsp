/**
 * @file OpIndexParityTest.cpp
 * @brief Comprehensive parity tests for opIndex overloads across types and member autocompletion.
 */

#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/TestUtils.h"
#include "spdlog/fmt/fmt.h"
#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

namespace
{
using namespace angel_lsp;
using namespace angel_lsp::test;

/**
 * @brief Helper to check if a completion list contains an item with the expected label.
 * @param[in] items List of LSP completion items.
 * @param[in] label Target label to search for.
 * @return True if found; false otherwise.
 */
bool HasItemLabel(const std::vector<lsp::CompletionItem>& items, std::string_view label)
{
    return std::any_of(items.begin(), items.end(),
                       [label](const lsp::CompletionItem& it) { return it.label == label; });
}
} // namespace

TEST_SUITE("OpIndexParity")
{
    TEST_CASE("OpIndexParity - Overloaded opIndex across int, float, bool, string, and class handles verified with "
              "native engine oracle")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string valClass = GenerateRandomSymbolName("ValClass");
        const std::string containerClass = GenerateRandomSymbolName("Container");
        const std::string secretField = GenerateRandomSymbolName("secret");
        const std::string getSecretMethod = GenerateRandomSymbolName("GetSecret");

        const std::string script =
            fmt::format("class {0} {{\n"
                        "    int {2};\n"
                        "    int {3}() const {{ return {2}; }}\n"
                        "}}\n"
                        "class {1} {{\n"
                        "    int opIndex(int idx) {{ return idx * 2; }}\n"
                        "    string opIndex(float f) {{ return \"float_result\"; }}\n"
                        "    bool opIndex(bool b) {{ return !b; }}\n"
                        "    {0}@ opIndex(string key) {{\n"
                        "        {0} v;\n"
                        "        v.{2} = 99;\n"
                        "        return v;\n"
                        "    }}\n"
                        "    float opIndex({0}@ obj) {{\n"
                        "        if (obj is null) return 0.0f;\n"
                        "        return float(obj.{2});\n"
                        "    }}\n"
                        "}}\n"
                        "void main() {{\n"
                        "    {1} c;\n"
                        "    int a = c[10];\n"
                        "    string s = c[2.5f];\n"
                        "    int sLen = c[2.5f].length();\n"
                        "    bool b = c[true];\n"
                        "    {0}@ valObj = c[\"my_key\"];\n"
                        "    int secret = c[\"my_key\"].{3}();\n"
                        "    float f = c[valObj];\n"
                        "    if (a > 0 && s.length() > 0 && sLen > 0 && b && secret > 0 && f > 0.0f) {{\n"
                        "    }}\n"
                        "}}\n",
                        valClass, containerClass, secretField, getSecretMethod);

        if (fixture.HasOracleBinary())
        {
            std::string oracleError;
            const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
            CHECK(oracleAccepted);
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("OpIndexParity - Inherited opIndex overloads across class hierarchy verified with native engine oracle")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string valClass = GenerateRandomSymbolName("ValClass");
        const std::string baseContainer = GenerateRandomSymbolName("BaseContainer");
        const std::string derivedContainer = GenerateRandomSymbolName("DerivedContainer");
        const std::string secretField = GenerateRandomSymbolName("secret");
        const std::string getSecretMethod = GenerateRandomSymbolName("GetSecret");

        const std::string script =
            fmt::format("class {0} {{\n"
                        "    int {3};\n"
                        "    int {4}() const {{ return {3}; }}\n"
                        "}}\n"
                        "class {1} {{\n"
                        "    int opIndex(int idx) {{ return idx * 10; }}\n"
                        "}}\n"
                        "class {2} : {1} {{\n"
                        "    string opIndex(float f) {{ return \"from_derived\"; }}\n"
                        "    {0}@ opIndex(string key) {{\n"
                        "        {0} v;\n"
                        "        v.{3} = 42;\n"
                        "        return v;\n"
                        "    }}\n"
                        "}}\n"
                        "void main() {{\n"
                        "    {2} d;\n"
                        "    int num = d[5];\n"
                        "    string str = d[3.14f];\n"
                        "    int len = d[3.14f].length();\n"
                        "    {0}@ obj = d[\"entry\"];\n"
                        "    int sec = d[\"entry\"].{4}();\n"
                        "    if (num > 0 && str.length() > 0 && len > 0 && sec > 0 && obj !is null) {{\n"
                        "    }}\n"
                        "}}\n",
                        valClass, baseContainer, derivedContainer, secretField, getSecretMethod);

        if (fixture.HasOracleBinary())
        {
            std::string oracleError;
            const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
            CHECK(oracleAccepted);
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("OpIndexParity - Global indexed property accessor with chained member access verified with oracle")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string propName = GenerateRandomSymbolName("myStrings");
        const std::string globalVar = GenerateRandomSymbolName("backingVar");

        const std::string script = fmt::format("string {0};\n"
                                               "string get_{1}(int idx) property {{\n"
                                               "    return {0};\n"
                                               "}}\n"
                                               "void set_{1}(int idx, const string &in val) property {{\n"
                                               "    {0} = val;\n"
                                               "}}\n"
                                               "void main() {{\n"
                                               "    {1}[0] = \"hello\";\n"
                                               "    int len = {1}[0].length();\n"
                                               "    if (len > 0) {{}}\n"
                                               "}}\n",
                                               globalVar, propName);

        if (fixture.HasOracleBinary())
        {
            std::string oracleError;
            const bool oracleAccepted =
                fixture.VerifyWithNativeOracle(script, oracleError, "--property-accessor-mode=3");
            CHECK(oracleAccepted);
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("OpIndexParity - Member autocompletion on indexed expression selects correct overload type")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string valClass = GenerateRandomSymbolName("ValClass");
        const std::string containerClass = GenerateRandomSymbolName("Container");
        const std::string secretField = GenerateRandomSymbolName("secretVal");
        const std::string getSecretMethod = GenerateRandomSymbolName("ComputeSecret");

        const std::string scriptKey = fmt::format("class {0} {{\n"
                                                  "    int {2};\n"
                                                  "    int {3}() const {{ return {2}; }}\n"
                                                  "}}\n"
                                                  "class {1} {{\n"
                                                  "    int opIndex(int idx) {{ return idx; }}\n"
                                                  "    string opIndex(float f) {{ return \"test\"; }}\n"
                                                  "    bool opIndex(bool b) {{ return b; }}\n"
                                                  "    {0}@ opIndex(string key) {{ return null; }}\n"
                                                  "}}\n"
                                                  "void main() {{\n"
                                                  "    {1} c;\n"
                                                  "    c[\"key\"].\n"
                                                  "}}\n",
                                                  valClass, containerClass, secretField, getSecretMethod);

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, scriptKey);

        // Completion on line 12 right after c["key"]. (col 14)
        auto items = fixture.RequestCompletion(docUri, 12, 14);
        CHECK(HasItemLabel(items, secretField));
        CHECK(HasItemLabel(items, getSecretMethod));
        CHECK_FALSE(HasItemLabel(items, "length"));

        // Update document with float index: c[2.5f]. (col 12)
        const std::string scriptFloat = fmt::format("class {0} {{\n"
                                                    "    int {2};\n"
                                                    "    int {3}() const {{ return {2}; }}\n"
                                                    "}}\n"
                                                    "class {1} {{\n"
                                                    "    int opIndex(int idx) {{ return idx; }}\n"
                                                    "    string opIndex(float f) {{ return \"test\"; }}\n"
                                                    "    bool opIndex(bool b) {{ return b; }}\n"
                                                    "    {0}@ opIndex(string key) {{ return null; }}\n"
                                                    "}}\n"
                                                    "void main() {{\n"
                                                    "    {1} c;\n"
                                                    "    c[2.5f].\n"
                                                    "}}\n",
                                                    valClass, containerClass, secretField, getSecretMethod);

        fixture.UpdateVirtualDocument(docUri, scriptFloat);
        auto itemsFloat = fixture.RequestCompletion(docUri, 12, 12);
        CHECK(HasItemLabel(itemsFloat, "length"));
        CHECK_FALSE(HasItemLabel(itemsFloat, secretField));
    }

    TEST_CASE("OpIndexParity - Member autocompletion on global and member indexed properties")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string globalArrayProp = GenerateRandomSymbolName("stringArray");
        const std::string invClass = GenerateRandomSymbolName("Inventory");
        const std::string itemsProp = GenerateRandomSymbolName("items");

        // Test 1: Global indexed property
        const std::string scriptGlobal = fmt::format("string get_{0}(int idx) property {{ return \"\"; }}\n"
                                                     "void main() {{\n"
                                                     "    {0}[0].\n"
                                                     "}}\n",
                                                     globalArrayProp);

        const std::string docUri1 =
            fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("docGlobal")));
        fixture.AddVirtualDocument(docUri1, scriptGlobal);
        const uint32_t colGlobal = static_cast<uint32_t>(4 + globalArrayProp.size() + 4);
        auto globalItems = fixture.RequestCompletion(docUri1, 2, colGlobal);
        CHECK(HasItemLabel(globalItems, "length"));

        // Test 2: Member indexed property from outside
        const std::string scriptMember = fmt::format("class {0} {{\n"
                                                     "    string get_{1}(int idx) property {{ return \"\"; }}\n"
                                                     "}}\n"
                                                     "void main() {{\n"
                                                     "    {0} inv;\n"
                                                     "    inv.{1}[0].\n"
                                                     "}}\n",
                                                     invClass, itemsProp);

        const std::string docUri2 =
            fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("docMember")));
        fixture.AddVirtualDocument(docUri2, scriptMember);
        const uint32_t colMember = static_cast<uint32_t>(8 + itemsProp.size() + 4);
        auto memberItems = fixture.RequestCompletion(docUri2, 5, colMember);
        CHECK(HasItemLabel(memberItems, "length"));

        // Test 3: Member indexed property inside class method
        const std::string scriptInside = fmt::format("class {0} {{\n"
                                                     "    string get_{1}(int idx) property {{ return \"\"; }}\n"
                                                     "    void Inspect() {{\n"
                                                     "        {1}[0].\n"
                                                     "    }}\n"
                                                     "}}\n",
                                                     invClass, itemsProp);

        const std::string docUri3 =
            fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("docInside")));
        fixture.AddVirtualDocument(docUri3, scriptInside);
        const uint32_t colInside = static_cast<uint32_t>(8 + itemsProp.size() + 4);
        auto insideItems = fixture.RequestCompletion(docUri3, 3, colInside);
        CHECK(HasItemLabel(insideItems, "length"));
    }
}
