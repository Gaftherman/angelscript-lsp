#include <doctest/doctest.h>
#include "features/hover/HoverHandler.h"
#include "analysis/SymbolTable.h"
#include "features/hover/HoverHandler.h"
#include "utils/DoxygenParser.h"
#include "i18n/LspStrings.h"
#include "analysis/SymbolCollector.h"
#include "document/Document.h"
#include <tree_sitter/api.h>

extern "C" TSLanguage *tree_sitter_doxygen();

using namespace analysis;

TEST_CASE("Hover - Template Substitution")
{
    const char *source_code = "/**\n * @brief A first-in, first-out (FIFO) queue\n * @tparam T The type of the elements\n */";
    std::string test_md = angel_lsp::utils::FormatDoxygenToMarkdown(source_code, i18n::Locale::EN);
    
    std::string code = "class array<T> {\n"
                       "  /** @brief Inserts at the end */\n"
                       "  void insertLast(const T&in value) {}\n"
                       "}\n"
                       "void main() {\n"
                       "  array<Test@> listaTiempos;\n"
                       "  listaTiempos.insertLast(null);\n"
                       "}";
    Document doc("file:///test.as", code);

    SymbolTable table;
    SymbolCollector::CollectGlobals(doc, table);
    
    // Let's just manually add the local symbol for test simplicity
    auto localArray = std::make_shared<Symbol>();
    localArray->name = "listaTiempos";
    localArray->typeInfo = "array<Test@>";
    localArray->kind = SymbolKind::Variable;
    localArray->fullRange.start.line = 5;
    localArray->fullRange.end.line = 5;
    table.AddLocal(localArray);

    // Hover over insertLast inside main()
    lsp::requests::TextDocument_Hover::Result result;
    lsp::requests::TextDocument_Hover::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse("file:///test.as");
    req.position.line = 6;
    req.position.character = 17; // over "insertLast"

    angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::ES, nullptr);
    
    REQUIRE(!result.isNull());
    std::string markup_value;
        if (std::holds_alternative<lsp::Array<lsp::MarkedString>>((*result).contents)) {
            auto markedStrings = std::get<lsp::Array<lsp::MarkedString>>((*result).contents);
            for (const auto& ms : markedStrings) {
                if (std::holds_alternative<lsp::String>(ms)) {
                    markup_value += std::get<lsp::String>(ms);
                } else if (std::holds_alternative<lsp::MarkedString_Language_Value>(ms)) {
                    markup_value += std::get<lsp::MarkedString_Language_Value>(ms).value;
                }
            }
        } else if (std::holds_alternative<lsp::MarkupContent>((*result).contents)) {
            markup_value = std::get<lsp::MarkupContent>((*result).contents).value;
        }
        
        struct DummyMarkup { std::string value; } markup = { markup_value };
    std::string md = markup.value;
    
    // Output the markdown
    printf("MARKDOWN OUT:\n%s\n", md.c_str());
    
    // Check substitution
    CHECK( md.find("void array::insertLast(const Test@&in value)") != std::string::npos );
}
