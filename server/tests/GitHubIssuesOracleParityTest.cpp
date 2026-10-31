#include "helpers/TestUtils.h"
#include "helpers/UnifiedFeatureAuditHarness.h"
#include <doctest/doctest.h>

using namespace angel_lsp::test;

TEST_SUITE("GitHubIssuesOracleParity")
{

    TEST_CASE("Issue #3 - Completion filters out private and protected members on external access")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string cls = GenerateRandomSymbolName("Cls");
        const std::string fn = GenerateRandomSymbolName("Fn");
        const std::string script = "class " + cls +
                                   " {\n"
                                   "    private int m_priv;\n"
                                   "    protected int m_prot;\n"
                                   "    public int m_pub;\n"
                                   "}\n"
                                   "void " +
                                   fn +
                                   "() {\n"
                                   "    " +
                                   cls +
                                   " obj;\n"
                                   "    obj.\n"
                                   "}\n";
        const std::string uri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("comp") + ".as");
        harness.AddVirtualDocument(uri, script);

        harness.AssertCompletionExcludes(uri, 7, 8, "m_priv");
        harness.AssertCompletionExcludes(uri, 7, 8, "m_prot");
        harness.AssertCompletionContains(uri, 7, 8, "m_pub", lsp::CompletionItemKind::Field);

        const std::string derivedCls = GenerateRandomSymbolName("Derived");
        const std::string derivedScript = "class " + cls +
                                          " {\n"
                                          "    private int m_priv;\n"
                                          "    protected int m_prot;\n"
                                          "    public int m_pub;\n"
                                          "}\n"
                                          "class " +
                                          derivedCls + " : " + cls +
                                          " {\n"
                                          "    void Inspect() {\n"
                                          "        this.\n"
                                          "    }\n"
                                          "}\n";
        const std::string derivedUri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("derived") + ".as");
        harness.AddVirtualDocument(derivedUri, derivedScript);

        harness.AssertCompletionExcludes(derivedUri, 7, 13, "m_priv");
        harness.AssertCompletionContains(derivedUri, 7, 13, "m_prot", lsp::CompletionItemKind::Field);
        harness.AssertCompletionContains(derivedUri, 7, 13, "m_pub", lsp::CompletionItemKind::Field);
    }

    TEST_CASE("Issue #4 - Overload ambiguity on initializer list arguments reports error")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string fnAmb = GenerateRandomSymbolName("AmbFunc");
        const std::string fnCaller = GenerateRandomSymbolName("Caller");
        const std::string badScript = "void " + fnAmb +
                                      "(int[] a) {}\n"
                                      "void " +
                                      fnAmb +
                                      "(float[] a) {}\n"
                                      "void " +
                                      fnCaller +
                                      "() {\n"
                                      "    " +
                                      fnAmb +
                                      "({1, 2});\n"
                                      "}\n";
        const std::string badUri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("amb_bad") + ".as");
        harness.AddVirtualDocument(badUri, badScript);

        const auto& diags = harness.GetDiagnostics(badUri);
        bool foundError = false;
        for (const auto& d : diags)
        {
            if (d.code == "as-err-call-ambiguous" && d.severity == angel_lsp::analysis::DiagnosticSeverity::Error)
            {
                foundError = true;
                break;
            }
        }
        CHECK(foundError);

        const std::string fnGood = GenerateRandomSymbolName("UnambFunc");
        const std::string goodScript = "void " + fnGood +
                                       "(int[] a) {}\n"
                                       "void " +
                                       fnCaller +
                                       "2() {\n"
                                       "    " +
                                       fnGood +
                                       "({1, 2});\n"
                                       "}\n";
        harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("amb_good") + ".as", goodScript);

        const std::string warnScript = "void " + fnAmb +
                                       "(int a, double b) {}\n"
                                       "void " +
                                       fnAmb +
                                       "(double a, int b) {}\n"
                                       "void " +
                                       fnCaller +
                                       "3() {\n"
                                       "    " +
                                       fnAmb +
                                       "(1, 2);\n"
                                       "}\n";
        const std::string warnUri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("amb_warn") + ".as");
        harness.AddVirtualDocument(warnUri, warnScript);
        const auto& warnDiags = harness.GetDiagnostics(warnUri);
        bool foundWarning = false;
        for (const auto& d : warnDiags)
        {
            if (d.code == "as-err-call-ambiguous" && d.severity == angel_lsp::analysis::DiagnosticSeverity::Warning)
            {
                foundWarning = true;
                break;
            }
        }
        CHECK(foundWarning);
    }

    TEST_CASE("Issue #7 - Const handle to non-const handle emits error and offers quick fix")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string cls = GenerateRandomSymbolName("Resource");
        const std::string fnGet = GenerateRandomSymbolName("GetResource");
        const std::string fnTest = GenerateRandomSymbolName("TestHandle");

        const std::string badScript = "class " + cls +
                                      " {}\n"
                                      "const " +
                                      cls + "@ " + fnGet +
                                      "() { return null; }\n"
                                      "void " +
                                      fnTest +
                                      "() {\n"
                                      "    " +
                                      cls + "@ res = " + fnGet +
                                      "();\n"
                                      "}\n";
        const std::string badUri = harness.VerifyBadPath("scripts/" + GenerateRandomSymbolName("const_h") + ".as",
                                                         badScript, "as-err-no-implicit-conversion");

        lsp::Range lineRange{{3, 4}, {3, 20}};
        auto actions = harness.RequestCodeActions(badUri, lineRange);
        REQUIRE(actions.has_value());
        bool foundFix = false;
        for (const auto& action : *actions)
        {
            if (action.title.find("const") != std::string::npos)
            {
                foundFix = true;
                break;
            }
        }
        CHECK(foundFix);

        const std::string badAssignScript = "class " + cls +
                                            " {}\n"
                                            "const " +
                                            cls + "@ " + fnGet +
                                            "() { return null; }\n"
                                            "void " +
                                            fnTest + "2(" + cls +
                                            "@ res) {\n"
                                            "    @res = " +
                                            fnGet +
                                            "();\n"
                                            "}\n";
        harness.VerifyBadPath("scripts/" + GenerateRandomSymbolName("const_assign") + ".as", badAssignScript,
                              "as-err-no-implicit-conversion");
    }

    TEST_CASE("Issue #7 - Null handle passed to reference parameter reports E119")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string cls = GenerateRandomSymbolName("ParamType");
        const std::string fnDo = GenerateRandomSymbolName("DoAction");
        const std::string fnCaller = GenerateRandomSymbolName("CallAction");

        const std::string badScript = "class " + cls +
                                      " {}\n"
                                      "void " +
                                      fnDo + "(const " + cls +
                                      " &in param) {}\n"
                                      "void " +
                                      fnCaller +
                                      "() {\n"
                                      "    " +
                                      fnDo +
                                      "(null);\n"
                                      "}\n";
        const std::string badUri = harness.VerifyBadPath("scripts/" + GenerateRandomSymbolName("null_ref") + ".as",
                                                         badScript, "as-err-call-no-matching-signature");

        const auto& diags = harness.GetDiagnostics(badUri);
        bool foundNullDesc = false;
        for (const auto& d : diags)
        {
            if (d.code == "as-err-call-no-matching-signature" && d.message.find("<null handle>") != std::string::npos)
            {
                foundNullDesc = true;
                break;
            }
        }
        CHECK(foundNullDesc);
    }

    TEST_CASE("Issue #8 - Mixin class allows internal private member access")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string mixinName = GenerateRandomSymbolName("Mix");
        const std::string fieldName = GenerateRandomSymbolName("m_state");
        const std::string goodScript = "mixin class " + mixinName +
                                       " {\n"
                                       "    private int " +
                                       fieldName +
                                       ";\n"
                                       "    void Reset() {\n"
                                       "        this." +
                                       fieldName +
                                       " = 10;\n"
                                       "        " +
                                       fieldName +
                                       " = 20;\n"
                                       "    }\n"
                                       "}\n";
        harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("mix_good") + ".as", goodScript);

        const std::string extCls = GenerateRandomSymbolName("External");
        const std::string badScript = goodScript + "class " + extCls +
                                      " {\n"
                                      "    void Access(" +
                                      mixinName +
                                      "@ m) {\n"
                                      "        int val = m." +
                                      fieldName +
                                      ";\n"
                                      "    }\n"
                                      "}\n";
        harness.VerifyBadPath("scripts/" + GenerateRandomSymbolName("mix_bad") + ".as", badScript,
                              "as-err-private-member-access");
    }

    TEST_CASE("Issue #9 - Overridden virtual properties inherit base setters")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string baseCls = GenerateRandomSymbolName("BaseEntity");
        const std::string derivCls = GenerateRandomSymbolName("DerivEntity");
        const std::string propName = GenerateRandomSymbolName("active");

        const std::string goodScript = "class " + baseCls +
                                       " {\n"
                                       "    bool get_" +
                                       propName +
                                       "() const { return false; }\n"
                                       "    void set_" +
                                       propName +
                                       "(bool val) {}\n"
                                       "}\n"
                                       "class " +
                                       derivCls + " : " + baseCls +
                                       " {\n"
                                       "    bool get_" +
                                       propName +
                                       "() const override { return true; }\n"
                                       "    void Update() {\n"
                                       "        this." +
                                       propName +
                                       " = true;\n"
                                       "        " +
                                       propName +
                                       " = false;\n"
                                       "    }\n"
                                       "}\n";
        harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("prop_good") + ".as", goodScript);

        const std::string roCls = GenerateRandomSymbolName("RoBase");
        const std::string badScript = "class " + roCls +
                                      " {\n"
                                      "    int get_readOnly() const { return 100; }\n"
                                      "    void TrySet() {\n"
                                      "        this.readOnly = 200;\n"
                                      "    }\n"
                                      "}\n";
        harness.VerifyBadPath("scripts/" + GenerateRandomSymbolName("prop_bad") + ".as", badScript,
                              "as-err-read-only-property");
    }

} // TEST_SUITE
