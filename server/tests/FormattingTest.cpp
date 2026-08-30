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

    // ---------------------------------------------------------------------------------------
    // Value braces.
    //
    // Every `{` used to go onto its own line, which is right for a block and wrong for everything
    // else: an initializer list and a lambda body are values, and exploding them Allman-style
    // rewrote working code into something nobody writes. These pin the distinction, and the block
    // cases below pin that Allman did not change while it was made.
    // ---------------------------------------------------------------------------------------

    namespace
    {
        std::string Format4(const std::string &code, BraceStyle style = BraceStyle::Allman)
        {
            lsp::FormattingOptions options;
            options.tabSize = 4;
            options.insertSpaces = true;
            return FormatSourceCode(code, options, style);
        }
    }

    TEST_CASE("A list initializer stays on the declaration line")
    {
        CHECK(Format4("void main(){\narray<int> a = {1,2,3};\n}") ==
              "void main()\n{\n    array<int> a = {1, 2, 3};\n}\n");
    }

    TEST_CASE("A list passed as an argument stays inside the call")
    {
        CHECK(Format4("void main(){\nTake({1,2});\n}") ==
              "void main()\n{\n    Take({1, 2});\n}\n");
    }

    TEST_CASE("A returned list stays on the return line")
    {
        CHECK(Format4("array<int> Make(){\nreturn {1,2};\n}") ==
              "array<int> Make()\n{\n    return {1, 2};\n}\n");
    }

    TEST_CASE("A nested list keeps both levels inline")
    {
        CHECK(Format4("void main(){\ndictionary d = {{'a',1},{'b',2}};\n}") ==
              "void main()\n{\n    dictionary d = {{'a', 1}, {'b', 2}};\n}\n");
    }

    TEST_CASE("An omitted list element grows no space where it used to be")
    {
        // `{ 0, 1, , 4 }` compiles - the hole takes the type's default - so the formatter has to
        // carry it through unchanged rather than tidy it into something the compiler reads
        // differently.
        CHECK(Format4("void main(){\narray<int> a = {0,1,,4};\n}") ==
              "void main()\n{\n    array<int> a = {0, 1, , 4};\n}\n");
    }

    TEST_CASE("Two declarators each keep their own list")
    {
        CHECK(Format4("void main(){\narray<int> a = {1}, b = {2};\n}") ==
              "void main()\n{\n    array<int> a = {1}, b = {2};\n}\n");
    }

    TEST_CASE("A lambda passed as an argument keeps its body inline")
    {
        CHECK(Format4("void main(){\nSubscribe(function(int a){ Log(a); });\n}") ==
              "void main()\n{\n    Subscribe(function(int a) { Log(a); });\n}\n");
    }

    TEST_CASE("A block inside a lambda body is still a block")
    {
        // The reason a value brace is measured against the brace that encloses it rather than
        // against parenDepth zero: both of these open at parenDepth 1.
        CHECK(Format4("void main(){\nRun(function(){ if (c) { g(); } });\n}") ==
              "void main()\n{\n    Run(function() { if (c) { g(); } });\n}\n");
    }

    TEST_CASE("An assignment inside an if condition keeps its block brace")
    {
        CHECK(Format4("void main(){\nif (x = Next()) {\ng();\n}\n}") ==
              "void main()\n{\n    if (x = Next())\n    {\n        g();\n    }\n}\n");
    }

    TEST_CASE("A for-loop initializer keeps the body's block brace")
    {
        CHECK(Format4("void main(){\nfor (int i = 0; i < n; i++) {\ng();\n}\n}") ==
              "void main()\n{\n    for (int i = 0; i < n; i++)\n    {\n        g();\n    }\n}\n");
    }

    TEST_CASE("A metadata block keeps its own line")
    {
        // CScriptBuilder strips these before the compiler sees them and the grammar makes the
        // block a sibling of the declaration, so joining it onto the declaration line was wrong
        // twice over.
        CHECK(Format4("class C {\n[Property, Category=\"Weapons\"]\nint damage;\n}") ==
              "class C\n{\n    [Property, Category = \"Weapons\"]\n    int damage;\n}\n");
    }

    TEST_CASE("An index expression is not mistaken for metadata")
    {
        CHECK(Format4("void main(){\narr[0] = 1;\n}") ==
              "void main()\n{\n    arr[0] = 1;\n}\n");
    }

    // ---------------------------------------------------------------------------------------
    // Brace style. Allman is the default and everything above asserts it; these assert that K&R
    // moves the *block* brace and leaves every value brace exactly where it was.
    // ---------------------------------------------------------------------------------------

    TEST_CASE("K&R puts a function's block brace on the signature line")
    {
        CHECK(Format4("void main()\n{\nint a=1;\n}", BraceStyle::KAndR) ==
              "void main() {\n    int a = 1;\n}\n");
    }

    TEST_CASE("K&R puts else beside the brace that closed the if")
    {
        CHECK(Format4("void main(){\nif (c) {\nf();\n} else {\ng();\n}\n}", BraceStyle::KAndR) ==
              "void main() {\n    if (c) {\n        f();\n    } else {\n        g();\n    }\n}\n");
    }

    TEST_CASE("K&R leaves a list initializer exactly where Allman does")
    {
        CHECK(Format4("void main(){\narray<int> a = {1,2,3};\n}", BraceStyle::KAndR) ==
              "void main() {\n    array<int> a = {1, 2, 3};\n}\n");
    }

    TEST_CASE("Allman is what an unspecified style gives")
    {
        const std::string code = "void main(){\nint a=1;\n}";
        CHECK(Format4(code) == Format4(code, BraceStyle::Allman));
    }

    // ---------------------------------------------------------------------------------------
    // Three ways the formatter used to change what a file means, all found by running it over the
    // corpus and comparing the compiler's verdict before and after - see FormatterCorpusTest.cpp.
    // ---------------------------------------------------------------------------------------

    TEST_CASE("!is does not swallow the front of an identifier")
    {
        // The worst of the three: `!isdigit(s)` came out as `!is digit(s)`, which does not
        // compile. Twenty-eight of the 1061 corpus scripts were being rewritten this way.
        // tests/parity/doc_p20_not_is_word_boundary.as has the compiler's answer for both forms.
        CHECK(Format4("void main(){\nif (!isdigit(s)) { f(); }\n}") ==
              "void main()\n{\n    if (!isdigit(s))\n    {\n        f();\n    }\n}\n");

        CHECK(Format4("void main(){\nif (a !is b) { f(); }\n}") ==
              "void main()\n{\n    if (a !is b)\n    {\n        f();\n    }\n}\n");
    }

    TEST_CASE("An unterminated string keeps the line break after it")
    {
        // A plain `"` ends at the line break, matching the default engine. What is left is a
        // literal that ran off its line, and joining the next line onto it pulled that code
        // inside the literal. It is also the state every string is in while it is being typed.
        const std::string formatted = Format4("void main(){\nstring s = \"one\nstring t = \"two\";\n}");
        CHECK(formatted.find("\"one\n") != std::string::npos);
        CHECK(formatted.find("\"onestring") == std::string::npos);
    }

    TEST_CASE("A UTF-8 BOM survives formatting byte for byte")
    {
        // The compiler accepts a BOM and so does the grammar - doc_p14_utf8_bom.as. Tokenized as
        // three stray bytes it came back out as "\xEF \xBB \xBF", and the file stopped compiling.
        const std::string bom = "\xEF\xBB\xBF";
        const std::string formatted = Format4(bom + "void main(){\nint a=1;\n}");
        CHECK(formatted.rfind(bom, 0) == 0);
        CHECK(formatted == bom + "void main()\n{\n    int a = 1;\n}\n");
    }
}


