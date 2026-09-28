#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/TestUtils.h"
#include "spdlog/fmt/fmt.h"
#include <doctest/doctest.h>
#include <string>

using namespace angel_lsp::test;

TEST_SUITE("InitializerListParity")
{
    TEST_CASE("Anonymous dictionary argument in overloaded function call")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string fnName = GenerateRandomSymbolName("CreateEntity");
        const std::string entityType = GenerateRandomSymbolName("CBaseEntity");

        const std::string script = fmt::format(
            "class {0} {{}}\n"
            "{0}@ {1}(const string &in szClassName, dictionary@ pDictionary = null, bool fSpawn = true) {{\n"
            "    return null;\n"
            "}}\n"
            "void main() {{\n"
            "    {1}(\"monster_barney\", {{\n"
            "        {{ \"origin\", \"0 0 0\" }},\n"
            "        {{ \"angles\", \"0 0 0\" }},\n"
            "        {{ \"spawnflags\", \"1\" }}\n"
            "    }});\n"
            "}}\n",
            entityType, fnName);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated anonymous dictionary call argument");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Typed initializer list in index assignment (@dict[key] = array<Type@>@ = { @val })")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string itemType = GenerateRandomSymbolName("SpawnPoint");

        const std::string script = fmt::format(
            "class {0} {{}}\n"
            "dictionary SpawnList;\n"
            "void test({0}@ pNew) {{\n"
            "    @SpawnList[\"key\"] = array<{0}@>@ = {{ @pNew }};\n"
            "}}\n",
            itemType);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated typed initializer list in index assignment");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Typed initializer list in variable declaration and return statement")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string itemType = GenerateRandomSymbolName("Item");

        const std::string script = fmt::format(
            "class {0} {{}}\n"
            "array<{0}@>@ CreateItems({0}@ pItem) {{\n"
            "    array<{0}@>@ arr = array<{0}@>@ = {{ @pItem }};\n"
            "    if (@arr !is null) {{\n"
            "        return arr;\n"
            "    }}\n"
            "    return array<{0}@>@ = {{ @pItem }};\n"
            "}}\n",
            itemType);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated typed initializer list in declaration and return");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Typed initializer list in class member and chained assignments")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string script =
            "class MyContainer {\n"
            "    array<int>@ m_arr;\n"
            "    void init() {\n"
            "        @m_arr = array<int>@ = { 1, 2, 3 };\n"
            "    }\n"
            "}\n"
            "void chained() {\n"
            "    array<int>@ a;\n"
            "    array<int>@ b;\n"
            "    @a = @b = array<int>@ = { 1, 2, 3 };\n"
            "    if (@a !is null && @b !is null) {}\n"
            "}\n";

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated typed initializer list in member and chained assignment");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Dynamic rejection of initializer lists on non-container classes")
    {
        LspSemanticHarnessFixture fixture;
        const std::string clsName = GenerateRandomSymbolName("NotAContainer");
        const std::string fnName = GenerateRandomSymbolName("takeObject");

        const std::string script = fmt::format(
            "class {0} {{\n"
            "    void DoSomething() {{}}\n"
            "}}\n"
            "void {1}({0}@ obj) {{}}\n"
            "void main() {{\n"
            "    {1}({{ 1, 2 }});\n"
            "}}\n",
            clsName, fnName);

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertDiagnosticAt(docUri, 5, "as-err-no-implicit-conversion");
    }

    TEST_CASE("Template without list constructor or @listpattern rejects initializer list (asharness parity)")
    {
        LspSemanticHarnessFixture fixture;
        const std::string templateType = GenerateRandomSymbolName("Optional");
        const std::string fnName = GenerateRandomSymbolName("takeOptional");

        const std::string stubContent = fmt::format(
            "class {0}<T> {{\n"
            "    {0}();\n"
            "    {0}(const T &in val);\n"
            "    bool has_value() const;\n"
            "}}\n",
            templateType);

        const std::string stubUri = fixture.SandboxUri("custom.as.predefined");
        fixture.AddVirtualDocument(stubUri, stubContent);

        const std::string script = fmt::format(
            "void {0}({1}<int> opt) {{}}\n"
            "void main() {{\n"
            "    {0}({{ 42 }});\n"
            "}}\n",
            fnName, templateType);

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertDiagnosticAt(docUri, 2, "as-err-no-implicit-conversion");
    }

    TEST_CASE("Custom template with @listpattern accepts initializer list dynamically")
    {
        LspSemanticHarnessFixture fixture;
        const std::string templateType = GenerateRandomSymbolName("MyList");
        const std::string fnName = GenerateRandomSymbolName("processItems");

        const std::string stubContent = fmt::format(
            "/// @listpattern {{repeat T}}\n"
            "class {0}<T> {{\n"
            "    {0}();\n"
            "    uint length() const;\n"
            "}}\n",
            templateType);

        const std::string stubUri = fixture.SandboxUri("custom.as.predefined");
        fixture.AddVirtualDocument(stubUri, stubContent);

        const std::string script = fmt::format(
            "void {0}({1}<int> list) {{}}\n"
            "void main() {{\n"
            "    {0}({{ 10, 20, 30 }});\n"
            "}}\n",
            fnName, templateType);

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Custom aggregate class with list constructor accepts initializer list")
    {
        LspSemanticHarnessFixture fixture;
        const std::string vecType = GenerateRandomSymbolName("Vector3");
        const std::string fnName = GenerateRandomSymbolName("renderVertex");

        const std::string script = fmt::format(
            "class {0} {{\n"
            "    {0}(const int &in list) {{}}\n"
            "    float x;\n"
            "    float y;\n"
            "    float z;\n"
            "}}\n"
            "void {1}({0} v) {{}}\n"
            "void main() {{\n"
            "    {1}({{ 1.0f, 2.0f, 3.0f }});\n"
            "}}\n",
            vecType, fnName);

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Plain class without list constructor rejects initializer list (asharness parity)")
    {
        LspSemanticHarnessFixture fixture;
        const std::string vecType = GenerateRandomSymbolName("PlainVec3");
        const std::string fnName = GenerateRandomSymbolName("renderVertex");

        const std::string script = fmt::format(
            "class {0} {{\n"
            "    float x;\n"
            "    float y;\n"
            "    float z;\n"
            "}}\n"
            "void {1}({0} v) {{}}\n"
            "void main() {{\n"
            "    {1}({{ 1.0f, 2.0f, 3.0f }});\n"
            "}}\n",
            vecType, fnName);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        CHECK_FALSE(oracleAccepted);
        if (!oracleAccepted)
        {
            MESSAGE("asharness.exe correctly rejected initializer list for plain class without list constructor");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertDiagnosticAt(docUri, 7, "as-err-no-implicit-conversion");
    }

    TEST_CASE("Nested typed dictionary inside dictionary initializer list (asharness parity)")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string dictVar = GenerateRandomSymbolName("rootConfig");
        const std::string script = fmt::format(
            "void main() {{\n"
            "    dictionary {0} = {{\n"
            "        {{ \"version\", 1 }},\n"
            "        {{ \"settings\", dictionary = {{\n"
            "            {{ \"retries\", 3 }},\n"
            "            {{ \"timeout\", 100 }}\n"
            "        }} }}\n"
            "    }};\n"
            "    if ({0}.isEmpty()) {{}}\n"
            "}}\n",
            dictVar);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        CHECK(oracleAccepted);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated nested typed dictionary inside dictionary initializer");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Array of dictionaries and dictionary argument to array.insertLast (asharness parity)")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string arrVar = GenerateRandomSymbolName("entList");
        const std::string script = fmt::format(
            "void main() {{\n"
            "    array<dictionary> {0} = {{\n"
            "        {{ {{ \"origin\", \"0 0 0\" }}, {{ \"angles\", \"0 0 0\" }} }},\n"
            "        {{ {{ \"spawnflags\", \"1\" }} }}\n"
            "    }};\n"
            "    {0}.insertLast({{ {{ \"classname\", \"monster_barney\" }} }});\n"
            "}}\n",
            arrVar);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        CHECK(oracleAccepted);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated array of dictionaries and insertLast call");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Rejection of untyped anonymous initializer list inside dictionary for ? type (asharness parity)")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string dictVar = GenerateRandomSymbolName("invalidDict");
        const std::string script = fmt::format(
            "void main() {{\n"
            "    dictionary {0} = {{\n"
            "        {{ \"sub\", {{ {{ \"k\", 1 }} }} }}\n"
            "    }};\n"
            "    if ({0}.isEmpty()) {{}}\n"
            "}}\n",
            dictVar);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        CHECK_FALSE(oracleAccepted);
        if (!oracleAccepted)
        {
            MESSAGE("asharness.exe correctly rejected untyped nested initializer for '?' parameter");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertDiagnosticAt(docUri, 2, "as-err-initializer-list-not-supported");
    }

    TEST_CASE("Custom template container with @listpattern containing dictionaries")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string containerType = GenerateRandomSymbolName("CustomList");
        const std::string varName = GenerateRandomSymbolName("customListVar");

        const std::string predefContent = fmt::format(
            "/// @listpattern {{repeat T}}\n"
            "class {0}<T> {{\n"
            "    void insertLast(const T&in val);\n"
            "    uint length() const;\n"
            "}}\n",
            containerType);
        const std::string predefUri =
            fixture.SandboxUri(fmt::format("predef/{0}.as.predefined", GenerateRandomSymbolName("defs")));
        fixture.AddVirtualDocument(predefUri, predefContent);

        const std::string script = fmt::format(
            "void main() {{\n"
            "    {0}<dictionary> {1} = {{\n"
            "        {{ {{ \"key1\", 10 }} }},\n"
            "        {{ {{ \"key2\", 20 }} }}\n"
            "    }};\n"
            "    {1}.insertLast({{ {{ \"key3\", 30 }} }});\n"
            "}}\n",
            containerType, varName);

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }
}

