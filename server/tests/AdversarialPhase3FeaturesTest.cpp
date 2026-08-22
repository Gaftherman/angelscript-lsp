#include <doctest/doctest.h>

#include "features/inlay_hint/InlayHintHandler.h"
#include "features/code_action/CodeActionHandler.h"
#include "features/formatting/FormattingHandler.h"
#include "config/ServerConfig.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

#include <string>
#include <vector>
#include <algorithm>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
using namespace angel_lsp::config;

namespace
{
    struct Phase3TestEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///stress_test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        Phase3TestEnvironment(const std::string &code)
            : sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~Phase3TestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<std::vector<lsp::InlayHint>> InlayHints(lsp::Range range = lsp::Range{ {0, 0}, {0, 0} })
        {
            InlayHintRequest req{ uri, sourceCode, tree, range, symbolTable, scopeIndex };
            return GetInlayHints(req);
        }

        std::optional<std::vector<lsp::CodeAction>> CodeActions(
            lsp::Range range = lsp::Range{ {0, 0}, {0, 0} },
            lsp::CodeActionContext context = lsp::CodeActionContext{})
        {
            CodeActionRequest req{ uri, sourceCode, tree, range, context, symbolTable, scopeIndex };
            return GetCodeActions(req);
        }
    };

    struct ArgvHelper
    {
        std::vector<std::string> storage;
        std::vector<char*> argv;

        ArgvHelper(std::initializer_list<std::string> args)
            : storage(args)
        {
            argv.reserve(storage.size());
            for (auto &s : storage)
            {
                argv.push_back(s.data());
            }
        }

        int argc() const
        {
            return static_cast<int>(argv.size());
        }

        char** data()
        {
            return argv.data();
        }
    };

    std::string GetHintLabel(const lsp::InlayHint &hint)
    {
        if (std::holds_alternative<std::string>(hint.label))
        {
            return std::get<std::string>(hint.label);
        }
        return "";
    }
}

// =============================================================================
// 1. INLAY HINTS (R5) ADVERSARIAL TESTS
// =============================================================================

