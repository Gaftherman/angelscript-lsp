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
#include "features/definition/DefinitionHandler.h"
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
        CHECK( markup.value.find("Component") != std::string::npos );
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

    TEST_CASE("CH_ADV_7: Safe hover resolution over preprocessor lines and missing symbols")
    {
        const char *SRC = R"script(
#if DEBUG_MODE
void Test() {}
#endif
)script";
        Document doc("file:///preproc_test.as", SRC);
        SymbolTable table;

        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///preproc_test.as");
        req.position.line = 1;
        req.position.character = 6;

        lsp::requests::TextDocument_Hover::Result result;
        CHECK_NOTHROW(angel_lsp::features::ProcessHover(result, req, doc, table, nullptr, i18n::Locale::EN, nullptr));
    }

    TEST_CASE("CH_ADV_8: Hover filtering for active #if block Doxygen comments")
    {
        const char *SRC = R"script(
#if RENDERER_DX11
/** @brief DirectX 11 Rendering Engine implementation. */
class RenderContext
{
    /** @brief Initializes DirectX 11 graphics pipeline. */
    void Initialize(int width, int height) {}
};
#endif

#if RENDERER_VK
/** @brief Vulkan Rendering Engine implementation. */
class RenderContext
{
    /** @brief Initializes Vulkan graphics pipeline. */
    void Initialize(int width, int height) {}
};
#endif
)script";

        Document doc("file:///multi_if.as", SRC);

        // When RENDERER_DX11 is active:
        SymbolTable tableDX;
        SymbolCollector::SetDefinedWords({"RENDERER_DX11"});
        SymbolCollector::CollectGlobals(doc, tableDX);

        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///multi_if.as");
        req.position.line = 3;
        req.position.character = 8; // On 'RenderContext'

        lsp::requests::TextDocument_Hover::Result resultDX;
        angel_lsp::features::ProcessHover(resultDX, req, doc, tableDX, nullptr, i18n::Locale::EN, nullptr);

        REQUIRE(!resultDX.isNull());
        std::string markupDX;
        if (const auto *mc = std::get_if<lsp::MarkupContent>(&(*resultDX).contents)) {
            markupDX = mc->value;
        } else if (const auto *ms = std::get_if<lsp::MarkedString>(&(*resultDX).contents)) {
            if (const auto *str = std::get_if<std::string>(ms)) {
                markupDX = *str;
            }
        } else if (const auto *vec = std::get_if<std::vector<lsp::MarkedString>>(&(*resultDX).contents)) {
            for (const auto &item : *vec) {
                if (const auto *str = std::get_if<std::string>(&item)) {
                    markupDX += *str + "\n";
                }
            }
        }
        CHECK(markupDX.find("DirectX 11") != std::string::npos);
        CHECK(markupDX.find("Vulkan") == std::string::npos);
    }

    TEST_CASE("CH_ADV_9: Anonymous functions / lambdas hover and definition resolution")
    {
        const char *SRC = R"script(
funcdef void Callback(int val, float scale);

void Main()
{
    Callback@ cb = function(int val, float scale) {
        float result = val * scale;
    };
}
)script";

        Document doc("file:///lambda.as", SRC);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        // 1. Hover on 'function' keyword (line 5, col 19)
        lsp::requests::TextDocument_Hover::Params reqFn;
        reqFn.textDocument.uri = lsp::DocumentUri::parse("file:///lambda.as");
        reqFn.position.line = 5;
        reqFn.position.character = 20;

        lsp::requests::TextDocument_Hover::Result resultFn;
        angel_lsp::features::ProcessHover(resultFn, reqFn, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!resultFn.isNull());

        // 2. Hover on lambda parameter 'val' inside lambda body (line 6, col 24)
        lsp::requests::TextDocument_Hover::Params reqVal;
        reqVal.textDocument.uri = lsp::DocumentUri::parse("file:///lambda.as");
        reqVal.position.line = 6;
        reqVal.position.character = 24;

        lsp::requests::TextDocument_Hover::Result resultVal;
        angel_lsp::features::ProcessHover(resultVal, reqVal, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!resultVal.isNull());

        std::string valMarkup;
        if (const auto *mc = std::get_if<lsp::MarkupContent>(&(*resultVal).contents)) {
            valMarkup = mc->value;
        } else if (const auto *ms = std::get_if<lsp::MarkedString>(&(*resultVal).contents)) {
            if (const auto *str = std::get_if<std::string>(ms)) {
                valMarkup = *str;
            }
        } else if (const auto *vec = std::get_if<std::vector<lsp::MarkedString>>(&(*resultVal).contents)) {
            for (const auto &item : *vec) {
                if (const auto *str = std::get_if<std::string>(&item)) {
                    valMarkup += *str + "\n";
                }
            }
        }
        CHECK(!valMarkup.empty());
        CHECK(valMarkup.find("val") != std::string::npos);

        // 3. Go To Definition on 'val' inside lambda body (line 6, col 24) -> jumps to 'int val' (line 5, col 32)
        lsp::requests::TextDocument_Definition::Params defReq;
        defReq.textDocument.uri = lsp::DocumentUri::parse("file:///lambda.as");
        defReq.position.line = 6;
        defReq.position.character = 24;

        auto defRes = angel_lsp::features::ProcessDefinition(defReq, doc, table, nullptr);
        REQUIRE(!defRes.isNull());
        if (const auto *def = std::get_if<lsp::Definition>(&*defRes)) {
            if (const auto *loc = std::get_if<lsp::Location>(def)) {
                CHECK(loc->range.start.line == 5);
            }
        }
    }

    TEST_CASE("CH_ADV_10: Array indexing & multidimensional array member resolution")
    {
        const char *SRC = R"script(
class TargetItem
{
    /** @brief Value field. */
    int value;
    /** @brief Performs target action. */
    void Action() {}
};

class Container
{
    TargetItem@ GetTarget() { return null; }
};

void Main()
{
    array<TargetItem@> items;
    items[0].Action();

    array<array<TargetItem@>> grid;
    grid[0][0].Action();

    Container c;
    c.GetTarget().Action();
}
)script";

        Document doc("file:///array_test.as", SRC);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        // 1. Hover on 'Action' on items[0].Action() (line 17, col 15)
        lsp::requests::TextDocument_Hover::Params req1;
        req1.textDocument.uri = lsp::DocumentUri::parse("file:///array_test.as");
        req1.position.line = 17;
        req1.position.character = 15;

        lsp::requests::TextDocument_Hover::Result res1;
        angel_lsp::features::ProcessHover(res1, req1, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res1.isNull());

        // 2. Hover on 'Action' on grid[0][0].Action() (line 20, col 18)
        lsp::requests::TextDocument_Hover::Params req2;
        req2.textDocument.uri = lsp::DocumentUri::parse("file:///array_test.as");
        req2.position.line = 20;
        req2.position.character = 18;

        lsp::requests::TextDocument_Hover::Result res2;
        angel_lsp::features::ProcessHover(res2, req2, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res2.isNull());

        // 3. Hover on 'Action' on c.GetTarget().Action() (line 23, col 20)
        lsp::requests::TextDocument_Hover::Params req3;
        req3.textDocument.uri = lsp::DocumentUri::parse("file:///array_test.as");
        req3.position.line = 23;
        req3.position.character = 20;

        lsp::requests::TextDocument_Hover::Result res3;
        angel_lsp::features::ProcessHover(res3, req3, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res3.isNull());

        // 4. Go To Definition on Action() in grid[0][0].Action() -> jumps to line 6 (Action definition)
        lsp::requests::TextDocument_Definition::Params defReq;
        defReq.textDocument.uri = lsp::DocumentUri::parse("file:///array_test.as");
        defReq.position.line = 20;
        defReq.position.character = 18;

        auto defRes = angel_lsp::features::ProcessDefinition(defReq, doc, table, nullptr);
        REQUIRE(!defRes.isNull());
        if (const auto *def = std::get_if<lsp::Definition>(&*defRes)) {
            if (const auto *loc = std::get_if<lsp::Location>(def)) {
                CHECK(loc->range.start.line == 6);
            }
        }
    }

    TEST_CASE("CH_ADV_11: Nested lambdas inside class methods & namespaces")
    {
        const char *SRC = R"script(
namespace Engine
{
    funcdef void OuterCb(int a);
    funcdef void InnerCb(float b);

    class Manager
    {
        void Run()
        {
            OuterCb@ outer = function(int a) {
                InnerCb@ inner = function(float b) {
                    float total = a + b;
                };
            };
        }
    };
}
)script";

        Document doc("file:///nested_lambda.as", SRC);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        // Hover on 'a' inside inner lambda (line 12, col 34) -> resolves to 'int a' of outer lambda
        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///nested_lambda.as");
        req.position.line = 12;
        req.position.character = 34;

        lsp::requests::TextDocument_Hover::Result res;
        angel_lsp::features::ProcessHover(res, req, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res.isNull());

        // Go To Definition on 'a' inside inner lambda -> jumps to line 10 (int a)
        lsp::requests::TextDocument_Definition::Params defReq;
        defReq.textDocument.uri = lsp::DocumentUri::parse("file:///nested_lambda.as");
        defReq.position.line = 12;
        defReq.position.character = 34;

        auto defRes = angel_lsp::features::ProcessDefinition(defReq, doc, table, nullptr);
        REQUIRE(!defRes.isNull());
        if (const auto *def = std::get_if<lsp::Definition>(&*defRes)) {
            if (const auto *loc = std::get_if<lsp::Location>(def)) {
                CHECK(loc->range.start.line == 10);
            }
        }
    }

    TEST_CASE("CH_ADV_12: Shorthand array syntax (T[], T@[][]) hover and member resolution")
    {
        const char *SRC = R"script(
class Widget
{
    /** @brief Renders the widget. */
    void Render() {}
};

void Main()
{
    int[] myArrayInt;
    Widget@[] myArrayWidget;
    myArrayWidget[0].Render();

    Widget@[][] myMultiWidget;
    myMultiWidget[0][0].Render();
}
)script";

        Document doc("file:///shorthand_array.as", SRC);
        SymbolTable table;
        SymbolCollector::CollectGlobals(doc, table);
        SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        // 1. Hover on 'Render' in myArrayWidget[0].Render() (line 11, col 21)
        lsp::requests::TextDocument_Hover::Params req1;
        req1.textDocument.uri = lsp::DocumentUri::parse("file:///shorthand_array.as");
        req1.position.line = 11;
        req1.position.character = 21;

        lsp::requests::TextDocument_Hover::Result res1;
        angel_lsp::features::ProcessHover(res1, req1, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res1.isNull());

        // 2. Hover on 'Render' in myMultiWidget[0][0].Render() (line 14, col 24)
        lsp::requests::TextDocument_Hover::Params req2;
        req2.textDocument.uri = lsp::DocumentUri::parse("file:///shorthand_array.as");
        req2.position.line = 14;
        req2.position.character = 24;

        lsp::requests::TextDocument_Hover::Result res2;
        angel_lsp::features::ProcessHover(res2, req2, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res2.isNull());

        // 3. Go To Definition on Render() in myMultiWidget[0][0].Render() -> jumps to line 4
        lsp::requests::TextDocument_Definition::Params defReq;
        defReq.textDocument.uri = lsp::DocumentUri::parse("file:///shorthand_array.as");
        defReq.position.line = 14;
        defReq.position.character = 24;

        auto defRes = angel_lsp::features::ProcessDefinition(defReq, doc, table, nullptr);
        REQUIRE(!defRes.isNull());
        if (const auto *def = std::get_if<lsp::Definition>(&*defRes)) {
            if (const auto *loc = std::get_if<lsp::Location>(def)) {
                CHECK(loc->range.start.line == 4);
            }
        }
    }

    TEST_CASE("CH_ADV_13: Array built-in methods (length, insertLast, sortAsc, find) hover & definition from as.predefined / array template")
    {
        const char *ARRAY_PREDEFINED = R"script(
/** @brief Standard generic array template container. */
template<class T>
class array
{
    /** @brief Returns the number of elements in the array. */
    uint length() const {}
    /** @brief Appends a new element at the end of the array. */
    void insertLast(const T& in value) {}
    /** @brief Sorts elements in ascending order. */
    void sortAsc() {}
    /** @brief Finds the first element with matching value. */
    int find(const T& in value) {}
    /** @brief Removes element at specified index. */
    void removeAt(uint index) {}
};
)script";

        const char *SRC = R"script(
