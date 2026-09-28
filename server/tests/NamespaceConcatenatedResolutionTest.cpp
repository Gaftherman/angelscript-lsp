#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/TestUtils.h"
#include "spdlog/fmt/fmt.h"
#include <doctest/doctest.h>
#include <string>

using namespace angel_lsp::test;

TEST_SUITE("NamespaceConcatenatedResolution")
{
    TEST_CASE("Unqualified call to parent namespace function from compound namespace")
    {
        LspSemanticHarnessFixture fixture;
        const std::string nsA = GenerateRandomSymbolName("NsA");
        const std::string nsB = GenerateRandomSymbolName("NsB");
        const std::string fnFoo = GenerateRandomSymbolName("foo");
        const std::string clsOOF = GenerateRandomSymbolName("OOF");
        const std::string method = GenerateRandomSymbolName("method");

        const std::string script = fmt::format(
            "namespace {0} {{\n"
            "    void {2}() {{}}\n"
            "}}\n"
            "\n"
            "namespace {0}::{1} {{\n"
            "    class {3} {{\n"
            "        void {4}() {{\n"
            "            {2}();\n"
            "        }}\n"
            "    }}\n"
            "}}\n",
            nsA, nsB, fnFoo, clsOOF, method);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated unqualified call in compound namespace");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);

        // Assert hover on foo() inside method() shows the parent namespace function signature
        const uint32_t callLine = 7;
        const uint32_t callCol = 13;
        fixture.AssertHoverSignature(docUri, callLine, callCol, fmt::format("void {0}::{1}()", nsA, fnFoo));
    }

    TEST_CASE("Shadowing in compound namespace: inner function shadows outer namespace function")
    {
        LspSemanticHarnessFixture fixture;
        const std::string nsA = GenerateRandomSymbolName("NsA");
        const std::string nsB = GenerateRandomSymbolName("NsB");
        const std::string fnFoo = GenerateRandomSymbolName("foo");
        const std::string clsOOF = GenerateRandomSymbolName("OOF");

        const std::string script = fmt::format(
            "namespace {0} {{\n"
            "    void {2}() {{}}\n"
            "}}\n"
            "\n"
            "namespace {0}::{1} {{\n"
            "    int {2}() {{ return 42; }}\n"
            "    class {3} {{\n"
            "        void run() {{\n"
            "            int val = {2}();\n"
            "            if (val > 0) {{}}\n"
            "        }}\n"
            "    }}\n"
            "}}\n",
            nsA, nsB, fnFoo, clsOOF);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated shadowing in compound namespace");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);

        // Hover on foo() inside run() should resolve to int NsA::NsB::foo(), not void NsA::foo()
        const uint32_t callLine = 8;
        const uint32_t callCol = 23;
        fixture.AssertHoverSignature(docUri, callLine, callCol, fmt::format("int {0}::{1}::{2}()", nsA, nsB, fnFoo));
    }

    TEST_CASE("Multi-level compound namespace resolution: A::B::C accesses symbols from A and A::B")
    {
        LspSemanticHarnessFixture fixture;
        const std::string nsA = GenerateRandomSymbolName("NsA");
        const std::string nsB = GenerateRandomSymbolName("NsB");
        const std::string nsC = GenerateRandomSymbolName("NsC");
        const std::string fnInA = GenerateRandomSymbolName("fnInA");
        const std::string fnInB = GenerateRandomSymbolName("fnInB");
        const std::string clsOOF = GenerateRandomSymbolName("OOF");

        const std::string script = fmt::format(
            "namespace {0} {{\n"
            "    void {3}() {{}}\n"
            "}}\n"
            "namespace {0}::{1} {{\n"
            "    void {4}() {{}}\n"
            "}}\n"
            "namespace {0}::{1}::{2} {{\n"
            "    class {5} {{\n"
            "        void method() {{\n"
            "            {3}();\n"
            "            {4}();\n"
            "        }}\n"
            "    }}\n"
            "}}\n",
            nsA, nsB, nsC, fnInA, fnInB, clsOOF);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated multi-level compound namespace");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Non-function symbols (enums, classes) from parent namespace in compound namespace")
    {
        LspSemanticHarnessFixture fixture;
        const std::string nsA = GenerateRandomSymbolName("NsA");
        const std::string nsB = GenerateRandomSymbolName("NsB");
        const std::string enumType = GenerateRandomSymbolName("MyEnum");
        const std::string enumVal = GenerateRandomSymbolName("VAL_ONE");
        const std::string parentClass = GenerateRandomSymbolName("BaseConfig");

        const std::string script = fmt::format(
            "namespace {0} {{\n"
            "    enum {2} {{\n"
            "        {3} = 1\n"
            "    }}\n"
            "    class {4} {{\n"
            "        int id = 0;\n"
            "    }}\n"
            "}}\n"
            "\n"
            "namespace {0}::{1} {{\n"
            "    class Consumer {{\n"
            "        {4} config;\n"
            "        {2} state = {3};\n"
            "        void execute() {{\n"
            "            if (state == {3}) {{\n"
            "                config.id = 1;\n"
            "            }}\n"
            "        }}\n"
            "    }}\n"
            "}}\n",
            nsA, nsB, enumType, enumVal, parentClass);

        std::string oracleError;
        const bool oracleAccepted = fixture.VerifyWithNativeOracle(script, oracleError);
        if (oracleAccepted)
        {
            MESSAGE("asharness.exe validated enum and class access in compound namespace");
        }

        const std::string docUri = fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("doc")));
        fixture.AddVirtualDocument(docUri, script);
        fixture.AssertNoDiagnostics(docUri);
    }

    TEST_CASE("Unqualified call across included module files in compound namespace")
    {
        LspSemanticHarnessFixture fixture;
        const std::string nsA = GenerateRandomSymbolName("NsA");
        const std::string nsB = GenerateRandomSymbolName("NsB");
        const std::string fnFoo = GenerateRandomSymbolName("foo");
        const std::string clsOOF = GenerateRandomSymbolName("OOF");

        const std::string commonScript = fmt::format(
            "namespace {0} {{\n"
            "    void {1}() {{}}\n"
            "}}\n",
            nsA, fnFoo);

        const std::string weaponScript = fmt::format(
            "namespace {0}::{1} {{\n"
            "    class {2} {{\n"
            "        void method() {{\n"
            "            {3}();\n"
            "        }}\n"
            "    }}\n"
            "}}\n",
            nsA, nsB, clsOOF, fnFoo);

        const std::string commonUri =
            fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("common")));
        const std::string weaponUri =
            fixture.SandboxUri(fmt::format("scripts/{0}.as", GenerateRandomSymbolName("weapon")));

        fixture.AddVirtualDocument(commonUri, commonScript);
        fixture.AddVirtualDocument(weaponUri, weaponScript);
        fixture.AssertNoDiagnostics(weaponUri);
    }
}
