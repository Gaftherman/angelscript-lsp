#include <doctest/doctest.h>
#include "features/formatting/FormattingHandler.h"
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;

TEST_SUITE("Formatting")
{
    TEST_CASE("IndentWithFourSpaces")
    {
        std::string code = "void main(){\nint a=1;\n}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected = "void main()\n{\n    int a = 1;\n}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("IndentWithTwoSpaces")
    {
        std::string code = "void main(){\nint a=1;\n}";
        lsp::FormattingOptions options;
        options.tabSize = 2;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected = "void main()\n{\n  int a = 1;\n}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("IndentWithTabs")
    {
        std::string code = "void main(){\nint a=1;\n}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = false;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected = "void main()\n{\n\tint a = 1;\n}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("AllmanBraceAlignmentForFunctionsAndControlFlow")
    {
        std::string code = "void test(){if(true){doWork();}}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected = 
            "void test()\n"
            "{\n"
            "    if (true)\n"
            "    {\n"
            "        doWork();\n"
            "    }\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("AllmanBraceAlignmentForIfElseLadder")
    {
        std::string code = "if(x>0){a();}else if(x<0){b();}else{c();}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "if (x > 0)\n"
            "{\n"
            "    a();\n"
            "}\n"
            "else if (x < 0)\n"
            "{\n"
            "    b();\n"
            "}\n"
            "else\n"
            "{\n"
            "    c();\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("AllmanBraceAlignmentForClassAndMethods")
    {
        std::string code = "class Bar:IFoo,IBar{int m_val;void DoAction(float dt){if(dt>0.0f){m_val+=1;}else{m_val=0;}}}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "class Bar : IFoo, IBar\n"
            "{\n"
            "    int m_val;\n"
            "    void DoAction(float dt)\n"
            "    {\n"
            "        if (dt > 0.0f)\n"
            "        {\n"
            "            m_val += 1;\n"
            "        }\n"
            "        else\n"
            "        {\n"
            "            m_val = 0;\n"
            "        }\n"
            "    }\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("AllmanBraceAlignmentForEnumAndNamespace")
    {
        std::string code = "namespace Game{enum State{Idle,Running=1,Paused};}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "namespace Game\n"
            "{\n"
            "    enum State\n"
            "    {\n"
            "        Idle,\n"
            "        Running = 1,\n"
            "        Paused\n"
            "    };\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("WhitespaceAndOperatorNormalization")
    {
        std::string code = "int x=5+3*2;if(x>0){foo(1,2,3);}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "int x = 5 + 3 * 2;\n"
            "if (x > 0)\n"
            "{\n"
            "    foo(1, 2, 3);\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("ForLoopSemicolonAndIncrement")
    {
        std::string code = "for(int i=0;i<10;++i){sum+=i;}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "for (int i = 0; i < 10; ++i)\n"
            "{\n"
            "    sum += i;\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("UnaryAndTernaryOperators")
    {
        std::string code = "int a=-5;int b=+10;bool c=!flag;int res=c?a:b;";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "int a = -5;\n"
            "int b = +10;\n"
            "bool c = !flag;\n"
            "int res = c ? a : b;\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("TemplatesAndHandles")
    {
        std::string code = "array<int>@ arr=null;dictionary<string,int> dict;";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "array<int>@ arr = null;\n"
            "dictionary<string, int> dict;\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("SwitchCaseFormatting")
    {
        std::string code = "switch(state){case 0:return;case 1:{int x=1;break;}default:break;}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "switch (state)\n"
            "{\n"
            "    case 0:\n"
            "        return;\n"
            "    case 1:\n"
            "    {\n"
            "        int x = 1;\n"
            "        break;\n"
            "    }\n"
            "    default:\n"
            "        break;\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("CommentsAndStringsPreservation")
    {
        std::string code = 
            "// Header comment\n"
            "void foo() // inline comment   \n"
            "{\n"
            "    string s = \"int x = 1 + 2; { not a block }\";\n"
            "    int a = 1;   \n"
            "}\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;
        options.trimTrailingWhitespace = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "// Header comment\n"
            "void foo() // inline comment\n"
            "{\n"
            "    string s = \"int x = 1 + 2; { not a block }\";\n"
            "    int a = 1;\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("PreprocessorDirectivesAtColumnZero")
    {
        std::string code = "#include \"header.as\"\n#define MAX 100\nvoid main(){}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "#include \"header.as\"\n"
            "#define MAX 100\n"
            "void main()\n"
            "{\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("TrailingWhitespaceAndNewlinesCleanup")
    {
        std::string code = "void test() {   \n    int x = 10;   \n}   \n\n\n";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;
        options.trimTrailingWhitespace = true;
        options.insertFinalNewline = true;
        options.trimFinalNewlines = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected =
            "void test()\n"
            "{\n"
            "    int x = 10;\n"
            "}\n";
        CHECK(formatted == expected);
    }

    TEST_CASE("FormatDocumentAndFormatRangeAPI")
    {
        std::string uri = "file:///test.as";
        std::string code = "void test(){int x=1;}void other(){int y=2;}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        FormattingRequest docReq{ uri, code, nullptr, options };
        auto docEdits = FormatDocument(docReq);
        REQUIRE(docEdits.has_value());
        REQUIRE(!docEdits->empty());
        CHECK(docEdits->size() == 1);
        CHECK((*docEdits)[0].newText == "void test()\n{\n    int x = 1;\n}\nvoid other()\n{\n    int y = 2;\n}\n");

        RangeFormattingRequest rangeReq{ uri, code, nullptr, lsp::Range{ { 0, 0 }, { 0, 10 } }, options };
        auto rangeEdits = FormatRange(rangeReq);
        REQUIRE(rangeEdits.has_value());
    }

    TEST_CASE("FormatOnTypeAPI")
    {
        std::string uri = "file:///test.as";
        std::string code = "void test()\n{\n    int x = 1;\n}\n";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        OnTypeFormattingRequest req{ uri, code, nullptr, lsp::Position{ 2, 14 }, ";", options };
        auto edits = FormatOnType(req);
        REQUIRE(edits.has_value());
    }

    TEST_CASE("FormatOnTypeClosingBrace")
    {
        std::string uri = "file:///test.as";
        std::string code = "void test(){\nint x=1;\n}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        OnTypeFormattingRequest req{ uri, code, nullptr, lsp::Position{ 2, 1 }, "}", options };
        auto edits = FormatOnType(req);
        REQUIRE(edits.has_value());
    }

    TEST_CASE("EmptyDocumentRobustness")
    {
        std::string uri = "file:///empty.as";
        std::string code = "";
        lsp::FormattingOptions options;

        FormattingRequest docReq{ uri, code, nullptr, options };
        auto docEdits = FormatDocument(docReq);
        REQUIRE(docEdits.has_value());
        CHECK(docEdits->empty());

        OnTypeFormattingRequest onTypeReq{ uri, code, nullptr, lsp::Position{ 0, 0 }, ";", options };
        auto onTypeEdits = FormatOnType(onTypeReq);
        CHECK(!onTypeEdits.has_value());
    }
}