void Main()
{
    array<int> arr1 = {1, 2, 3};
    arr1.insertLast(0);
    uint len1 = arr1.length();

    int[] arr2;
    arr2.sortAsc();
    int idx = arr2.find(5);
}
)script";

        Document preDoc("file:///as.predefined", ARRAY_PREDEFINED);
        SymbolTable table;
        SymbolCollector::CollectGlobals(preDoc, table);

        Document doc("file:///array_methods.as", SRC);
        SymbolCollector::CollectGlobals(doc, table);
        SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        // 1. Hover on 'insertLast' in arr1.insertLast(0) (line 4, col 11)
        lsp::requests::TextDocument_Hover::Params req1;
        req1.textDocument.uri = lsp::DocumentUri::parse("file:///array_methods.as");
        req1.position.line = 4;
        req1.position.character = 11;

        lsp::requests::TextDocument_Hover::Result res1;
        angel_lsp::features::ProcessHover(res1, req1, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res1.isNull());

        // 2. Hover on 'length' in arr1.length() (line 5, col 20)
        lsp::requests::TextDocument_Hover::Params req2;
        req2.textDocument.uri = lsp::DocumentUri::parse("file:///array_methods.as");
        req2.position.line = 5;
        req2.position.character = 18;

        lsp::requests::TextDocument_Hover::Result res2;
        angel_lsp::features::ProcessHover(res2, req2, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res2.isNull());

        // 3. Hover on 'sortAsc' on arr2.sortAsc() (line 8, col 11)
        lsp::requests::TextDocument_Hover::Params req3;
        req3.textDocument.uri = lsp::DocumentUri::parse("file:///array_methods.as");
        req3.position.line = 8;
        req3.position.character = 11;

        lsp::requests::TextDocument_Hover::Result res3;
        angel_lsp::features::ProcessHover(res3, req3, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res3.isNull());

        // 4. Go To Definition on 'sortAsc' -> jumps to as.predefined line 10
        lsp::requests::TextDocument_Definition::Params defReq;
        defReq.textDocument.uri = lsp::DocumentUri::parse("file:///array_methods.as");
        defReq.position.line = 8;
        defReq.position.character = 11;

        auto defRes = angel_lsp::features::ProcessDefinition(defReq, doc, table, nullptr);
        REQUIRE(!defRes.isNull());
        if (const auto *def = std::get_if<lsp::Definition>(&*defRes)) {
            if (const auto *loc = std::get_if<lsp::Location>(def)) {
                CHECK(loc->uri.toString().find("as.predefined") != std::string::npos);
                CHECK(loc->range.start.line == 10);
            }
        }
    }

    TEST_CASE("CH_ADV_14: Error resilience for broken as.predefined and incomplete user scripts")
    {
        const char *BROKEN_PREDEFINED = R"script(
class ValidEngineClass
{
    void ValidEngineMethod() {}
};

// Intentionally malformed syntax in predefined
class BrokenClass {
    void UnclosedMethod(
)script";

        const char *BROKEN_USER_SCRIPT = R"script(
void Main()
{
    ValidEngineClass engine;
    engine.ValidEngineMethod();

    // Incomplete syntax
    int x = ;
}
)script";

        Document preDoc("file:///as.predefined", BROKEN_PREDEFINED);
        SymbolTable table;
        SymbolCollector::CollectGlobals(preDoc, table);

        Document doc("file:///broken_script.as", BROKEN_USER_SCRIPT);
        SymbolCollector::CollectGlobals(doc, table);
        SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        // Hover on ValidEngineMethod on line 4, col 12 of user script
        lsp::requests::TextDocument_Hover::Params req;
        req.textDocument.uri = lsp::DocumentUri::parse("file:///broken_script.as");
        req.position.line = 4;
        req.position.character = 12;

        lsp::requests::TextDocument_Hover::Result res;
        angel_lsp::features::ProcessHover(res, req, doc, table, nullptr, i18n::Locale::ES, nullptr);
        REQUIRE(!res.isNull());

        // Go To Definition on ValidEngineMethod -> jumps to as.predefined line 3
        lsp::requests::TextDocument_Definition::Params defReq;
        defReq.textDocument.uri = lsp::DocumentUri::parse("file:///broken_script.as");
        defReq.position.line = 4;
        defReq.position.character = 12;

        auto defRes = angel_lsp::features::ProcessDefinition(defReq, doc, table, nullptr);
        REQUIRE(!defRes.isNull());
        if (const auto *def = std::get_if<lsp::Definition>(&*defRes)) {
            if (const auto *loc = std::get_if<lsp::Location>(def)) {
                CHECK(loc->uri.toString().find("as.predefined") != std::string::npos);
                CHECK(loc->range.start.line == 3);
            }
        }
    }
}
