#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/TestUtils.h"

#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <variant>

TEST_CASE("HoverHandler - Global predefined function shows its signature and documentation at a call site")
{
    angel_lsp::test::LspSemanticHarnessFixture fixture;
    const std::string functionName = angel_lsp::test::GenerateRandomSymbolName("EngineFunction");
    const std::string countName = angel_lsp::test::GenerateRandomSymbolName("count");
    const std::string scaleName = angel_lsp::test::GenerateRandomSymbolName("scale");
    const std::string signature = "void " + functionName + "(int " + countName + ", float " + scaleName + ")";
    const std::string documentation = "Applies the engine scaling factor to the requested count.";
    const auto stubPath =
        fixture.GetSandboxDir() / (angel_lsp::test::GenerateRandomSymbolName("engine") + ".as.predefined");
    {
        std::ofstream stub(stubPath, std::ios::binary);
        stub << "/**\n * " << documentation << "\n */\n" << signature << ";\n";
        stub.close();
        REQUIRE(stub.good());
    }
    fixture.LoadPredefinedStub(stubPath.string());

    const std::string scriptUri = fixture.SandboxUri(angel_lsp::test::GenerateRandomSymbolName("caller") + ".as");
    fixture.AddVirtualDocument(scriptUri, "void main()\n{\n    " + functionName + "(2, 1.5f);\n}\n");

    const auto hover = fixture.RequestHover(scriptUri, 2, 4);
    REQUIRE(hover.has_value());
    const auto* markdown = std::get_if<lsp::MarkupContent>(&hover->contents);
    REQUIRE(markdown != nullptr);
    CHECK(markdown->value.find(signature) != std::string::npos);
    CHECK(markdown->value.find(documentation) != std::string::npos);
}