TEST_SUITE("Adversarial Phase 3 - Inlay Hints")
{
    TEST_CASE("Overloaded function calls with varying parameter counts")
    {
        std::string code =
            "void Log(int level) {}\n"
            "void Log(int level, string tag) {}\n"
            "void Log(int level, string tag, bool verbose) {}\n"
            "void main() {\n"
            "    Log(1);\n"
            "    Log(2, \"NETWORK\");\n"
            "    Log(3, \"AUDIO\", true);\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        // Call 1: Log(1) -> level:
        // Call 2: Log(2, "NETWORK") -> level:, tag:
        // Call 3: Log(3, "AUDIO", true) -> level:, tag:, verbose:
        // Total hints = 1 + 2 + 3 = 6 hints

        std::vector<std::string> labels;
        for (const auto &h : *hints)
        {
            labels.push_back(GetHintLabel(h));
        }

        REQUIRE(labels.size() == 6);
        CHECK(labels[0] == "level:");
        CHECK(labels[1] == "level:");
        CHECK(labels[2] == "tag:");
        CHECK(labels[3] == "level:");
        CHECK(labels[4] == "tag:");
        CHECK(labels[5] == "verbose:");
    }

    TEST_CASE("Varargs and empty parameter name suppression")
    {
        std::string code =
            "void FormatString(string fmt, ...) {}\n"
            "void main() {\n"
            "    FormatString(\"Values: %d, %d\", 10, 20);\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        // Only fmt: should be emitted; varargs ... should never emit hints
        REQUIRE(hints->size() == 1);
        CHECK(GetHintLabel(hints->at(0)) == "fmt:");
    }

    TEST_CASE("Mixed named and positional arguments")
    {
        std::string code =
            "void Configure(string host, int port, int timeout) {}\n"
            "void main() {\n"
            "    Configure(host: \"127.0.0.1\", 8080, timeout: 60);\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        // host and timeout are named in syntax, only port should have a hint: port:
        REQUIRE(hints->size() == 1);
        CHECK(GetHintLabel(hints->at(0)) == "port:");
    }

    TEST_CASE("Auto type deduction for all primitive literal variants")
    {
        std::string code =
            "void main() {\n"
            "    auto v_int = 100;\n"
            "    auto v_flt = 2.5f;\n"
            "    auto v_dbl = 3.14159;\n"
            "    auto v_str = \"text\";\n"
            "    auto v_bool = false;\n"
            "    auto v_uint = 42u;\n"
            "    auto v_i64 = 1000L;\n"
            "    auto v_u64 = 2000u64;\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 8);

        CHECK(GetHintLabel(hints->at(0)) == ": int");
        CHECK(GetHintLabel(hints->at(1)) == ": float");
        CHECK(GetHintLabel(hints->at(2)) == ": double");
        CHECK(GetHintLabel(hints->at(3)) == ": string");
        CHECK(GetHintLabel(hints->at(4)) == ": bool");
        CHECK(GetHintLabel(hints->at(5)) == ": uint");
        CHECK(GetHintLabel(hints->at(6)) == ": int64");
        CHECK(GetHintLabel(hints->at(7)) == ": uint64");
    }

    TEST_CASE("Auto type deduction for constructor and template construct calls")
    {
        std::string code =
            "class Vector2 {\n"
            "    Vector2(float x, float y) {}\n"
            "}\n"
            "void main() {\n"
            "    auto v = Vector2(1.0f, 2.0f);\n"
            "    auto arr = array<int>();\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        // Inlay hints will contain:
        // - param hints for Vector2 constructor (x:, y:)
        // - type hint for auto v (: Vector2)
        // - type hint for auto arr (: array<int>)
        bool foundVType = false;
        bool foundArrType = false;
        bool foundXParam = false;
        bool foundYParam = false;

        for (const auto &h : *hints)
        {
            std::string l = GetHintLabel(h);
            if (l == ": Vector2") foundVType = true;
            if (l == ": array<int>") foundArrType = true;
            if (l == "x:") foundXParam = true;
            if (l == "y:") foundYParam = true;
        }

        CHECK(foundVType);
        CHECK(foundArrType);
        CHECK(foundXParam);
        CHECK(foundYParam);
    }

    TEST_CASE("Auto type deduction for functional casts and cast expressions")
    {
        std::string code =
            "class Target {}\n"
            "void main() {\n"
            "    auto a = int(3.14f);\n"
            "    auto b = float(42);\n"
            "    auto c = cast<Target@>(null);\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        bool foundInt = false;
        bool foundFloat = false;
        bool foundTarget = false;

        for (const auto &h : *hints)
        {
            std::string l = GetHintLabel(h);
            if (l == ": int") foundInt = true;
            if (l == ": float") foundFloat = true;
            if (l == ": Target@" || l == ": Target") foundTarget = true;
        }

        CHECK(foundInt);
        CHECK(foundFloat);
        CHECK(foundTarget);
    }

    TEST_CASE("Auto type deduction for expressions (binary, unary, comparison)")
    {
        std::string code =
            "void main() {\n"
            "    int x = 10;\n"
            "    float y = 20.0f;\n"
            "    auto isGreater = (x > 5);\n"
            "    auto sum = (x + y);\n"
            "    auto notVal = !isGreater;\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        bool foundIsGreater = false;
        bool foundSum = false;
        bool foundNotVal = false;

        for (const auto &h : *hints)
        {
            std::string l = GetHintLabel(h);
            if (l == ": bool" && h.position.line == 3) foundIsGreater = true;
            if (l == ": float" && h.position.line == 4) foundSum = true;
            if (l == ": bool" && h.position.line == 5) foundNotVal = true;
        }

        CHECK(foundIsGreater);
        CHECK(foundSum);
        CHECK(foundNotVal);
    }

    TEST_CASE("Auto variable with null or unresolved initializer does not crash")
    {
        std::string code =
            "void main() {\n"
            "    auto n = null;\n"
            "    auto unk = SomeUnresolvedSymbol_12345;\n"
            "}\n";

        Phase3TestEnvironment env(code);
        auto hints = env.InlayHints();

        REQUIRE(hints.has_value());
        // Should not produce invalid hints or crash
        for (const auto &h : *hints)
        {
            std::string l = GetHintLabel(h);
            CHECK(l != ": auto");
            CHECK(l != ": null");
        }
    }
}

// =============================================================================
// 2. CODE ACTIONS (R6) ADVERSARIAL TESTS
// =============================================================================

