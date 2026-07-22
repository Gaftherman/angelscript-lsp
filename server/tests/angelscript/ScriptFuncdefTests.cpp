#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"
#include "document/Document.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolResolver.h"

TEST_SUITE("Script - Funcdefs")
{
    TEST_CASE("Funcdef declaration and delegate assignment work")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            funcdef void Callback(int);

            void MyCallback(int value) {}

            void Main()
            {
                Callback@ cb = MyCallback;
                if (cb !is null)
                {
                    cb(42);
                }
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Calling delegate with incorrect signature produces error")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            funcdef void Callback(int);

            void MyCallback(int value) {}

            void Main()
            {
                Callback@ cb = MyCallback;
                if (cb !is null)
                {
                    cb("hello"); // Error: string passed instead of int
                }
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.HasError());
    }

    TEST_CASE("Funcdef symbol is collected in SymbolTable and resolves")
    {
        std::string code = R"(
            funcdef void CallbackFunc();

            void Main()
            {
                CallbackFunc@ callback = function() {
                };
            }
        )";

        Document doc("file:///funcdef_test.as", code);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);

        const analysis::Symbol *sym = table.FindByNameDeep("CallbackFunc");
        REQUIRE(sym != nullptr);
        CHECK(sym->kind == analysis::SymbolKind::Funcdef);
        CHECK(sym->name == "CallbackFunc");

        // Resolve 'function' keyword inside lambda -> should point to CallbackFunc
        // 'function' is on line 5 (0-indexed), col 43
        const analysis::Symbol *lambdaResolved = analysis::SymbolResolver::ResolveAt(doc, table, 5, 43);
        REQUIRE(lambdaResolved != nullptr);
        CHECK(lambdaResolved->name == "CallbackFunc");
        CHECK(lambdaResolved->kind == analysis::SymbolKind::Funcdef);
    }
}
