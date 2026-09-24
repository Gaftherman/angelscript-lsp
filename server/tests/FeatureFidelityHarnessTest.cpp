#include "helpers/LspSemanticHarnessFixture.h"
#include <doctest/doctest.h>

using namespace angel_lsp::test;

TEST_SUITE("FeatureFidelityHarness")
{

    TEST_CASE("Hover Inspection on properties displays accessor and documentation")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "class Clock\n"
                                   "{\n"
                                   "    /** @brief Elapsed minutes of the clock. */\n"
                                   "    uint8 Minutes\n"
                                   "    {\n"
                                   "        get const { return 0; }\n"
                                   "    }\n"
                                   "}\n"
                                   "\n"
                                   "void CheckClock()\n"
                                   "{\n"
                                   "    Clock c;\n"
                                   "    uint8 m = c.Minutes;\n"
                                   "}\n";

        const std::string uri = fixture.SandboxUri("scripts/hover_property.as");
        fixture.AddVirtualDocument(uri, script);

        // Line 12, col 16: "Minutes" in "c.Minutes"
        fixture.AssertHoverContains(uri, 12, 16, "Minutes");
    }

    TEST_CASE("Hover Inspection on intermediate namespace segments")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "namespace meta_api\n"
                                   "{\n"
                                   "    namespace json\n"
                                   "    {\n"
                                   "        namespace v2\n"
                                   "        {\n"
                                   "            namespace fmt\n"
                                   "            {\n"
                                   "                void ToArray() {}\n"
                                   "            }\n"
                                   "        }\n"
                                   "    }\n"
                                   "}\n"
                                   "\n"
                                   "void TestHover()\n"
                                   "{\n"
                                   "    meta_api::json::v2::fmt::ToArray();\n"
                                   "}\n";

        const std::string uri = fixture.SandboxUri("scripts/hover_namespace.as");
        fixture.AddVirtualDocument(uri, script);

        // Line 16, col 24: "fmt" in "meta_api::json::v2::fmt::ToArray()"
        fixture.AssertHoverContains(uri, 16, 24, "fmt");
    }

    TEST_CASE("Hover Member Disambiguation prioritizes receiver member over global symbol")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "enum PrintResult\n"
                                   "{\n"
                                   "    Error = 1\n"
                                   "}\n"
                                   "\n"
                                   "namespace Logger\n"
                                   "{\n"
                                   "    class ASLogLevel\n"
                                   "    {\n"
                                   "        bool IsActive() const { return true; }\n"
                                   "    }\n"
                                   "}\n"
                                   "\n"
                                   "class Context\n"
                                   "{\n"
                                   "    Logger::ASLogLevel Error;\n"
                                   "\n"
                                   "    void Process()\n"
                                   "    {\n"
                                   "        if (this.Error.IsActive())\n"
                                   "        {\n"
                                   "        }\n"
                                   "    }\n"
                                   "}\n";

        const std::string uri = fixture.SandboxUri("scripts/hover_member_disambig.as");
        fixture.AddVirtualDocument(uri, script);

        // Line 19, col 17: "Error" in "this.Error.IsActive()"
        fixture.AssertHoverContains(uri, 19, 17, "ASLogLevel");
    }

    TEST_CASE("Completion Filtering isolates instance members from global and type symbols")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "class DataContainer\n"
                                   "{\n"
                                   "    void Get() {}\n"
                                   "    void Set() {}\n"
                                   "    void ValueOrDefault() {}\n"
                                   "}\n"
                                   "\n"
                                   "void TestComp()\n"
                                   "{\n"
                                   "    DataContainer data;\n"
                                   "    data.\n"
                                   "}\n";

        const std::string uri = fixture.SandboxUri("scripts/completion_instance.as");
        fixture.AddVirtualDocument(uri, script);

        // Line 10, col 9: after "data."
        fixture.AssertCompletionContains(uri, 10, 9, "Get", lsp::CompletionItemKind::Method);
        fixture.AssertCompletionContains(uri, 10, 9, "Set", lsp::CompletionItemKind::Method);
        fixture.AssertCompletionContains(uri, 10, 9, "ValueOrDefault", lsp::CompletionItemKind::Method);
        fixture.AssertCompletionExcludes(uri, 10, 9, "DataContainer");
    }

    TEST_CASE("Completion Filtering in namespace access proposes nested containers and types")
    {
        LspSemanticHarnessFixture fixture;
        const std::string script = "namespace meta_api\n"
                                   "{\n"
                                   "    namespace json\n"
                                   "    {\n"
                                   "        class Helper {}\n"
                                   "        enum Type { Object, Array }\n"
                                   "    }\n"
                                   "}\n"
                                   "\n"
                                   "void TestCompNamespace()\n"
                                   "{\n"
                                   "    int localVariable = 42;\n"
                                   "    meta_api::json::\n"
                                   "}\n";

        const std::string uri = fixture.SandboxUri("scripts/completion_ns.as");
        fixture.AddVirtualDocument(uri, script);

        // Line 12, col 20: after "meta_api::json::"
        fixture.AssertCompletionContains(uri, 12, 20, "Helper", lsp::CompletionItemKind::Class);
        fixture.AssertCompletionContains(uri, 12, 20, "Type", lsp::CompletionItemKind::Enum);
        fixture.AssertCompletionExcludes(uri, 12, 20, "localVariable");
    }

    TEST_CASE("Go-To-Definition Precedence prioritizes local workspace declarations over external stubs")
    {
        LspSemanticHarnessFixture fixture;
        fixture.LoadPredefinedStub("tests/fixtures/sdk-addons.as.predefined");

        const std::string jsonScript = "namespace meta_api\n"
                                       "{\n"
                                       "    namespace json\n"
                                       "    {\n"
                                       "        enum Type\n"
                                       "        {\n"
                                       "            Object = 1,\n"
                                       "            Array = 2\n"
                                       "        }\n"
                                       "    }\n"
                                       "}\n";

        const std::string jsonUri = fixture.SandboxUri("scripts/json.as");
        fixture.AddVirtualDocument(jsonUri, jsonScript);

        const std::string userScript = "void Main()\n"
                                       "{\n"
                                       "    int x = meta_api::json::Type::Object;\n"
                                       "}\n";

        const std::string userUri = fixture.SandboxUri("scripts/user.as");
        fixture.AddVirtualDocument(userUri, userScript);

        // Line 2, col 28: "Type" in "meta_api::json::Type::Object"
        fixture.AssertDefinitionTarget(userUri, 2, 28, "json.as", 4);
    }
}
