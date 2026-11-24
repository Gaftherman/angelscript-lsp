#include "helpers/TestUtils.h"
#include "helpers/UnifiedFeatureAuditHarness.h"

#include <doctest/doctest.h>

using namespace angel_lsp::test;

TEST_SUITE("oracle/EmptyParenAndTelemetryParity")
{

    TEST_CASE("EmptyParenAndTelemetryParity - Empty parentheses variable declarations good and boundary paths")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string cls = GenerateRandomSymbolName("Component");
        const std::string fieldName = GenerateRandomSymbolName("m_counter");
        const std::string goodScript = "class " + cls +
                                       " {\n"
                                       "    int " +
                                       fieldName +
                                       "();\n"
                                       "    void Run() {\n"
                                       "        this." +
                                       fieldName +
                                       " = 42;\n"
                                       "        " +
                                       fieldName +
                                       " = 100;\n"
                                       "    }\n"
                                       "}\n";
        const std::string uri =
            harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("ep_good") + ".as", goodScript);

        // Hover at fieldName definition (line 1, col 9)
        auto hover = harness.AuditHover(uri, 1, 9);
        REQUIRE(hover.has_value());

        // Boundary: reference-returning function should NOT become a variable
        const std::string refCls = GenerateRandomSymbolName("RefCls");
        const std::string boundaryScript = "class " + refCls +
                                           " {\n"
                                           "    const int& GetRef();\n"
                                           "}\n";
        harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("ep_ref") + ".as", boundaryScript);

        // Global scope empty paren variable
        const std::string globCls = GenerateRandomSymbolName("GlobalCls");
        const std::string globVar = GenerateRandomSymbolName("g_instance");
        const std::string globalScript = "class " + globCls + " { int val = 10; }\n" + globCls + " " + globVar +
                                         "();\n"
                                         "void main() {\n"
                                         "    " +
                                         globVar +
                                         ".val = 42;\n"
                                         "}\n";
        harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("ep_glob") + ".as", globalScript);

        // Namespace scope empty paren variable
        const std::string nsName = GenerateRandomSymbolName("NS");
        const std::string nsScript = "class " + globCls +
                                     " { int val = 10; }\n"
                                     "namespace " +
                                     nsName +
                                     " {\n"
                                     "    " +
                                     globCls + " " + globVar +
                                     "();\n"
                                     "}\n"
                                     "void main() {\n"
                                     "    " +
                                     nsName + "::" + globVar +
                                     ".val = 42;\n"
                                     "}\n";
        harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("ep_ns") + ".as", nsScript);
    }

    TEST_CASE("EmptyParenAndTelemetryParity - Feature results and latency telemetry across LSP capabilities")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string cls = GenerateRandomSymbolName("TelemetryTarget");
        const std::string fn = GenerateRandomSymbolName("ExecuteAction");
        const std::string varName = GenerateRandomSymbolName("localVar");

        const std::string script = "class " + cls +
                                   " {\n"
                                   "    int " +
                                   varName +
                                   ";\n"
                                   "    void " +
                                   fn +
                                   "() {\n"
                                   "        int localVal = 10;\n"
                                   "        this." +
                                   varName +
                                   " = localVal;\n"
                                   "    }\n"
                                   "}\n";
        const std::string uri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("telemetry") + ".as");
        harness.AddVirtualDocument(uri, script);

        auto hover = harness.AuditHover(uri, 1, 9);
        CHECK(hover.has_value());

        auto comp = harness.AuditCompletion(uri, 4, 13);
        CHECK(!comp.empty());

        auto def = harness.AuditDefinition(uri, 4, 14);
        CHECK(def.has_value());

        auto refs = harness.AuditReferences(uri, 1, 9);
        CHECK(refs.has_value());

        lsp::Range range{{3, 8}, {3, 20}};
        auto actions = harness.AuditCodeActions(uri, range);
        CHECK(actions.has_value());

        auto tokens = harness.AuditSemanticTokens(uri);
        CHECK(!tokens.data.empty());
    }

} // TEST_SUITE
