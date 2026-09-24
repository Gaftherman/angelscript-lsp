#include "helpers/LspSemanticHarnessFixture.h"
#include <doctest/doctest.h>

using namespace angel_lsp::test;

TEST_SUITE("IncludeGraphHarness")
{

    TEST_CASE("Multi-file include graph resolves cross-file declarations and definitions")
    {
        LspSemanticHarnessFixture fixture;

        const std::string jsonScript = "class JsonDocument\n"
                                       "{\n"
                                       "    void Parse(const string&in s) {}\n"
                                       "    int GetNodeCount() const { return 0; }\n"
                                       "}\n";

        const std::string jsonUri = fixture.SandboxUri("scripts/json.as");
        fixture.AddVirtualDocument(jsonUri, jsonScript);
        fixture.AssertNoDiagnostics(jsonUri);

        const std::string v1Script = "#include \"json.as\"\n"
                                     "\n"
                                     "void V1Process()\n"
                                     "{\n"
                                     "    JsonDocument doc;\n"
                                     "    doc.Parse(\"{}\");\n"
                                     "}\n";

        const std::string v1Uri = fixture.SandboxUri("scripts/v1.as");
        fixture.AddVirtualDocument(v1Uri, v1Script);
        fixture.AssertNoDiagnostics(v1Uri);

        const std::string v2Script = "#include \"json.as\"\n"
                                     "\n"
                                     "void V2Process()\n"
                                     "{\n"
                                     "    JsonDocument doc;\n"
                                     "    int count = doc.GetNodeCount();\n"
                                     "    if (count > 0) {}\n"
                                     "}\n";

        const std::string v2Uri = fixture.SandboxUri("scripts/v2.as");
        fixture.AddVirtualDocument(v2Uri, v2Script);
        fixture.AssertNoDiagnostics(v2Uri);

        // In v1.as line 4, col 6: "JsonDocument" navigates to json.as line 0
        fixture.AssertDefinitionTarget(v1Uri, 4, 6, "json.as", 0);

        // In v2.as line 5, col 22: "GetNodeCount" navigates to json.as line 3
        fixture.AssertDefinitionTarget(v2Uri, 5, 22, "json.as", 3);

        // In v1.as line 4, col 6: Hover over "JsonDocument"
        fixture.AssertHoverContains(v1Uri, 4, 6, "JsonDocument");
    }

    TEST_CASE("Multi-file incremental update reflects modified declarations without cache poisoning")
    {
        LspSemanticHarnessFixture fixture;

        const std::string initialJson = "class JsonDocument\n"
                                        "{\n"
                                        "    void Parse(const string&in s) {}\n"
                                        "}\n";

        const std::string jsonUri = fixture.SandboxUri("scripts/json.as");
        fixture.AddVirtualDocument(jsonUri, initialJson);

        const std::string clientScript = "#include \"json.as\"\n"
                                         "\n"
                                         "void ClientCall()\n"
                                         "{\n"
                                         "    JsonDocument doc;\n"
                                         "    doc.Parse(\"{}\");\n"
                                         "}\n";

        const std::string clientUri = fixture.SandboxUri("scripts/client.as");
        fixture.AddVirtualDocument(clientUri, clientScript);
        fixture.AssertNoDiagnostics(clientUri);

        // Incremental update to json.as adding new method Compact()
        const std::string updatedJson = "class JsonDocument\n"
                                        "{\n"
                                        "    void Parse(const string&in s) {}\n"
                                        "    void Compact() {}\n"
                                        "}\n";
        fixture.UpdateVirtualDocument(jsonUri, updatedJson);

        // Client calls newly added method Compact()
        const std::string updatedClient = "#include \"json.as\"\n"
                                          "\n"
                                          "void ClientCall()\n"
                                          "{\n"
                                          "    JsonDocument doc;\n"
                                          "    doc.Compact();\n"
                                          "}\n";
        fixture.UpdateVirtualDocument(clientUri, updatedClient);
        fixture.AssertNoDiagnostics(clientUri);

        // Definition of Compact() resolves to updated json.as line 3
        fixture.AssertDefinitionTarget(clientUri, 5, 10, "json.as", 3);
    }
}