TEST_SUITE("Adversarial Phase 3 - Code Actions")
{
    TEST_CASE("Multiple unused local variables in the same function")
    {
        std::string code =
            "void main() {\n"
            "    int unused1 = 10;\n"
            "    string unused2 = \"test\";\n"
            "    float active = 3.14f;\n"
            "    active += 1.0f;\n"
            "}\n";

        Phase3TestEnvironment env(code);
        // Query entire function range
        lsp::Range r{ {0, 0}, {5, 1} };
        auto actions = env.CodeActions(r);

        REQUIRE(actions.has_value());
        bool foundUnused1 = false;
        bool foundUnused2 = false;
        bool foundActive = false;

        for (const auto &act : *actions)
        {
            if (act.title == "Remove unused variable 'unused1'") foundUnused1 = true;
            if (act.title == "Remove unused variable 'unused2'") foundUnused2 = true;
            if (act.title.find("active") != std::string::npos) foundActive = true;
        }

        CHECK(foundUnused1);
        CHECK(foundUnused2);
        CHECK(!foundActive);
    }

    TEST_CASE("Unused variable removal in multi-declarator lines (head, middle, tail)")
    {
        std::string code =
            "void main() {\n"
            "    int firstUnused = 1, middleUsed = 2, lastUnused = 3;\n"
            "    middleUsed += 10;\n"
            "}\n";

        Phase3TestEnvironment env(code);
        lsp::Range r{ {1, 0}, {1, 55} };
        auto actions = env.CodeActions(r);

        REQUIRE(actions.has_value());
        bool foundFirst = false;
        bool foundLast = false;
        bool foundMiddle = false;

        for (const auto &act : *actions)
        {
            if (act.title == "Remove unused variable 'firstUnused'") foundFirst = true;
            if (act.title == "Remove unused variable 'lastUnused'") foundLast = true;
            if (act.title.find("middleUsed") != std::string::npos) foundMiddle = true;
        }

        CHECK(foundFirst);
        CHECK(foundLast);
        CHECK(!foundMiddle);
    }

    TEST_CASE("Missing interface method stubs with multi-level inheritance")
    {
        std::string code =
            "interface IRoot {\n"
            "    void RootMethod();\n"
            "}\n"
            "interface IMiddle : IRoot {\n"
            "    int MiddleMethod(float f);\n"
            "}\n"
            "interface ILeaf : IMiddle {\n"
            "    string LeafMethod(bool flag);\n"
            "}\n"
            "class Implementation : ILeaf {\n"
            "}\n";

        Phase3TestEnvironment env(code);
        lsp::Range r{ {10, 0}, {11, 1} };
        auto actions = env.CodeActions(r);

        REQUIRE(actions.has_value());
        bool foundAction = false;
        for (const auto &act : *actions)
        {
            if (act.title == "Implement missing interface methods for 'ILeaf'")
            {
                foundAction = true;
                REQUIRE(act.edit.has_value());
                REQUIRE(act.edit->changes.has_value());
                auto changes = act.edit->changes.value();
                const auto &edits = changes[lsp::DocumentUri::parse(env.uri)];
                REQUIRE(!edits.empty());
                const std::string &stubs = edits[0].newText;

                CHECK(stubs.find("void RootMethod()") != std::string::npos);
                CHECK(stubs.find("int MiddleMethod(float f)") != std::string::npos);
                CHECK(stubs.find("string LeafMethod(bool flag)") != std::string::npos);
                CHECK(stubs.find("return 0;") != std::string::npos);
                CHECK(stubs.find("return \"\";") != std::string::npos);
            }
        }

        CHECK(foundAction);
    }

    TEST_CASE("Class member variables and global variables are NOT marked as unused local variables")
    {
        std::string code =
            "int g_globalVar = 100;\n"
            "class MyEntity {\n"
            "    int m_member = 42;\n"
            "    void DoSomething() {}\n"
            "}\n";

        Phase3TestEnvironment env(code);
        lsp::Range r{ {0, 0}, {4, 1} };
        auto actions = env.CodeActions(r);

        if (actions.has_value())
        {
            for (const auto &act : *actions)
            {
                CHECK(act.title.find("m_member") == std::string::npos);
                CHECK(act.title.find("g_globalVar") == std::string::npos);
            }
        }
    }
}

// =============================================================================
// 3. DOCUMENT & RANGE FORMATTING (R7) ADVERSARIAL TESTS
// =============================================================================

