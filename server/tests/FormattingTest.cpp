#include "features/formatting/ClangFormatReader.h"
#include "features/formatting/FormattingHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include <doctest/doctest.h>
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
        std::string expected = "void test()\n"
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
        std::string expected = "if (x > 0)\n"
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
        std::string code =
            "class Bar:IFoo,IBar{int m_val;void DoAction(float dt){if(dt>0.0f){m_val+=1;}else{m_val=0;}}}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        std::string expected = "class Bar : IFoo, IBar\n"
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
        std::string expected = "namespace Game\n"
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
        std::string expected = "int x = 5 + 3 * 2;\n"
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
        std::string expected = "for (int i = 0; i < 10; ++i)\n"
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
        std::string expected = "int a = -5;\n"
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
        std::string expected = "array<int>@ arr = null;\n"
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
        std::string expected = "switch (state)\n"
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
        std::string code = "// Header comment\n"
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
        std::string expected = "// Header comment\n"
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
        std::string expected = "#include \"header.as\"\n"
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
        std::string expected = "void test()\n"
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

        FormattingRequest docReq{uri, code, nullptr, options};
        auto docEdits = FormatDocument(docReq);
        REQUIRE(docEdits.has_value());
        REQUIRE(!docEdits->empty());
        CHECK(docEdits->size() == 1);
        CHECK((*docEdits)[0].newText == "void test()\n{\n    int x = 1;\n}\nvoid other()\n{\n    int y = 2;\n}\n");

        RangeFormattingRequest rangeReq{uri, code, nullptr, lsp::Range{{0, 0}, {0, 10}}, options};
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

        OnTypeFormattingRequest req{uri, code, nullptr, lsp::Position{2, 14}, ";", options};
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

        OnTypeFormattingRequest req{uri, code, nullptr, lsp::Position{2, 1}, "}", options};
        auto edits = FormatOnType(req);
        REQUIRE(edits.has_value());
    }

    TEST_CASE("EmptyDocumentRobustness")
    {
        std::string uri = "file:///empty.as";
        std::string code = "";
        lsp::FormattingOptions options;

        FormattingRequest docReq{uri, code, nullptr, options};
        auto docEdits = FormatDocument(docReq);
        REQUIRE(docEdits.has_value());
        CHECK(docEdits->empty());

        OnTypeFormattingRequest onTypeReq{uri, code, nullptr, lsp::Position{0, 0}, ";", options};
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
    std::string Format4(const std::string& code, BraceStyle style = BraceStyle::Allman)
    {
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;
        return FormatSourceCode(code, options, style);
    }
    } // namespace

    TEST_CASE("A list initializer stays on the declaration line")
    {
        CHECK(Format4("void main(){\narray<int> a = {1,2,3};\n}") ==
              "void main()\n{\n    array<int> a = {1, 2, 3};\n}\n");
    }

    TEST_CASE("A list passed as an argument stays inside the call")
    {
        CHECK(Format4("void main(){\nTake({1,2});\n}") == "void main()\n{\n    Take({1, 2});\n}\n");
    }

    TEST_CASE("A returned list stays on the return line")
    {
        CHECK(Format4("array<int> Make(){\nreturn {1,2};\n}") == "array<int> Make()\n{\n    return {1, 2};\n}\n");
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
        CHECK(Format4("void main(){\narr[0] = 1;\n}") == "void main()\n{\n    arr[0] = 1;\n}\n");
    }

    // ---------------------------------------------------------------------------------------
    // Brace style. Allman is the default and everything above asserts it; these assert that K&R
    // moves the *block* brace and leaves every value brace exactly where it was.
    // ---------------------------------------------------------------------------------------

    TEST_CASE("K&R puts a function's block brace on the signature line")
    {
        CHECK(Format4("void main()\n{\nint a=1;\n}", BraceStyle::KAndR) == "void main() {\n    int a = 1;\n}\n");
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

    TEST_CASE("FormatRangePreservesLineAlignmentAcrossBraceShifts")
    {
        const std::string fnPre1 = angel_lsp::test::GenerateRandomSymbolName("PreFuncA");
        const std::string fnPre2 = angel_lsp::test::GenerateRandomSymbolName("PreFuncB");
        const std::string fnTarget = angel_lsp::test::GenerateRandomSymbolName("TargetFunc");
        const std::string fnPost = angel_lsp::test::GenerateRandomSymbolName("PostFunc");
        const std::string flagVar = angel_lsp::test::GenerateRandomSymbolName("flag");

        std::string code = "void " + fnPre1 +
                           "() {\n"
                           "    return;\n"
                           "}\n"
                           "void " +
                           fnPre2 +
                           "() {\n"
                           "    return;\n"
                           "}\n"
                           "bool " +
                           fnTarget +
                           "()\n"
                           "{\n"
                           "return false;\n"
                           "}\n"
                           "void " +
                           fnPost +
                           "()\n"
                           "{\n"
                           "    if (" +
                           flagVar +
                           " == 1) return;\n"
                           "}\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        RangeFormattingRequest req{"file:///test.as", code, nullptr, lsp::Range{{8, 0}, {8, 13}}, options};
        auto edits = FormatRange(req);

        REQUIRE(edits.has_value());
        REQUIRE(!edits->empty());
        CHECK(edits->size() == 1);
        CHECK((*edits)[0].range.start.line == 8);
        CHECK((*edits)[0].range.end.line == 8);
        CHECK((*edits)[0].newText == "    return false;");
        CHECK((*edits)[0].newText.find(fnPost) == std::string::npos);
        CHECK((*edits)[0].newText.find(flagVar) == std::string::npos);
    }

    TEST_CASE("FormatRangeReturnsEmptyWhenAlreadyFormatted")
    {
        const std::string fnPre = angel_lsp::test::GenerateRandomSymbolName("FuncKAndR");
        const std::string fnTarget = angel_lsp::test::GenerateRandomSymbolName("FuncTarget");

        std::string code = "void " + fnPre +
                           "() {\n"
                           "    return;\n"
                           "}\n"
                           "void " +
                           fnTarget +
                           "()\n"
                           "{\n"
                           "    return;\n"
                           "}\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        RangeFormattingRequest req{"file:///test.as", code, nullptr, lsp::Range{{5, 0}, {5, 11}}, options};
        auto edits = FormatRange(req);

        REQUIRE(edits.has_value());
        CHECK(edits->empty());
    }

    TEST_CASE("FormatOnTypeRejectsNewlineTrigger")
    {
        std::string code = "void main()\n{\n    return;\n}\n";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        OnTypeFormattingRequest req{"file:///test.as", code, nullptr, lsp::Position{2, 11}, "\n", options};
        auto edits = FormatOnType(req);
        CHECK(!edits.has_value());
    }

    TEST_CASE("FormatOnTypeSemicolonFormatsOnlyTargetStatement")
    {
        const std::string fnPre = angel_lsp::test::GenerateRandomSymbolName("FuncKAndR");
        const std::string fnTarget = angel_lsp::test::GenerateRandomSymbolName("FuncTarget");
        const std::string varName = angel_lsp::test::GenerateRandomSymbolName("localVar");

        std::string code = "void " + fnPre +
                           "() {\n"
                           "    return;\n"
                           "}\n"
                           "void " +
                           fnTarget +
                           "()\n"
                           "{\n"
                           "int " +
                           varName +
                           "=42;\n"
                           "}\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        OnTypeFormattingRequest req{"file:///test.as", code, nullptr, lsp::Position{5, 15}, ";", options};
        auto edits = FormatOnType(req);

        REQUIRE(edits.has_value());
        REQUIRE(!edits->empty());
        CHECK(edits->size() == 1);
        CHECK((*edits)[0].range.start.line == 5);
        CHECK((*edits)[0].range.end.line == 5);
        CHECK((*edits)[0].newText == "    int " + varName + " = 42;");
    }

    TEST_CASE("SpacesInsideParentheses - Document formatting formats with inner spaces when enabled")
    {
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("FuncParen");
        const std::string arg1 = angel_lsp::test::GenerateRandomSymbolName("paramA");
        const std::string arg2 = angel_lsp::test::GenerateRandomSymbolName("paramB");

        std::string code =
            "void " + fnName + "(int " + arg1 + ",float " + arg2 + "){\nif(true){\n" + fnName + "(1,2);\n}\n}\n";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formattedWithSpaces = FormatSourceCode(code, options, BraceStyle::Allman, true);
        std::string expectedWithSpaces = "void " + fnName + "( int " + arg1 + ", float " + arg2 +
                                         " )\n"
                                         "{\n"
                                         "    if ( true )\n"
                                         "    {\n"
                                         "        " +
                                         fnName +
                                         "( 1, 2 );\n"
                                         "    }\n"
                                         "}\n";
        CHECK(formattedWithSpaces == expectedWithSpaces);

        // Empty parens should never have space inside: ()
        const std::string emptyFn = angel_lsp::test::GenerateRandomSymbolName("EmptyCall");
        std::string emptyCode = "void " + emptyFn + "(){\n" + emptyFn + "();\n}\n";
        std::string formattedEmpty = FormatSourceCode(emptyCode, options, BraceStyle::Allman, true);
        std::string expectedEmpty = "void " + emptyFn +
                                    "()\n"
                                    "{\n"
                                    "    " +
                                    emptyFn +
                                    "();\n"
                                    "}\n";
        CHECK(formattedEmpty == expectedEmpty);

        // Standard formatting (default false) must remain compact without inner spaces
        std::string formattedDefault = FormatSourceCode(code, options, BraceStyle::Allman, false);
        std::string expectedDefault = "void " + fnName + "(int " + arg1 + ", float " + arg2 +
                                      ")\n"
                                      "{\n"
                                      "    if (true)\n"
                                      "    {\n"
                                      "        " +
                                      fnName +
                                      "(1, 2);\n"
                                      "    }\n"
                                      "}\n";
        CHECK(formattedDefault == expectedDefault);
    }

    TEST_CASE("SpacesInsideParentheses - FormatOnType preserves spaces inside parentheses")
    {
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Caller");
        const std::string callee = angel_lsp::test::GenerateRandomSymbolName("TargetCall");
        const std::string varA = angel_lsp::test::GenerateRandomSymbolName("valA");
        const std::string varB = angel_lsp::test::GenerateRandomSymbolName("valB");

        std::string code = "void " + fnName +
                           "()\n"
                           "{\n"
                           "    " +
                           callee + "( " + varA + ", " + varB +
                           " );\n"
                           "}\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        OnTypeFormattingRequest req{"file:///test.as",  code, nullptr, lsp::Position{2, 25}, ";", options,
                                    BraceStyle::Allman, true};
        auto edits = FormatOnType(req);

        // When formatted with spacesInsideParentheses = true, text already matches expected format
        // so no spurious changes that collapse spaces occur.
        if (edits.has_value() && !edits->empty())
        {
            CHECK((*edits)[0].newText.find("( " + varA) != std::string::npos);
            CHECK((*edits)[0].newText.find(varB + " )") != std::string::npos);
        }
    }

    TEST_CASE("KeepEmptyBlocksOnSingleLine - FormatSourceCode preserves empty function and constructor blocks")
    {
        const std::string className = angel_lsp::test::GenerateRandomSymbolName("Weapon");
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Init");

        std::string code = "class " + className +
                           "\n"
                           "{\n"
                           "    " +
                           className +
                           "() {}\n"
                           "    void " +
                           fnName +
                           "() {}\n"
                           "};\n";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        FormatCodeOptions opts{options, BraceStyle::Allman, false, true};
        std::string formatted = FormatSourceCode(code, opts);

        CHECK(formatted.find(className + "() {}") != std::string::npos);
        CHECK(formatted.find("void " + fnName + "() {}") != std::string::npos);

        FormatCodeOptions legacyOpts{options, BraceStyle::Allman, false, false};
        std::string expanded = FormatSourceCode(code, legacyOpts);
        CHECK(expanded.find(className + "()\n    {\n    }") != std::string::npos);
    }

    TEST_CASE("FormatOnType - Rejects Tree With Syntax Error")
    {
        parser::AngelScriptParser parser;
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("BadSyntax");
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    this.Use(null, null USE_TOGGLE, 0.0f);:\n"
                                 "}\n";

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        CHECK(ts_node_has_error(ts_tree_root_node(tree)));

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        OnTypeFormattingRequest req{"file:///test.as",  code, tree, lsp::Position{1, 43}, ";", options,
                                    BraceStyle::Allman, false};
        auto edits = FormatOnType(req);
        CHECK_FALSE(edits.has_value());

        ts_tree_delete(tree);
    }

    TEST_CASE("ForeachControlKeywordSpacing")
    {
        const std::string itemVar = test::GenerateRandomSymbolName("item");
        const std::string listVar = test::GenerateRandomSymbolName("list");
        std::string code = "void main() { foreach(auto " + itemVar + " : " + listVar + ") { int x = 1; } }";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        CHECK(formatted.find("foreach (auto " + itemVar + " : " + listVar + ")") != std::string::npos);
    }

    TEST_CASE("PrecisionHandleDeclarationsAndPointerAlignment")
    {
        const std::string typeName = test::GenerateRandomSymbolName("Person");
        const std::string varName = test::GenerateRandomSymbolName("p");
        std::string code = typeName + " @ " + varName + " = null;";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        // Default: Left alignment ("Type@ var")
        FormatCodeOptions leftOpts{options, BraceStyle::Allman, false, false, PointerAlignment::Left};
        std::string leftFormatted = FormatSourceCode(code, leftOpts);
        CHECK(leftFormatted == typeName + "@ " + varName + " = null;\n");

        // Right alignment ("Type @var")
        FormatCodeOptions rightOpts{options, BraceStyle::Allman, false, false, PointerAlignment::Right};
        std::string rightFormatted = FormatSourceCode(code, rightOpts);
        CHECK(rightFormatted == typeName + " @" + varName + " = null;\n");

        // Middle alignment ("Type @ var")
        FormatCodeOptions midOpts{options, BraceStyle::Allman, false, false, PointerAlignment::Middle};
        std::string midFormatted = FormatSourceCode(code, midOpts);
        CHECK(midFormatted == typeName + " @ " + varName + " = null;\n");
    }

    TEST_CASE("UnaryHandleOperatorFormatting")
    {
        const std::string varA = test::GenerateRandomSymbolName("a");
        const std::string varB = test::GenerateRandomSymbolName("b");
        std::string code = "void test() { @ " + varA + " = @ " + varB + "; if ( @ " + varA + " == @ " + varB +
                           " ) return @ " + varA + "; }";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        // Unary handle operator '@' must have NO space after it: "@a = @b;"
        CHECK(formatted.find("@" + varA + " = @" + varB + ";") != std::string::npos);
        CHECK(formatted.find("if (@" + varA + " == @" + varB + ")") != std::string::npos);
        CHECK(formatted.find("return @" + varA + ";") != std::string::npos);
    }

    TEST_CASE("TemplateCastAndConsecutiveHandles")
    {
        const std::string typeName = test::GenerateRandomSymbolName("TargetType");
        const std::string objVar = test::GenerateRandomSymbolName("obj");
        std::string code = typeName + "@@ h; " + typeName + "@ res = cast< " + typeName + "@ > ( " + objVar + " );";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        // Consecutive handles: Type@@ h;
        CHECK(formatted.find(typeName + "@@ h;") != std::string::npos);
        // Template cast: cast<Type@>(obj) without extra space before '>' or '('
        CHECK(formatted.find("cast<" + typeName + "@>(" + objVar + ")") != std::string::npos);
    }

    TEST_CASE("ClangFormatConfigParsing")
    {
        std::string yamlConfig = "# Clang format config\n"
                                 "BasedOnStyle: LLVM\n"
                                 "IndentWidth: 2\n"
                                 "UseTab: Always\n"
                                 "BreakBeforeBraces: Attach\n"
                                 "PointerAlignment: Right\n"
                                 "SpacesInParentheses: true\n"
                                 "AllowShortBlocksOnASingleLine: false\n";

        auto style = ParseClangFormat(yamlConfig);
        REQUIRE(style.has_value());
        CHECK(style->indentWidth == 2);
        CHECK(style->useTab == UseTabStyle::Always);
        CHECK(style->braceStyle == BraceStyle::KAndR);
        CHECK(style->pointerAlignment == PointerAlignment::Right);
        CHECK(style->spacesInsideParentheses == true);
        CHECK(style->allowShortBlocksOnASingleLine == ShortBlockStyle::Never);

        FormatCodeOptions opts;
        ApplyClangFormatStyle(opts, *style);
        CHECK(opts.options.tabSize == 2);
        CHECK(opts.options.insertSpaces == false);
        CHECK(opts.braceStyle == BraceStyle::KAndR);
        CHECK(opts.pointerAlignment == PointerAlignment::Right);
        CHECK(opts.spacesInsideParentheses == true);
        CHECK(opts.allowShortBlocksOnASingleLine == ShortBlockStyle::Never);
    }

    TEST_CASE("ClangFormatPresetsTest")
    {
        auto msStyle = ParseClangFormat("BasedOnStyle: Microsoft\n");
        REQUIRE(msStyle.has_value());
        CHECK(msStyle->braceStyle == BraceStyle::Allman);
        CHECK(msStyle->indentWidth == 4);
        CHECK(msStyle->pointerAlignment == PointerAlignment::Left);
        CHECK(msStyle->namespaceIndentation == NamespaceIndentationStyle::All);

        auto googleStyle = ParseClangFormat("BasedOnStyle: Google\n");
        REQUIRE(googleStyle.has_value());
        CHECK(googleStyle->braceStyle == BraceStyle::KAndR);
        CHECK(googleStyle->indentWidth == 2);
        CHECK(googleStyle->pointerAlignment == PointerAlignment::Left);
        CHECK(googleStyle->indentCaseLabels == true);

        auto gnuStyle = ParseClangFormat("BasedOnStyle: GNU\n");
        REQUIRE(gnuStyle.has_value());
        CHECK(gnuStyle->braceStyle == BraceStyle::GNU);
        CHECK(gnuStyle->spaceBeforeParens == SpaceBeforeParensStyle::Always);
    }

    TEST_CASE("MultiLanguageClangFormatConfigTest")
    {
        std::string multiDoc = "BasedOnStyle: LLVM\n"
                               "IndentWidth: 2\n"
                               "---\n"
                               "Language: Cpp\n"
                               "IndentWidth: 4\n"
                               "PointerAlignment: Left\n"
                               "---\n"
                               "Language: JavaScript\n"
                               "IndentWidth: 8\n";
        auto style = ParseClangFormat(multiDoc);
        REQUIRE(style.has_value());
        CHECK(style->indentWidth == 4);
        CHECK(style->pointerAlignment == PointerAlignment::Left);

        std::string withAs = multiDoc + "---\nLanguage: AngelScript\nIndentWidth: 3\nPointerAlignment: Right\n";
        auto asStyle = ParseClangFormat(withAs);
        REQUIRE(asStyle.has_value());
        CHECK(asStyle->indentWidth == 3);
        CHECK(asStyle->pointerAlignment == PointerAlignment::Right);
    }

    TEST_CASE("ClangFormatReader - Real-world Legacy ClangFormat Gist Ingestion")
    {
        const std::string gistYaml = "TabWidth: 4\n"
                                     "IndentWidth: 4\n"
                                     "UseTab: Never\n"
                                     "Language: Cpp\n"
                                     "Standard: Cpp11\n"
                                     "PointerAlignment: Left\n"
                                     "AccessModifierOffset: 0\n"
                                     "BreakBeforeBraces: Attach\n"
                                     "ColumnLimit: 80\n"
                                     "SpaceBeforeParens: Always\n"
                                     "SpaceAfterCStyleCast: false\n"
                                     "SpacesInAngles: false\n"
                                     "SpacesInParentheses: false\n"
                                     "SpacesInSquareBrackets: false\n"
                                     "MaxEmptyLinesToKeep: 2\n"
                                     "DerivePointerBinding: false\n"
                                     "AllowShortBlocksOnASingleLine: false\n"
                                     "AllowShortFunctionsOnASingleLine: false\n";

        auto style = ParseClangFormat(gistYaml);
        REQUIRE(style.has_value());
        CHECK(style->tabWidth == 4);
        CHECK(style->indentWidth == 4);
        CHECK(style->useTab == UseTabStyle::Never);
        CHECK(style->braceStyle == BraceStyle::KAndR);
        CHECK(style->pointerAlignment == PointerAlignment::Left);
        CHECK(style->spaceBeforeParens == SpaceBeforeParensStyle::Always);
        CHECK(style->columnLimit == 80);
        CHECK(style->spacesInsideParentheses == false);
        CHECK(style->allowShortBlocksOnASingleLine == ShortBlockStyle::Never);
        CHECK(style->allowShortFunctionsOnASingleLine == ShortFunctionStyle::None);
    }

    TEST_CASE("CombinatorialBraceStylesTest")
    {
        const std::string fnName = test::GenerateRandomSymbolName("func");
        std::string code = "void " + fnName + "() { if (true) { int a = 1; } else { int b = 2; } }";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        FormatCodeOptions allmanOpts;
        allmanOpts.options = options;
        allmanOpts.braceStyle = BraceStyle::Allman;
        std::string allman = FormatSourceCode(code, allmanOpts);
        CHECK(allman.find("void " + fnName + "()\n{\n") != std::string::npos);
        CHECK(allman.find("    }\n    else\n    {\n") != std::string::npos);

        FormatCodeOptions krOpts;
        krOpts.options = options;
        krOpts.braceStyle = BraceStyle::KAndR;
        std::string kr = FormatSourceCode(code, krOpts);
        CHECK(kr.find("void " + fnName + "() {\n") != std::string::npos);
        CHECK(kr.find("} else {\n") != std::string::npos);

        FormatCodeOptions stroustrupOpts;
        stroustrupOpts.options = options;
        stroustrupOpts.braceStyle = BraceStyle::Stroustrup;
        std::string st = FormatSourceCode(code, stroustrupOpts);
        CHECK(st.find("void " + fnName + "() {\n") != std::string::npos);
        CHECK(st.find("    }\n    else {\n") != std::string::npos);
    }

    TEST_CASE("CombinatorialPointerAndReferenceAlignmentTest")
    {
        const std::string typeName = test::GenerateRandomSymbolName("Entity");
        const std::string varName = test::GenerateRandomSymbolName("ent");
        std::string handleCode = typeName + " @ " + varName + ";";
        std::string refCode = "void f(const " + typeName + " & in " + varName + ");";

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        FormatCodeOptions leftOpts;
        leftOpts.options = options;
        leftOpts.pointerAlignment = PointerAlignment::Left;
        leftOpts.referenceAlignment = ReferenceAlignment::Left;
        CHECK(FormatSourceCode(handleCode, leftOpts) == typeName + "@ " + varName + ";\n");
        CHECK(FormatSourceCode(refCode, leftOpts).find("const " + typeName + "& in " + varName) != std::string::npos);

        FormatCodeOptions rightOpts;
        rightOpts.options = options;
        rightOpts.pointerAlignment = PointerAlignment::Right;
        rightOpts.referenceAlignment = ReferenceAlignment::Right;
        CHECK(FormatSourceCode(handleCode, rightOpts) == typeName + " @" + varName + ";\n");
        CHECK(FormatSourceCode(refCode, rightOpts).find("const " + typeName + " &in " + varName) != std::string::npos);

        FormatCodeOptions midOpts;
        midOpts.options = options;
        midOpts.pointerAlignment = PointerAlignment::Middle;
        midOpts.referenceAlignment = ReferenceAlignment::Middle;
        CHECK(FormatSourceCode(handleCode, midOpts) == typeName + " @ " + varName + ";\n");
        CHECK(FormatSourceCode(refCode, midOpts).find("const " + typeName + " & in " + varName) != std::string::npos);
    }

    TEST_CASE("CombinatorialSpacingRulesTest")
    {
        const std::string fn = test::GenerateRandomSymbolName("calc");
        std::string code = "void " + fn + "(int a, int b) { if(a == b) { array<int> arr = {1, 2}; int x = arr[0]; } }";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        FormatCodeOptions spaceOpts;
        spaceOpts.options = options;
        spaceOpts.spacesInsideParentheses = true;
        spaceOpts.spacesInSquareBrackets = true;
        spaceOpts.spacesInAngles = true;
        spaceOpts.spaceBeforeParens = SpaceBeforeParensStyle::Never;
        std::string sp = FormatSourceCode(code, spaceOpts);
        CHECK(sp.find("if( a == b )") != std::string::npos);
        CHECK(sp.find("array< int >") != std::string::npos);
        CHECK(sp.find("arr[ 0 ]") != std::string::npos);

        FormatCodeOptions alwaysOpts;
        alwaysOpts.options = options;
        alwaysOpts.spaceBeforeParens = SpaceBeforeParensStyle::Always;
        std::string al = FormatSourceCode("void test() { " + fn + "(1, 2); }", alwaysOpts);
        CHECK(al.find(fn + " (1, 2);") != std::string::npos);
    }

    TEST_CASE("SwitchCaseAndAccessModifierOffsetTest")
    {
        std::string code = "class MyClass { public: int x; private: int y; };";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        FormatCodeOptions defOpts;
        defOpts.options = options;
        defOpts.accessModifierOffset = -4;
        std::string defOut = FormatSourceCode(code, defOpts);
        CHECK(defOut.find("\npublic:\n") != std::string::npos);

        FormatCodeOptions zeroOpts;
        zeroOpts.options = options;
        zeroOpts.accessModifierOffset = 0;
        std::string zeroOut = FormatSourceCode(code, zeroOpts);
        CHECK(zeroOut.find("\n    public:\n") != std::string::npos);
    }

    TEST_CASE("ClangFormatOffOnPragmaTest")
    {
        std::string code = "// clang-format off\n"
                           "void    messy_func  (   int   x  ,int y)   {return;}\n"
                           "// clang-format on\n"
                           "void clean_func(int a,int b){return;}";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        std::string formatted = FormatSourceCode(code, options);
        CHECK(formatted.find("void    messy_func  (   int   x  ,int y)   {return;}") != std::string::npos);
        CHECK(formatted.find("void clean_func(int a, int b)") != std::string::npos);
    }

    TEST_CASE("MaxEmptyLinesAndTrailingCommentsTest")
    {
        std::string code = "int a = 1; // comment 1\n\n\n\n\nint b = 2;";
        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        FormatCodeOptions opts;
        opts.options = options;
        opts.maxEmptyLinesToKeep = 1;
        opts.spacesBeforeTrailingComments = 3;
        std::string formatted = FormatSourceCode(code, opts);
        CHECK(formatted.find("int a = 1;   // comment 1") != std::string::npos);
        CHECK(formatted.find("\n\nint b = 2;") != std::string::npos);
        CHECK(formatted.find("\n\n\nint b = 2;") == std::string::npos);
    }
}
