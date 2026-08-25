#include <doctest/doctest.h>
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    const Scope *FindInnermostScope(const Scope *root, uint32_t line, uint32_t character)
    {
        if (!root)
        {
            return nullptr;
        }

        const auto contains = [line, character](const Scope &scope)
        {
            if (line < scope.startLine || line > scope.endLine)
            {
                return false;
            }
            if (line == scope.startLine && character < scope.startCharacter)
            {
                return false;
            }
            if (line == scope.endLine && character > scope.endCharacter)
            {
                return false;
            }
            return true;
        };

        if (!contains(*root))
        {
            return nullptr;
        }

        const Scope *current = root;
        for (bool descended = true; descended;)
        {
            descended = false;
            for (const auto &child : current->children)
            {
                if (child && contains(*child))
                {
                    current = child.get();
                    descended = true;
                    break;
                }
            }
        }
        return current;
    }

    /**
     * @brief Helper to parse code and deduce the expression type for the first expression statement in main().
     */
    std::string DeduceTypeInMain(const std::string &code)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///test.as";
        collector.CollectSymbols(fileUri, code, parser, table);
        auto scopeRoot = scopes.CollectScopes(code, parser);
        TSTree *tree = parser.Parse(code);

        std::string result;
        if (tree)
        {
            TSNode root = ts_tree_root_node(tree);
            // Search for the last expression_statement or return_statement in main
            uint32_t count = ts_node_named_child_count(root);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode func = ts_node_named_child(root, i);
                if (std::string_view(ts_node_type(func)) == "func_declaration")
                {
                    TSNode body = ts_node_child_by_field_name(func, "body", 4);
                    if (!ts_node_is_null(body))
                    {
                        uint32_t stmtCount = ts_node_named_child_count(body);
                        for (uint32_t j = 0; j < stmtCount; ++j)
                        {
                            TSNode stmt = ts_node_named_child(body, j);
                            std::string_view stmtType = ts_node_type(stmt);
                            if (stmtType == "expression_statement" && ts_node_named_child_count(stmt) > 0)
                            {
                                TSNode expr = ts_node_named_child(stmt, 0);
                                TSPoint pt = ts_node_start_point(expr);
                                const Scope *innerScope = FindInnermostScope(scopeRoot.get(), pt.row, pt.column);
                                result = ResolveExpressionType(expr, innerScope ? innerScope : scopeRoot.get(), table, code, fileUri);
                            }
                            else if (stmtType == "return_statement" && ts_node_named_child_count(stmt) > 0)
                            {
                                TSNode expr = ts_node_named_child(stmt, 0);
                                TSPoint pt = ts_node_start_point(expr);
                                const Scope *innerScope = FindInnermostScope(scopeRoot.get(), pt.row, pt.column);
                                result = ResolveExpressionType(expr, innerScope ? innerScope : scopeRoot.get(), table, code, fileUri);
                            }
                        }
                    }
                }
            }
            ts_tree_delete(tree);
        }
        return result;
    }
}

TEST_SUITE("ExpressionTypeDeduction")
{
    TEST_CASE("Primitive Arithmetic Promotion")
    {
        CHECK(DeduceTypeInMain("void main() { 10 + 20; }") == "int");
        CHECK(DeduceTypeInMain("void main() { 10 + 2.5f; }") == "float");
        CHECK(DeduceTypeInMain("void main() { 2.5f + 3.14; }") == "double");
        CHECK(DeduceTypeInMain("void main() { uint(5) + uint(10); }") == "uint");
        CHECK(DeduceTypeInMain("void main() { int8(1) + int16(2); }") == "int");
    }

    TEST_CASE("Relational and Logical Expressions")
    {
        CHECK(DeduceTypeInMain("void main() { 10 < 20; }") == "bool");
        CHECK(DeduceTypeInMain("void main() { 10 == 20; }") == "bool");
        CHECK(DeduceTypeInMain("void main() { true && false; }") == "bool");
        CHECK(DeduceTypeInMain("void main() { !true; }") == "bool");
    }

    TEST_CASE("String Concatenation")
    {
        std::string code =
            "void main()\n"
            "{\n"
            "    string s = \"hello\";\n"
            "    s + 42;\n"
            "}\n";
        CHECK(DeduceTypeInMain(code) == "string");
    }

    TEST_CASE("User-Defined Operator Overloads")
    {
        std::string code =
            "class Vector2\n"
            "{\n"
            "    float x, y;\n"
            "    Vector2 opAdd(const Vector2 &in other) const { return this; }\n"
            "    Vector2 opMul_r(float scalar) const { return this; }\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    Vector2 v1, v2;\n"
            "    v1 + v2;\n"
            "}\n";
        CHECK(DeduceTypeInMain(code) == "Vector2");

        std::string revCode =
            "class Vector2\n"
            "{\n"
            "    float x, y;\n"
            "    Vector2 opMul_r(float scalar) const { return this; }\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    Vector2 v;\n"
            "    2.5f * v;\n"
            "}\n";
        CHECK(DeduceTypeInMain(revCode) == "Vector2");
    }

    TEST_CASE("Ternary Expressions")
    {
        CHECK(DeduceTypeInMain("void main() { true ? 1 : 2; }") == "int");
        CHECK(DeduceTypeInMain("void main() { true ? 1 : 2.5f; }") == "float");
        CHECK(DeduceTypeInMain("void main() { true ? 1.0f : 2.0; }") == "double");
    }

    TEST_CASE("Method and Property Chaining")
    {
        std::string code =
            "class Ammo\n"
            "{\n"
            "    int count;\n"
            "}\n"
            "class Weapon\n"
            "{\n"
            "    Ammo@ GetAmmo() { return null; }\n"
            "}\n"
            "class Player\n"
            "{\n"
            "    Weapon@ GetWeapon() { return null; }\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    Player p;\n"
            "    p.GetWeapon().GetAmmo().count;\n"
            "}\n";
        CHECK(DeduceTypeInMain(code) == "int");
    }

    TEST_CASE("Template Indexing and Nested Templates")
    {
        std::string code1 =
            "void main()\n"
            "{\n"
            "    array<int> arr;\n"
            "    arr[0];\n"
            "}\n";
        CHECK(DeduceTypeInMain(code1) == "int");

        std::string code2 =
            "void main()\n"
            "{\n"
            "    array<dictionary<string, int>> arr;\n"
            "    arr[0];\n"
            "}\n";
        CHECK(DeduceTypeInMain(code2) == "dictionary<string, int>");

        std::string code3 =
            "void main()\n"
            "{\n"
            "    array<dictionary<string, int>> arr;\n"
            "    arr[0][\"score\"];\n"
            "}\n";
        CHECK(DeduceTypeInMain(code3) == "int");

        std::string code4 =
            "void main()\n"
            "{\n"
            "    array<array<float>> grid;\n"
            "    grid[0][0];\n"
            "}\n";
        CHECK(DeduceTypeInMain(code4) == "float");
    }
}
