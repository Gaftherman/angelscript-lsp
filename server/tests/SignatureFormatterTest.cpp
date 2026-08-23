#include <doctest/doctest.h>

#include "analysis/SignatureFormatter.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    /** @brief Collects a snippet and renders the declaration of one qualified symbol. */
    std::string RenderFirst(const std::string &code, const std::string &qualifiedName)
    {
        AngelScriptParser parser;
        SymbolCollector collector{ nullptr };
        SymbolTable table;
        collector.CollectSymbols("file:///fmt.as", code, parser, table);

        const auto symbol = table.FindFirstSymbol(qualifiedName);
        if (!symbol.has_value())
        {
            return "";
        }

        switch (symbol->type)
        {
        case SymbolType::Function:
        case SymbolType::Funcdef:
            return FormatFunctionDeclaration(*symbol);
        case SymbolType::Class:
        case SymbolType::Interface:
            return FormatTypeDeclaration(*symbol);
        case SymbolType::Variable:
            return FormatVariableDeclaration(symbol->GetVariable(), symbol->name);
        default:
            return symbol->name;
        }
    }
}

TEST_CASE("SignatureFormatter - renders parameter reference direction")
{
    const std::string code =
        "class Foo\n"
        "{\n"
        "\tvoid Store(const string &in key, int64 &inout value, bool &out ok);\n"
        "}\n";

    CHECK(RenderFirst(code, "Foo::Store") ==
          "void Foo::Store(const string &in key, int64 &inout value, bool &out ok)");
}

TEST_CASE("SignatureFormatter - renders access modifiers on members")
{
    const std::string code =
        "class Foo\n"
        "{\n"
        "\tprivate void Hidden();\n"
        "\tprotected void Shielded();\n"
        "\tvoid Open();\n"
        "}\n";

    CHECK(RenderFirst(code, "Foo::Hidden") == "private void Foo::Hidden()");
    CHECK(RenderFirst(code, "Foo::Shielded") == "protected void Foo::Shielded()");
    CHECK(RenderFirst(code, "Foo::Open") == "void Foo::Open()");
}

TEST_CASE("SignatureFormatter - renders trailing const and function attributes")
{
    const std::string code =
        "class Foo\n"
        "{\n"
        "\tuint Length() const;\n"
        "\tvoid Tick() override;\n"
        "\tvoid Seal() final;\n"
        "\tvoid Both() const override;\n"
        "}\n";

    CHECK(RenderFirst(code, "Foo::Length") == "uint Foo::Length() const");
    CHECK(RenderFirst(code, "Foo::Tick") == "void Foo::Tick() override");
    CHECK(RenderFirst(code, "Foo::Seal") == "void Foo::Seal() final");
    CHECK(RenderFirst(code, "Foo::Both") == "void Foo::Both() const override");
}

TEST_CASE("SignatureFormatter - keeps handles and const on types")
{
    const std::string code =
        "class Foo\n"
        "{\n"
        "\tprivate Foo@ m_next;\n"
        "\tprotected const string m_name;\n"
        "\tFoo@ Clone() const;\n"
        "\tvoid Adopt(const Foo@ &in other);\n"
        "}\n"
        "Foo@ g_active;\n";

    CHECK(RenderFirst(code, "Foo::m_next") == "private Foo@ m_next");
    CHECK(RenderFirst(code, "Foo::m_name") == "protected const string m_name");
    CHECK(RenderFirst(code, "Foo::Clone") == "Foo@ Foo::Clone() const");
    CHECK(RenderFirst(code, "Foo::Adopt") == "void Foo::Adopt(const Foo@ &in other)");
    CHECK(RenderFirst(code, "g_active") == "Foo@ g_active");
}

TEST_CASE("SignatureFormatter - renders by-reference returns")
{
    const std::string code =
        "class Foo\n"
        "{\n"
        "\tconst string& GetName();\n"
        "\tFoo& opAssign(const Foo &in other);\n"
        "}\n";

    CHECK(RenderFirst(code, "Foo::GetName") == "const string& Foo::GetName()");
    CHECK(RenderFirst(code, "Foo::opAssign") == "Foo& Foo::opAssign(const Foo &in other)");
}

TEST_CASE("SignatureFormatter - renders default argument values")
{
    const std::string code =
        "void Sort(uint startAt = 0, uint count = uint(-1));\n";

    CHECK(RenderFirst(code, "Sort") == "void Sort(uint startAt = 0, uint count = uint(-1))");
}

TEST_CASE("SignatureFormatter - renders class declaration modifiers")
{
    const std::string code =
        "shared abstract class Base {}\n"
        "final class Leaf : Base {}\n"
        "mixin class Helper {}\n"
        "interface IThing {}\n";

    CHECK(RenderFirst(code, "Base") == "shared abstract class Base");
    CHECK(RenderFirst(code, "Leaf") == "final class Leaf : Base");
    CHECK(RenderFirst(code, "Helper") == "mixin class Helper");
    CHECK(RenderFirst(code, "IThing") == "interface IThing");
}

TEST_CASE("SignatureFormatter - renders funcdef parameters with modifiers")
{
    const std::string code =
        "funcdef bool less(const ?&in a, const ?&in b);\n";

    CHECK(RenderFirst(code, "less") == "funcdef bool less(const ? &in a, const ? &in b)");
}

TEST_CASE("SignatureFormatter - const and handle flags survive an unmarked type name")
{
    ParameterInformation param;
    param.typeName = "Foo";
    param.name = "value";
    param.isConst = true;
    param.isHandle = true;
    param.modifier = ParameterModifier::In;

    CHECK(FormatParameter(param) == "const Foo@ &in value");

    // An already-marked type text must not gain a second 'const' or '@'.
    param.typeName = "const Foo@";
    CHECK(FormatParameter(param) == "const Foo@ &in value");
}

TEST_CASE("SignatureFormatter - renders a virtual property's accessor block")
{
    // A virtual property reaches the formatter as a VariableSignature, so it used to render as
    // `string Name` - nothing said it was a property, let alone which half of it existed. The
    // collector had the accessors all along.
    VariableSignature property;
    property.typeName = "string";
    property.isVirtualProperty = true;
    property.hasGet = true;
    property.isGetConst = true;
    property.hasSet = true;

    CHECK(FormatVariableDeclaration(property, "Name") == "string Name { get const; set; }");
}

TEST_CASE("SignatureFormatter - renders override and final on an accessor")
{
    VariableSignature property;
    property.typeName = "int";
    property.isVirtualProperty = true;
    property.hasGet = true;
    property.isGetOverride = true;
    property.hasSet = true;
    property.isSetFinal = true;

    CHECK(FormatVariableDeclaration(property, "Health") == "int Health { get override; set final; }");
}

TEST_CASE("SignatureFormatter - a get-only property renders only its get")
{
    VariableSignature property;
    property.typeName = "string";
    property.isVirtualProperty = true;
    property.hasGet = true;

    CHECK(FormatVariableDeclaration(property, "Name") == "string Name { get; }");
}

TEST_CASE("SignatureFormatter - an ordinary field gains no accessor block")
{
    VariableSignature field;
    field.typeName = "string";
    field.modifiers.access = AccessModifier::Private;

    CHECK(FormatVariableDeclaration(field, "m_name") == "private string m_name");
}

TEST_CASE("SignatureFormatter - undirected reference still renders an ampersand")
{
    ParameterInformation param;
    param.typeName = "int";
    param.name = "n";
    param.isReference = true;
    param.modifier = ParameterModifier::None;

    CHECK(FormatParameter(param) == "int & n");
}
