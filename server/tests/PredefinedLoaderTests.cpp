#include <doctest/doctest.h>
#include <angelscript.h>
#include "analysis/PredefinedLoader.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "document/Document.h"

using namespace analysis;

void MessageCallback(const asSMessageInfo *msg, void *param) {
    printf("AS MESSAGE: %s (Row: %d)\n", msg->message, msg->row);
}

TEST_SUITE("PredefinedLoader")
{
    TEST_CASE("PL1.1: DummyStringFactory - engine accepts string literal with dummy type")
    {
        asIScriptEngine* engine = asCreateScriptEngine();
        engine->SetMessageCallback(asFUNCTION(MessageCallback), 0, asCALL_CDECL);
        REQUIRE(engine != nullptr);

        SymbolTable table;
        // stringType="string", arrayType="array"
        bool ok = PredefinedLoader::LoadFromSource("class string { uint Length() const; string opAdd(const string&in) const; } class array<T> { uint length() const; }", engine, table, "string", "array");
        CHECK(ok == true);

        // Verify string type exists
        int typeId = engine->GetTypeIdByDecl("string");
        CHECK(typeId >= 0);

        // Compilar un script que use un literal string — no debe dar error
        asIScriptModule* mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
        mod->AddScriptSection("test", R"( void Main() { string s = "hola"; } )");
        int r = mod->Build();
        CHECK(r >= 0); // 0 = éxito

        engine->ShutDownAndRelease();
    }

    TEST_CASE("PL1.1b: DummyStringFactory - works with custom type name 'String'")
    {
        asIScriptEngine* engine = asCreateScriptEngine();
        engine->SetMessageCallback(asFUNCTION(MessageCallback), 0, asCALL_CDECL);
        REQUIRE(engine != nullptr);

        SymbolTable table;
        bool ok = PredefinedLoader::LoadFromSource("class String { uint Length() const; }", engine, table, "String", "array");
        CHECK(ok == true);

        asIScriptModule* mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
        mod->AddScriptSection("test", R"( void Main() { String s = "hello"; } )");
        int r = mod->Build();
        CHECK(r >= 0);

        engine->ShutDownAndRelease();
    }

    TEST_CASE("PL1.2: RegisterDefaultArrayType - engine accepts array syntax with dummy type")
    {
        asIScriptEngine* engine = asCreateScriptEngine();
        engine->SetMessageCallback(asFUNCTION(MessageCallback), 0, asCALL_CDECL);
        REQUIRE(engine != nullptr);

        SymbolTable table;
        bool ok = PredefinedLoader::LoadFromSource("class array<T> { uint length() const; }", engine, table, "string", "array");
        CHECK(ok == true);

        // Verificar que int[] es sintaxis válida
        asIScriptModule* mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
        mod->AddScriptSection("test", R"( void Main() { array<int>@ arr; } )");
        int r = mod->Build();
        CHECK(r >= 0);

        engine->ShutDownAndRelease();
    }

    TEST_CASE("PL1.3: asFUNCTION(0) safety - does engine accept null func pointer in Build()")
    {
        asIScriptEngine* engine = asCreateScriptEngine();
        REQUIRE(engine != nullptr);

        engine->RegisterObjectType("MyClass", 0, asOBJ_REF | asOBJ_NOCOUNT);

        int r = engine->RegisterObjectMethod(
            "MyClass", "void DoSomething()",
            asFUNCTION(0), asCALL_CDECL_OBJFIRST
        );
        MESSAGE("asFUNCTION(0) register result: " << r);

        asIScriptModule* mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
        mod->AddScriptSection("test", R"( void Main() { MyClass@ c; } )");
        int buildResult = mod->Build();
        MESSAGE("Build result with asFUNCTION(0) method: " << buildResult);
        
        // Let's assert it succeeds because we want to know if it fails.
        // Actually it might fail on some platforms if the engine verifies the pointer, 
        // but AS usually only verifies it if executing or if strict checking is enabled.
        CHECK(buildResult >= 0);

        engine->ShutDownAndRelease();
    }

    TEST_CASE("PL1.4: Full as.predefined script loads with 0 engine errors")
    {
        const char* PREDEFINED = R"(
            typedef uint32 size_t;
            class array<T> {
                uint length() const;
                void insertAt(uint index, const T& in value);
            }
            class string {
                uint Length() const;
                bool IsEmpty() const;
                string opAdd(const string& in s) const;
            }
            class CCustomClass {
                void ThisIsACCustomClassMemberFunction();
                int thisIsACCustomClassVariable;
            }
            namespace ThisIsANamespace {
                class NameSpaceClass {
                    void ThisIsANameSpaceClassFunction();
                    float thisIsANameSpaceClassVariable;
                }
            }
        )";

        asIScriptEngine* engine = asCreateScriptEngine();
        engine->SetMessageCallback(asFUNCTION(MessageCallback), 0, asCALL_CDECL);
        REQUIRE(engine != nullptr);

        SymbolTable table;
        bool loaded = PredefinedLoader::LoadFromSource(PREDEFINED, engine, table, "string", "array");
        CHECK(loaded == true);

        // We can't really call this custom function because we used asFUNCTION(0), 
        // but compiling a reference to it shouldn't crash if we don't execute.
        // Wait, AngelScript doesn't execute anything in Build(), it just compiles.
        // BUT calling a function might generate bytecode. Does generating bytecode for asFUNCTION(0) crash?
        // Let's test that!
        asIScriptModule* mod = engine->GetModule("user", asGM_ALWAYS_CREATE);
        mod->AddScriptSection("user", R"(
            void Main() {
                string s = "hola";
                array<int>@ arr;
                CCustomClass obj;
                // We should test compiling a function call!
                obj.ThisIsACCustomClassMemberFunction();
            }
        )");
        int r = mod->Build();
        CHECK(r >= 0); 

        engine->ShutDownAndRelease();
    }

    TEST_CASE("PL1.5: SymbolCollector extracts all types from as.predefined source")
    {
        const char* src = R"(
            class string { uint Length() const; }
            class array<T> { uint length() const; }
            namespace Engine { class RigidBody { float mass; } }
        )";

        Document doc("file:///as.predefined", src);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        CHECK(table.FindGlobalByName("string") != nullptr);
        // Wait, what name does the parser extract for class array<T>?
        // It might be "array" or "array<T>". We'll check the output in the log when we run.
        // Let's just check if it finds it.
        bool foundArray = table.FindGlobalByName("array<T>") != nullptr || table.FindGlobalByName("array") != nullptr;
        CHECK(foundArray == true);
        CHECK(table.FindGlobalByName("Engine") != nullptr);

        auto* ns = table.FindGlobalByName("Engine");
        if (ns) {
            CHECK(ns->children.size() > 0);
        }
    }
}
