#include "helpers/LspSemanticHarnessFixture.h"
#include <doctest/doctest.h>

using namespace angel_lsp::test;

TEST_SUITE("SvenCoopIdiomsHarness")
{

    TEST_CASE("Exhaustive Switch CFG with terminal default return")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "bool ValidateType(int kind)\n"
                                   "{\n"
                                   "    switch (kind)\n"
                                   "    {\n"
                                   "        case 1:\n"
                                   "        case 2:\n"
                                   "            return true;\n"
                                   "        case 3:\n"
                                   "            return true;\n"
                                   "        default:\n"
                                   "            return false;\n"
                                   "    }\n"
                                   "}\n";

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated Exhaustive Switch CFG");
        }

        const std::string uri = fixture.SandboxUri("scripts/switch_cfg.as");
        fixture.AddVirtualDocument(uri, script);
        fixture.AssertNoDiagnostics(uri);
    }

    TEST_CASE("Implicit Constructor Synthesis for derived classes")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "class Deserializer\n"
                                   "{\n"
                                   "    void Reset() {}\n"
                                   "}\n"
                                   "\n"
                                   "class __Deserializer__ : Deserializer\n"
                                   "{\n"
                                   "}\n"
                                   "\n"
                                   "void TestInstantiation()\n"
                                   "{\n"
                                   "    __Deserializer__ d();\n"
                                   "    d.Reset();\n"
                                   "}\n";

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated Implicit Constructor Synthesis");
        }

        const std::string uri = fixture.SandboxUri("scripts/derived_ctor.as");
        fixture.AddVirtualDocument(uri, script);
        fixture.AssertNoDiagnostics(uri);
    }

    TEST_CASE("Scoped Enum as return type and expression value in nested namespaces")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "namespace meta_api\n"
                                   "{\n"
                                   "    namespace json\n"
                                   "    {\n"
                                   "        enum Version\n"
                                   "        {\n"
                                   "            V1 = 1,\n"
                                   "            V2 = 2\n"
                                   "        }\n"
                                   "\n"
                                   "        Version GetCurrentVersion()\n"
                                   "        {\n"
                                   "            return Version::V1;\n"
                                   "        }\n"
                                   "    }\n"
                                   "}\n"
                                   "\n"
                                   "void Main()\n"
                                   "{\n"
                                   "    meta_api::json::Version v = meta_api::json::GetCurrentVersion();\n"
                                   "    if (v == meta_api::json::Version::V1)\n"
                                   "    {\n"
                                   "    }\n"
                                   "}\n";

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated Scoped Enums in Namespaces");
        }

        const std::string uri = fixture.SandboxUri("scripts/scoped_enum.as");
        fixture.AddVirtualDocument(uri, script);
        fixture.AssertNoDiagnostics(uri);
    }

    TEST_CASE("Output Reference Handle Binding with this")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "class json\n"
                                   "{\n"
                                   "    void Deserialize(const string&in data, json@&out outTarget)\n"
                                   "    {\n"
                                   "    }\n"
                                   "\n"
                                   "    void SelfDeserialize(const string&in data)\n"
                                   "    {\n"
                                   "        Deserialize(data, this);\n"
                                   "    }\n"
                                   "}\n";

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated Output Reference Handle Binding");
        }

        const std::string uri = fixture.SandboxUri("scripts/handle_out_ref.as");
        fixture.AddVirtualDocument(uri, script);
        fixture.AssertNoDiagnostics(uri);
    }

    TEST_CASE("Mutating Reference String Semantics")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string validScript = "void Modify(string& text)\n"
                                        "{\n"
                                        "    text.resize(10);\n"
                                        "}\n"
                                        "\n"
                                        "void Process()\n"
                                        "{\n"
                                        "    string s = \"test\";\n"
                                        "    Modify(s);\n"
                                        "}\n";

        const std::string validUri = fixture.SandboxUri("scripts/valid_string_mut.as");
        fixture.AddVirtualDocument(validUri, validScript);
        fixture.AssertNoDiagnostics(validUri);

        const std::string invalidScript = "void Inspect(const string&in text)\n"
                                          "{\n"
                                          "    text.resize(10);\n"
                                          "}\n";
        const std::string invalidUri = fixture.SandboxUri("scripts/invalid_string_mut.as");
        fixture.AddVirtualDocument(invalidUri, invalidScript);
        fixture.AssertDiagnosticAt(invalidUri, 2, "as-err-const-method-required");
    }

    TEST_CASE("Preprocessor Lexical Anchoring across functions in namespace")
    {
        LspSemanticHarnessFixture fixture;
        fixture.Config().preprocessor.defineInScripts = true;

        const std::string script = "#define METAMOD_PLUGIN_ASLP\n"
                                   "\n"
                                   "namespace MyPlugin\n"
                                   "{\n"
                                   "#if METAMOD_PLUGIN_ASLP\n"
                                   "    bool __METAMOD__ = true;\n"
                                   "#endif\n"
                                   "\n"
                                   "    bool IsActive()\n"
                                   "    {\n"
                                   "        return __METAMOD__;\n"
                                   "    }\n"
                                   "}\n";

        const std::string uri = fixture.SandboxUri("scripts/preprocessor_anchor.as");
        fixture.AddVirtualDocument(uri, script);
        fixture.AssertNoDiagnostics(uri);
    }
}
