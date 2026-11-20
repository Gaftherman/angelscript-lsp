#include "analysis/diagnostics/DiagnosticCodes.h"
#include "helpers/TestUtils.h"
#include "helpers/UnifiedFeatureAuditHarness.h"

#include <algorithm>
#include <doctest/doctest.h>

using namespace angel_lsp::test;

TEST_SUITE("oracle/GitHubIssuesOracleParity")
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

    TEST_CASE("Issue #1 - A weakref built from this inside a derived class constructor is accepted")
    {
        UnifiedFeatureAuditHarness harness;
        harness.LoadPredefinedStub("sdk-addons.as.predefined");
        const std::string ns = GenerateRandomSymbolName("Space");
        const std::string base = GenerateRandomSymbolName("Base");
        const std::string derived = GenerateRandomSymbolName("Derived");
        const std::string list = GenerateRandomSymbolName("refs");
        const std::string script = "namespace " + ns + "\n{\n    class " + base + " {}\n    array<weakref<" + base +
                                   ">> " + list + ";\n}\n\nclass " + derived + " : " + ns + "::" + base + "\n{\n    " +
                                   derived + "()\n    {\n        weakref<" + ns + "::" + base +
                                   "> ref(this);\n        " + ns + "::" + list + ".insertLast( ref );\n    }\n}\n";
        harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("weakref_ctor") + ".as", script);
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
        const std::string ambUri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("amb_call") + ".as");
        harness.AddVirtualDocument(ambUri, warnScript);
        const auto& ambDiags = harness.GetDiagnostics(ambUri);
        bool foundAmbError = false;
        for (const auto& d : ambDiags)
        {
            if (d.code == "as-err-call-ambiguous" && d.severity == angel_lsp::analysis::DiagnosticSeverity::Error)
            {
                foundAmbError = true;
                break;
            }
        }
        CHECK(foundAmbError);
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

    TEST_CASE("Issue #15 - Instance getter resolves when a namespace type shares its name")
    {
        const std::string owner = GenerateRandomSymbolName("Owner");
        const std::string inner = GenerateRandomSymbolName("Inner");
        const std::string other = GenerateRandomSymbolName("Other");

        const auto build = [&](bool withGetter, const std::string& receiverStmt)
        {
            const std::string getter =
                withGetter ? "    " + owner + "::" + inner + "@ get_" + inner + "() property { return null; }\n" : "";
            return "namespace " + owner + " { class " + inner + " {} }\nclass " + other + " {\n    " + owner +
                   "::" + inner + "@ get_" + inner + "() property { return null; }\n}\nclass " + owner + " {\n" +
                   getter + "    void Run() {\n        " + receiverStmt + "\n    }\n}\n";
        };
        const std::string access = "auto@ v = this." + inner + ";";

        const auto memberNotFound = [](const std::vector<angel_lsp::analysis::Diagnostic>& diags)
        {
            return std::count_if(diags.begin(), diags.end(), [](const angel_lsp::analysis::Diagnostic& d)
                                 { return d.code == angel_lsp::diagnostics::codes::MemberNotFound; });
        };

        SUBCASE("the getter binds and the qualified type stays resolvable")
        {
            UnifiedFeatureAuditHarness harness;
            const std::string uri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("getter") + ".as");
            harness.AddVirtualDocument(uri, build(true, access));
            CHECK(memberNotFound(harness.GetDiagnostics(uri)) == 0);
        }

        SUBCASE("without the getter the member is genuinely missing, even with an unrelated same-name getter")
        {
            UnifiedFeatureAuditHarness harness;
            const std::string uri = harness.SandboxUri("scripts/" + GenerateRandomSymbolName("nogetter") + ".as");
            harness.AddVirtualDocument(uri, build(false, access));
            const auto& diags = harness.GetDiagnostics(uri);
            REQUIRE(memberNotFound(diags) == 1);
            const auto hit = std::find_if(diags.begin(), diags.end(), [](const angel_lsp::analysis::Diagnostic& d)
                                          { return d.code == angel_lsp::diagnostics::codes::MemberNotFound; });
            CHECK(hit->range.start.line == 6);
            CHECK(hit->range.start.character == 8 + std::string("auto@ v = this.").size());
        }
    }

    TEST_CASE("Property accessors - Names around the old buffer sizes resolve like any other")
    {
        UnifiedFeatureAuditHarness harness;
        // 124 and 125 straddle the 128 byte buffer once `get_` is added; 508 and 509 straddle the 512 byte one.
        for (const size_t length :
             {size_t(20), size_t(124), size_t(125), size_t(130), size_t(508), size_t(509), size_t(600)})
        {
            const std::string cls = GenerateRandomSymbolName("Holder");
            const std::string fn = GenerateRandomSymbolName("use");
            const std::string prop = "p" + std::string(length - 1, 'x');
            const std::string script = "class " + cls + "\n{\n    int get_" + prop +
                                       "() property { return 1; }\n    void set_" + prop +
                                       "(int v) property { }\n}\nvoid " + fn + "(" + cls + "@ c)\n{\n    c." + prop +
                                       " = c." + prop + " + 1;\n}\n";
            INFO("property name length " << length);
            harness.VerifyGoodPath("scripts/" + GenerateRandomSymbolName("long") + ".as", script);
        }
    }

    TEST_CASE("Property accessors - A missing accessor of a long name is still reported")
    {
        UnifiedFeatureAuditHarness harness;
        const std::string cls = GenerateRandomSymbolName("Holder");
        const std::string fn = GenerateRandomSymbolName("use");
        const std::string prop = "p" + std::string(199, 'x');
        const std::string script = "class " + cls + "\n{\n    int get_" + prop + "() property { return 1; }\n}\nvoid " +
                                   fn + "(" + cls + "@ c)\n{\n    c." + prop + " = 2;\n}\n";
        // The setter is absent, so the write must fail whatever the length of the name.
        harness.VerifyBadPath("scripts/" + GenerateRandomSymbolName("readonly") + ".as", script,
                              "as-err-read-only-property");
    }

    TEST_CASE("Property accessors - Reads and writes bind to the accessors that exist")
    {
        UnifiedFeatureAuditHarness harness;
        const auto path = [](const char* prefix) { return "scripts/" + GenerateRandomSymbolName(prefix) + ".as"; };

        // Documents of one workspace share a global scope, so each script declares its own class name.
        const auto script = [](const std::string& declarations, const std::string& body)
        {
            const std::string name = GenerateRandomSymbolName("Cls");
            std::string text = declarations + "void use(" + name + "@ c) { " + body + " }\n";
            for (size_t at = text.find("@@"); at != std::string::npos; at = text.find("@@"))
            {
                text.replace(at, 2, name);
            }
            return text;
        };
        const std::string readWrite =
            "class @@ { int v; int get_p() property { return v; } void set_p(int x) property { v = x; } }\n";
        const std::string readOnly = "class @@ { int v; int get_p() property { return v; } }\n";
        const std::string writeOnly = "class @@ { void set_p(int x) property { } }\n";

        SUBCASE("a property with both accessors can be read, written and compounded")
        {
            harness.VerifyGoodPath(path("rw"), script(readWrite, "c.p = 1; c.p = c.p + 1; c.p += 2;"));
        }
        SUBCASE("a derived getter keeps the setter of its base")
        {
            const std::string name = GenerateRandomSymbolName("Cls");
            harness.VerifyGoodPath(path("derived"),
                                   "class " + name +
                                       " { int v; int get_p() property { return v; } void set_p(int x) "
                                       "property { v = x; } }\nclass D" +
                                       name + " : " + name + " { int get_p() property { return 2; } }\nvoid use(D" +
                                       name + "@ d) { d.p = 3; }\n");
        }
        SUBCASE("a write of the wrong type is rejected")
        {
            harness.VerifyBadPath(path("badtype"), script(readWrite, "c.p = \"x\";"), "as-err-no-implicit-conversion");
        }
        SUBCASE("a read-only property rejects plain and compound writes")
        {
            harness.VerifyBadPath(path("ro1"), script(readOnly, "c.p = 1;"), "as-err-read-only-property");
            harness.VerifyBadPath(path("ro2"), script(readOnly, "c.p += 1;"), "as-err-read-only-property");
            harness.VerifyBadPath(path("ro3"), script(readOnly, "c.p -= 2;"), "as-err-read-only-property");
        }
        SUBCASE("a read-only property rejects increment")
        {
            harness.VerifyBadPath(path("ro4"), script(readOnly, "c.p++;"), "as-err-inc-dec-on-virtual-prop");
        }
        SUBCASE("a write-only property rejects every kind of read")
        {
            harness.VerifyBadPath(path("wo1"), script(writeOnly, "int y = c.p;"), "as-err-write-only-property");
            harness.VerifyBadPath(path("wo2"), script(writeOnly, "int y = 0; y = c.p;"), "as-err-write-only-property");
        }
    }

    TEST_CASE("Issue #6 - An object reference parameter accepts what the compiler accepts and rejects null")
    {
        UnifiedFeatureAuditHarness harness;
        const auto path = [](const char* prefix) { return "scripts/" + GenerateRandomSymbolName(prefix) + ".as"; };

        // Every script gets its own class and function names: documents of one workspace share a global scope.
        struct Names
        {
            std::string a = GenerateRandomSymbolName("Alpha");
            std::string b = GenerateRandomSymbolName("Beta");
            std::string take = GenerateRandomSymbolName("take");
            std::string Decl() const
            {
                return "class " + a + " { }\nvoid " + take + "(" + a + "&in p) { }\n";
            }
        };

        // Accepted by the compiler: a value, a handle, and a const handle all bind to a copying reference.
        const Names value;
        harness.VerifyGoodPath(path("val"), value.Decl() + "void main1()\n{\n    " + value.a + " v;\n    " +
                                                value.take + "(v);\n}\n");
        const Names handle;
        harness.VerifyGoodPath(path("hnd"), handle.Decl() + "void main2()\n{\n    " + handle.a + "@ h = " + handle.a +
                                                "();\n    " + handle.take + "(h);\n}\n");
        const Names constHandle;
        harness.VerifyGoodPath(path("chnd"), constHandle.Decl() + "void main3()\n{\n    const " + constHandle.a +
                                                 "@ h = " + constHandle.a + "();\n    " + constHandle.take +
                                                 "(h);\n}\n");

        // A constructor taking the argument converts it, whether the argument is a value or a handle.
        const Names ctorInt;
        harness.VerifyGoodPath(path("ctorint"), "class " + ctorInt.a + " { " + ctorInt.a + "(int x) { } }\nvoid " +
                                                    ctorInt.take + "(" + ctorInt.a +
                                                    "&in p) { }\nvoid main4()\n{\n    " + ctorInt.take + "(1);\n}\n");
        const Names ctorHandle;
        harness.VerifyGoodPath(path("ctorhandle"),
                               "class " + ctorHandle.b + " { }\nclass " + ctorHandle.a + " { " + ctorHandle.a + "(" +
                                   ctorHandle.b + "@ x) { } }\nvoid " + ctorHandle.take + "(" + ctorHandle.a +
                                   "&in p) { }\nvoid main5()\n{\n    " + ctorHandle.b + "@ h = " + ctorHandle.b +
                                   "();\n    " + ctorHandle.take + "(h);\n}\n");

        // A handle parameter takes null; an object reference parameter does not, whatever its direction.
        const Names nullHandle;
        harness.VerifyGoodPath(path("nullhandle"), "class " + nullHandle.a + " { }\nvoid " + nullHandle.take + "(" +
                                                       nullHandle.a + "@ p) { }\nvoid main6()\n{\n    " +
                                                       nullHandle.take + "(null);\n}\n");

        SUBCASE("null to an object reference parameter")
        {
            const Names n;
            harness.VerifyBadPath(path("nullref"), n.Decl() + "void main7()\n{\n    " + n.take + "(null);\n}\n",
                                  "as-err-call-no-matching-signature");
        }
        SUBCASE("null to an out reference parameter")
        {
            const Names n;
            harness.VerifyBadPath(path("nullout"),
                                  "class " + n.a + " { }\nvoid " + n.take + "(" + n.a +
                                      "&out p) { }\nvoid main8()\n{\n    " + n.take + "(null);\n}\n",
                                  "as-err-call-no-matching-signature");
        }
    }
} // TEST_SUITE