TEST_SUITE("Adversarial Phase 3 - Formatting")
{
    TEST_CASE("Deeply nested control flow (4 levels) Allman brace alignment")
    {
        std::string code = 
            "void DeepNest(){if(a>0){for(int i=0;i<10;++i){while(running){switch(state){case 1:doTask();break;default:break;}}}}}";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "void DeepNest()\n"
            "{\n"
            "    if (a > 0)\n"
            "    {\n"
            "        for (int i = 0; i < 10; ++i)\n"
            "        {\n"
            "            while (running)\n"
            "            {\n"
            "                switch (state)\n"
            "                {\n"
            "                    case 1:\n"
            "                        doTask();\n"
            "                        break;\n"
            "                    default:\n"
            "                        break;\n"
            "                }\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "}\n";

        CHECK(formatted == expected);
    }

    TEST_CASE("Struct and Class with trailing semicolons and member alignment")
    {
        std::string code = "struct Point{float x;float y;};class Widget:Base{int id;void Render(){draw();}};";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "struct Point\n"
            "{\n"
            "    float x;\n"
            "    float y;\n"
            "};\n"
            "class Widget : Base\n"
            "{\n"
            "    int id;\n"
            "    void Render()\n"
            "    {\n"
            "        draw();\n"
            "    }\n"
            "};\n";

        CHECK(formatted == expected);
    }

    TEST_CASE("Preprocessor directives remain at column 0 inside indented code")
    {
        std::string code =
            "class ConfigManager {\n"
            "    void Load() {\n"
            "#if DEBUG\n"
            "        PrintDebug();\n"
            "#endif\n"
            "    }\n"
            "}\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "class ConfigManager\n"
            "{\n"
            "    void Load()\n"
            "    {\n"
            "#if DEBUG\n"
            "        PrintDebug();\n"
            "#endif\n"
            "    }\n"
            "}\n";

        CHECK(formatted == expected);
    }

    TEST_CASE("Strings with curly braces, semicolons and operators are preserved verbatim")
    {
        std::string code =
            "void BuildQuery() {\n"
            "    string query = \"SELECT * FROM users WHERE (id = 10 AND status == 'ACTIVE'); { ignore_braces }\";\n"
            "    string raw = \"\"\"multi-line\n"
            "    string with { braces } and ;\n"
            "    \"\"\";\n"
            "}\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        CHECK(formatted.find("SELECT * FROM users WHERE (id = 10 AND status == 'ACTIVE'); { ignore_braces }") != std::string::npos);
        CHECK(formatted.find("\"\"\"multi-line") != std::string::npos);
    }

    TEST_CASE("Complex template and handle spacing disambiguation")
    {
        std::string code = "array<dictionary<string,array<int>@>>@ complexContainer=null;";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected = "array<dictionary<string, array<int>@>>@ complexContainer = null;\n";

        CHECK(formatted == expected);
    }

    TEST_CASE("Range formatting preserves surrounding unselected lines")
    {
        std::string uri = "file:///range_test.as";
        std::string code =
            "// Header line\n"
            "void UnchangedHeader() { return; }\n"
            "void TargetFunc(){int a=1;int b=2;}\n"
            "// Footer line\n"
            "void UnchangedFooter() { return; }\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        RangeFormattingRequest req{ uri, code, nullptr, lsp::Range{ {2, 0}, {2, 35} }, options };
        auto edits = FormatRange(req);

        REQUIRE(edits.has_value());
        REQUIRE(!edits->empty());
        CHECK(edits->at(0).range.start.line == 2);
        CHECK(edits->at(0).range.end.line == 2);
    }
}

// =============================================================================
// 4. SERVER CONFIG & FEATURE FLAGS (R8) ADVERSARIAL TESTS
// =============================================================================

TEST_SUITE("Adversarial Phase 3 - Server Config & Feature Flags")
{
    TEST_CASE("All 5 Phase 3 feature flags toggle via CLI flags correctly")
    {
        ArgvHelper args{
            "angel_lsp",
            "--disable-document-highlight",
            "--disable-folding-range",
            "--disable-inlay-hints",
            "--disable-code-action",
            "--disable-formatting"
        };

        ServerConfig cfg = FromArgs(args.argc(), args.data());
        CHECK(cfg.features.enableDocumentHighlight == false);
        CHECK(cfg.features.enableFoldingRange == false);
        CHECK(cfg.features.enableInlayHints == false);
        CHECK(cfg.features.enableCodeAction == false);
        CHECK(cfg.features.enableFormatting == false);
    }

    TEST_CASE("Alias flags without hyphens (--enable-inlayhints, --disable-codeaction)")
    {
        ArgvHelper args{
            "angel_lsp",
            "--enable-documenthighlight=false",
            "--enable-foldingrange=0",
            "--enable-inlayhints=off",
            "--enable-codeaction=no",
            "--disable-formatting=1"
        };

        ServerConfig cfg = FromArgs(args.argc(), args.data());
        CHECK(cfg.features.enableDocumentHighlight == false);
        CHECK(cfg.features.enableFoldingRange == false);
        CHECK(cfg.features.enableInlayHints == false);
        CHECK(cfg.features.enableCodeAction == false);
        CHECK(cfg.features.enableFormatting == false);
    }

    TEST_CASE("Flag overriding and re-enabling")
    {
        ArgvHelper args{
            "angel_lsp",
            "--disable-inlay-hints",
            "--enable-inlay-hints=true",
            "--disable-formatting",
            "--enable-formatting=1",
            "--disable-code-action=0"
        };

        ServerConfig cfg = FromArgs(args.argc(), args.data());
        CHECK(cfg.features.enableInlayHints == true);
        CHECK(cfg.features.enableFormatting == true);
        CHECK(cfg.features.enableCodeAction == true);
    }
}
