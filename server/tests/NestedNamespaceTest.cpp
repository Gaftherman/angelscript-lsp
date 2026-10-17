#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/TestUtils.h"
#include "spdlog/fmt/fmt.h"
#include <doctest/doctest.h>
#include <string>

using namespace angel_lsp::test;

TEST_SUITE("NestedNamespaceTest")
{
    TEST_CASE("Hierarchical block namespaces A { B { C { ... } } } resolve parent and ancestor symbols")
    {
        LspSemanticHarnessFixture fixture;
        const std::string nsA = GenerateRandomSymbolName("NsA");
        const std::string nsB = GenerateRandomSymbolName("NsB");
        const std::string nsC = GenerateRandomSymbolName("NsC");
        const std::string varA = GenerateRandomSymbolName("g_valA");
        const std::string fnA = GenerateRandomSymbolName("FuncA");
        const std::string fnB = GenerateRandomSymbolName("FuncB");
        const std::string fnC = GenerateRandomSymbolName("FuncC");

        const std::string script = fmt::format("namespace {0} {{\n"
                                               "    int {3} = 10;\n"
                                               "    void {4}() {{}}\n"
                                               "    namespace {1} {{\n"
                                               "        void {5}() {{}}\n"
                                               "        namespace {2} {{\n"
                                               "            void {6}() {{\n"
                                               "                {4}();\n"
                                               "                {5}();\n"
                                               "                {3} = 20;\n"
                                               "            }}\n"
                                               "        }}\n"
                                               "    }}\n"
                                               "}}\n",
                                               nsA, nsB, nsC, varA, fnA, fnB, fnC);

        if (fixture.HasOracleBinary())
        {
            std::string oracleError;
            const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
            CHECK(oracleAccepted);
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);

        // Verify Definition on FuncA() call inside FuncC (line 7, col 17) -> line 2
        fixture.AssertDefinitionTarget(docUri, 7, 17, "", 2);

        // Verify Definition on FuncB() call inside FuncC (line 8, col 17) -> line 4
        fixture.AssertDefinitionTarget(docUri, 8, 17, "", 4);
    }

    TEST_CASE("Compound namespace A::B::C accesses ancestor symbols and allows qualified external calls")
    {
        LspSemanticHarnessFixture fixture;
        const std::string nsA = GenerateRandomSymbolName("Outer");
        const std::string nsB = GenerateRandomSymbolName("Middle");
        const std::string nsC = GenerateRandomSymbolName("Inner");
        const std::string fnA = GenerateRandomSymbolName("FnOuter");
        const std::string fnB = GenerateRandomSymbolName("FnMiddle");
        const std::string fnC = GenerateRandomSymbolName("FnInner");
        const std::string entryFn = GenerateRandomSymbolName("MainEntry");

        const std::string script = fmt::format("namespace {0} {{\n"
                                               "    void {3}() {{}}\n"
                                               "}}\n"
                                               "namespace {0}::{1} {{\n"
                                               "    void {4}() {{}}\n"
                                               "}}\n"
                                               "namespace {0}::{1}::{2} {{\n"
                                               "    void {5}() {{\n"
                                               "        {3}();\n"
                                               "        {4}();\n"
                                               "    }}\n"
                                               "}}\n"
                                               "void {6}() {{\n"
                                               "    {0}::{1}::{2}::{5}();\n"
                                               "}}\n",
                                               nsA, nsB, nsC, fnA, fnB, fnC, entryFn);

        if (fixture.HasOracleBinary())
        {
            std::string oracleError;
            const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
            CHECK(oracleAccepted);
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);

        // Definition check on A::B::C::FnInner() in MainEntry -> line 7
        const std::string prefix1 = fmt::format("    {0}::{1}::{2}::", nsA, nsB, nsC);
        const uint32_t callCol1 = static_cast<uint32_t>(prefix1.size());
        fixture.AssertDefinitionTarget(docUri, 13, callCol1, "", 7);
    }

    TEST_CASE("Sibling compound namespaces under same parent resolve relatively")
    {
        LspSemanticHarnessFixture fixture;
        const std::string rootNs = GenerateRandomSymbolName("Root");
        const std::string branchNs = GenerateRandomSymbolName("Branch");
        const std::string sibA = GenerateRandomSymbolName("WorkerA");
        const std::string sibB = GenerateRandomSymbolName("WorkerB");
        const std::string taskA = GenerateRandomSymbolName("DoTaskA");
        const std::string taskB = GenerateRandomSymbolName("DoTaskB");

        const std::string script = fmt::format("namespace {0}::{1}::{2} {{\n"
                                               "    void {4}() {{}}\n"
                                               "}}\n"
                                               "namespace {0}::{1}::{3} {{\n"
                                               "    void {5}() {{\n"
                                               "        {2}::{4}();\n"
                                               "    }}\n"
                                               "}}\n",
                                               rootNs, branchNs, sibA, sibB, taskA, taskB);

        if (fixture.HasOracleBinary())
        {
            std::string oracleError;
            const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
            CHECK(oracleAccepted);
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);

        // Definition check on WorkerA::DoTaskA inside WorkerB -> line 1
        const std::string prefix2 = fmt::format("        {0}::", sibA);
        const uint32_t callCol2 = static_cast<uint32_t>(prefix2.size());
        fixture.AssertDefinitionTarget(docUri, 5, callCol2, "", 1);
    }
}
