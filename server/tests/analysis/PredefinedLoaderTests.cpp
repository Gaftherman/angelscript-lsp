#include <doctest/doctest.h>
#include "analysis/PredefinedLoader.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "document/Document.h"

using namespace analysis;

TEST_SUITE("PredefinedLoader")
{
    TEST_CASE("PL1.1: LoadFromSource populates SymbolTable with string and array types")
    {
        SymbolTable table;
        bool ok = PredefinedLoader::LoadFromSource("class string { uint Length() const; string opAdd(const string&in) const; } class array<T> { uint length() const; }", table, "string", "array");
        CHECK(ok == true);

        const Symbol *stringSym = table.FindGlobalByName("string");
        CHECK(stringSym != nullptr);
        if (stringSym)
        {
            CHECK(stringSym->kind == SymbolKind::Class);
        }

        const Symbol *arraySym = table.FindGlobalByName("array<T>");
        if (!arraySym)
        {
            arraySym = table.FindGlobalByName("array");
        }
        CHECK(arraySym != nullptr);
    }

    TEST_CASE("PL1.1b: LoadFromSource works with custom type name 'String'")
    {
        SymbolTable table;
        bool ok = PredefinedLoader::LoadFromSource("class String { uint Length() const; }", table, "String", "array");
        CHECK(ok == true);

        const Symbol *stringSym = table.FindGlobalByName("String");
        CHECK(stringSym != nullptr);
    }

    TEST_CASE("PL1.2: SymbolTable stores array syntax template type")
    {
        SymbolTable table;
        bool ok = PredefinedLoader::LoadFromSource("class array<T> { uint length() const; }", table, "string", "array");
        CHECK(ok == true);

        const Symbol *arraySym = table.FindGlobalByName("array<T>");
        if (!arraySym)
        {
            arraySym = table.FindGlobalByName("array");
        }
        CHECK(arraySym != nullptr);
    }

    TEST_CASE("PL1.4: Full as.predefined script loads into SymbolTable cleanly")
    {
        const char *PREDEFINED = R"(
typedef uint32 size_t;

funcdef bool less(const ?&in a, const ?&in b);
class array<T>
{
    uint length() const;
    void resize(uint length);
}

class string
{
    uint Length() const;
}

class CCustomClass
{
    void ThisIsACCustomClassMemberFunction();
    int thisIsACCustomClassVariable;
}

namespace ThisIsANamespace
{
    class NameSpaceClass
    {
        void ThisIsANameSpaceClassFunction();
        float thisIsANameSpaceClassVariable;
    }
}
        )";

        SymbolTable table;
        bool loaded = PredefinedLoader::LoadFromSource(PREDEFINED, table, "string", "array");
        CHECK(loaded == true);

        CHECK(table.FindGlobalByName("size_t") != nullptr);
        CHECK(table.FindGlobalByName("less") != nullptr);
        CHECK(table.FindGlobalByName("string") != nullptr);
        CHECK(table.FindGlobalByName("CCustomClass") != nullptr);
        CHECK(table.FindGlobalByName("ThisIsANamespace") != nullptr);
    }

    TEST_CASE("PL1.5: SymbolCollector extracts all types from as.predefined source")
    {
        const char *src = R"(
            class string { uint Length() const; }
            class array<T> { uint length() const; }
            namespace Engine { class RigidBody { float mass; } }
        )";

        Document doc("file:///as.predefined", src);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        CHECK(table.FindGlobalByName("string") != nullptr);
        bool foundArray = table.FindGlobalByName("array<T>") != nullptr || table.FindGlobalByName("array") != nullptr;
        CHECK(foundArray == true);
        CHECK(table.FindGlobalByName("Engine") != nullptr);

        auto *ns = table.FindGlobalByName("Engine");
        if (ns)
        {
            CHECK(ns->children.size() > 0);
        }
    }
}
