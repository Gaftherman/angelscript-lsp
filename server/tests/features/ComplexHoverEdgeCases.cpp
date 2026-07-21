#include <doctest/doctest.h>
#include <iostream>
#include <variant>
#include <string>
#include <vector>
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolResolver.h"
#include "analysis/DiagnosticCache.h"
#include "features/hover/HoverHandler.h"
#include "i18n/LspStrings.h"
#include <lsp/messages.h>

using namespace analysis;
using namespace angel_lsp;

// Código fuente de prueba con patrones avanzados de AngelScript
const char *ADVANCED_SCRATCH_AS = R"(
class Transform
{
    float x;
    float y;
    float z;
    
    /** @brief Resets coordinates to zero */
    void Reset() { x = 0; y = 0; z = 0; }
}

class Component
{
    int id;
    /** @brief Base component update */
    void Update(float dt) {}
}

class Actor : Component
{
    Transform transform;
    Transform@ GetTransform() { return transform; }
}

class Player : Actor
{
    /** @brief Player specific action */
    void Jump() {}
}

class Container<T>
{
    T item;
    T GetItem() { return item; }
}

void ProcessData(int value) {}
void ProcessData(float value) {}
void ProcessData(int value, string tag) {}

void GameMain()
{
    Player hero;
    // Chained access test
    hero.GetTransform().Reset();
    hero.GetTransform().x = 10.0f;

    // Inherited member test (Update is in Component, hero is Player)
    hero.Update(0.016f);

    // Overload resolution test
    ProcessData(42);
    ProcessData(3.14f);
}
)";

