#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "features/completion/CompletionHandler.h"

using namespace angel_lsp;

TEST_CASE("Completion: Basic Keyword & Global Completion")
{
    std::string code = "void main() { int testVar = 10; tes";
    auto doc = test::CreateTestDocument("file:///test.as", code);
    analysis::SymbolTable table;
    test::PopulateTestSymbolTable(doc, table);

    lsp::requests::TextDocument_Completion::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse("file:///test.as");
    req.position.line = 0;
    req.position.character = 34;

    auto result = features::ProcessCompletion(req, doc, table);
    CHECK_FALSE(result.isNull());
}
