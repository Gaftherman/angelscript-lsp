#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "features/signature_help/SignatureHelpHandler.h"

using namespace angel_lsp;

TEST_CASE("SignatureHelp: Function Call Argument Active Parameter")
{
    std::string code = "void testFunc(int a, float b) {}\nvoid main() { testFunc(";
    auto doc = test::CreateTestDocument("file:///test.as", code);
    analysis::SymbolTable table;
    test::PopulateTestSymbolTable(doc, table);

    lsp::requests::TextDocument_SignatureHelp::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse("file:///test.as");
    req.position.line = 1;
    req.position.character = 23;

    auto result = features::ProcessSignatureHelp(req, doc, table, nullptr);
    CHECK_FALSE(result.isNull());
}