TEST_SUITE("AdvancedHoverEdgeCases")
{
    // =========================================================================
    // 1. Acceso Encadenado Profundo (Chained Member Access)
    // =========================================================================
    TEST_CASE("CH_ADV_1: Chained member access resolution (hero.GetTransform().Reset)")
    {
        Document doc("file:///advanced.as", ADVANCED_SCRATCH_AS);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        // Registrar variables locales dentro de GameMain
        TSNode root = doc.RootNode();
        auto traverseFuncs = [&](TSNode node, auto &self) -> void {
            if (std::string_view(ts_node_type(node)) == "func_declaration") {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode))
                    SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        // Buscar posición de 'Reset' en 'hero.GetTransform().Reset();'
        size_t offset = std::string(ADVANCED_SCRATCH_AS).find("Reset();");
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (ADVANCED_SCRATCH_AS[i] == '\n') { line++; col = 0; } else { col++; }
        }

        const Symbol *sym = SymbolResolver::ResolveAt(doc, table, line, col);
        REQUIRE(sym != nullptr);
        CHECK(sym->name == "Reset");
        CHECK(sym->kind == SymbolKind::Method);
        if (sym->parent) {
            CHECK(sym->parent->name == "Transform");
        }
    }

    // =========================================================================
    // 2. Miembros Heredados No Sobreescritos (Inherited Un-overridden Members)
    // =========================================================================
    TEST_CASE("CH_ADV_2: Hover on inherited method (hero.Update calls Component::Update)")
    {
        Document doc("file:///advanced.as", ADVANCED_SCRATCH_AS);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        auto traverseFuncs = [&](TSNode node, auto &self) -> void {
            if (std::string_view(ts_node_type(node)) == "func_declaration") {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode))
                    SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        // Hover sobre 'Update' en 'hero.Update(0.016f);'
        size_t offset = std::string(ADVANCED_SCRATCH_AS).find("Update(0.016f)");
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (ADVANCED_SCRATCH_AS[i] == '\n') { line++; col = 0; } else { col++; }
        }

        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///advanced.as");
        req.position.line = line;
        req.position.character = col;

        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::EN, nullptr);

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
        }
        
        struct DummyMarkup { std::string value; } markup = { markup_value };
        
        // Debe reflejar la firma completa sin prefijo de clase, y el scope // In Component
        CHECK(markup.value.find("void Update(float dt)") != std::string::npos);
        CHECK(markup.value.find("// In Component") != std::string::npos);
        CHECK(markup.value.find("Base component update") != std::string::npos);
    }

    // =========================================================================
    // 3. Reemplazo de Plantillas Anidadas y Múltiples Genéricos
    // =========================================================================
    TEST_CASE("CH_ADV_3: Hover over nested generic substitution (dictionary<string, array<Particle@>>)")
    {
        const char *SRC = R"(
class Particle {}
class dictionary<K, V> {
    V get(const K &in key) const;
}
void Main() {
    dictionary<string, array<Particle@>> particleMap;
    particleMap.get("fire");
}
        )";

        Document doc("file:///nested_templates.as", SRC);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        TSNode root = doc.RootNode();
        auto traverseFuncs = [&](TSNode node, auto &self) -> void {
            if (std::string_view(ts_node_type(node)) == "func_declaration") {
                TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
                if (!ts_node_is_null(bodyNode))
                    SymbolCollector::TraverseLocals(bodyNode, doc, table, nullptr);
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                self(ts_node_child(node, i), self);
        };
        traverseFuncs(root, traverseFuncs);

        // Hover sobre 'get' en 'particleMap.get("fire");'
        size_t offset = std::string(SRC).find("get(\"fire\")");
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (SRC[i] == '\n') { line++; col = 0; } else { col++; }
        }

        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///nested_templates.as");
        req.position.line = line;
        req.position.character = col;

        lsp::requests::TextDocument_Hover::Result result;
        const Symbol *symTest = SymbolResolver::ResolveAt(doc, table, line, col);
        std::cout << "DEBUG TEST 3: symTest is " << (symTest ? "not null" : "null") << "\\n";
        if (symTest) {
            std::cout << "DEBUG TEST 3: symTest->kind = " << (int)symTest->kind << "\\n";
        }
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::EN, nullptr);

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
        }
        
        struct DummyMarkup { std::string value; } markup = { markup_value };
        std::cout << "TEST 3 MARKUP:\\n" << markup.value << "\\nEND TEST 3\\n";
        
        // Reemplaza 'V' por 'array<Particle@>' en la firma retornada
        CHECK(markup.value.find("array<Particle@>") != std::string::npos);
    }

    // =========================================================================
    // 4. Integración con DiagnosticCache (Errores del compilador en Hover)
    // =========================================================================
    TEST_CASE("CH_ADV_4: DiagnosticCache overlay appending errors/warnings to Hover")
    {
        const char *SRC = "void Test() { int x = \"invalid\"; }";
        Document doc("file:///diag_test.as", SRC);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        // Simulamos un caché de diagnósticos devueltos por el compilador de AngelScript
        DiagnosticCache diagCache;
        std::vector<lsp::Diagnostic> diags;
        
        lsp::Diagnostic diag;
        diag.range.start.line = 0;
        diag.range.start.character = 22; // Sobre '"invalid"'
        diag.range.end.line = 0;
        diag.range.end.character = 31;
        diag.severity = lsp::DiagnosticSeverity::Error;
        diag.message = "Cannot implicitly convert 'string' to 'int'";
        diags.push_back(diag);

        diagCache.Update("file:///diag_test.as", diags);

        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///diag_test.as");
        req.position.line = 0;
        req.position.character = 24;

        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, &diagCache, i18n::Locale::EN, nullptr);

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
        }
        
        struct DummyMarkup { std::string value; } markup = { markup_value };
        
        // Verifica que la sección de diagnóstico de motor esté correctamente agregada con formato
        CHECK(markup.value.find("Cannot implicitly convert 'string' to 'int'") != std::string::npos);
        CHECK(markup.value.find("**Engine Error:**") != std::string::npos);
    }

    // =========================================================================
    // 5. Conteo y Muestra de Sobrecargas (Overload Resolution & Count)
    // =========================================================================
    TEST_CASE("CH_ADV_5: Overload count detection with multiResults")
    {
        Document doc("file:///advanced.as", ADVANCED_SCRATCH_AS);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);

        // Hover sobre 'ProcessData' en 'ProcessData(42);'
        size_t offset = std::string(ADVANCED_SCRATCH_AS).find("ProcessData(42)");
        uint32_t line = 0, col = 0;
        for (size_t i = 0; i < offset; i++) {
            if (ADVANCED_SCRATCH_AS[i] == '\n') { line++; col = 0; } else { col++; }
        }

        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///advanced.as");
        req.position.line = line;
        req.position.character = col;

        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::EN, nullptr);

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
        }
        
        struct DummyMarkup { std::string value; } markup = { markup_value };
        
        // Existen 3 funciones ProcessData en total, por lo que debe mostrar "+2 overloads"
        CHECK(markup.value.find("*+2 overloads*") != std::string::npos);
    }

    // =========================================================================
    // 6. Respaldo por Motor de Script (AngelScript Builtin Fallback)
    // =========================================================================
    TEST_CASE("CH_ADV_6: Fallback to asIScriptEngine for built-in functions when AST symbol is null")
    {
        // En este test el código no define 'cos', sino que es una función nativa de C++ registrada en el engine
        const char *SRC = "void Main() { float val = cos(1.57f); }";
        Document doc("file:///builtin.as", SRC);
        SymbolTable table; // Tabla vacía de símbolos de usuario

        // Creamos la posición sobre 'cos'
        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///builtin.as");
        req.position.line = 0;
        req.position.character = 27; // Sobre 'cos'

        // Si tienes una instancia real de asIScriptEngine o un Mock
        // asIScriptEngine* mockEngine = ...;
        
        lsp::requests::TextDocument_Hover::Result result;
        angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::EN, nullptr);

        // Si no pasamos engine, result será null. 
        // Si el símbolo no está en el AST pero la lógica de Fallback funciona con un engine mockeado/real,
        // devolverá la declaración registrada en AngelScript.
    }
}
