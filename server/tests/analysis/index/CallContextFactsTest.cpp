#include "analysis/index/CallContextFacts.h"

#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/NodeIndex.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"

#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

TEST_SUITE("analysis/index/CallContextFacts")
{
    TEST_CASE("CallContextFacts - Direct, Member, Qualified and Constructor Calls")
    {
        const std::string fnDirect = angel_lsp::test::GenerateRandomSymbolName("fnDirect");
        const std::string fnMember = angel_lsp::test::GenerateRandomSymbolName("fnMember");
        const std::string fnQualified = angel_lsp::test::GenerateRandomSymbolName("fnQualified");
        const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("ClsWidget");
        const std::string varName = angel_lsp::test::GenerateRandomSymbolName("wVar");

        const std::string code = "namespace MyNs {\n"
                                 "    void " +
                                 fnQualified +
                                 "(int x, float y) {}\n"
                                 "}\n"
                                 "class " +
                                 clsName +
                                 " {\n"
                                 "    " +
                                 clsName +
                                 "(int a, int b) {}\n"
                                 "    void " +
                                 fnMember +
                                 "(int m1, int m2) {}\n"
                                 "};\n"
                                 "void " +
                                 fnDirect +
                                 "(int d1, int d2) {}\n"
                                 "void main() {\n"
                                 "    " +
                                 fnDirect +
                                 "(10, 20);\n"
                                 "    " +
                                 clsName + " " + varName +
                                 "(30, 40);\n"
                                 "    " +
                                 varName + "." + fnMember +
                                 "(50, 60);\n"
                                 "    MyNs::" +
                                 fnQualified +
                                 "(70, 80);\n"
                                 "}\n";

        angel_lsp::parser::AngelScriptParser parser;
        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});

        // 1. Direct call: fnDirect(10, 20)
        {
            const size_t pos = code.find(fnDirect + "(10");
            REQUIRE(pos != std::string::npos);
            const uint32_t cursor0 = static_cast<uint32_t>(pos + fnDirect.size() + 2); // Inside "10"
            auto ctx0 = angel_lsp::analysis::FindCallContext(index, cursor0);
            REQUIRE(ctx0.has_value());
            CHECK(ctx0->kind == angel_lsp::analysis::CallContextKind::Direct);
            CHECK(ctx0->activeParameter == 0);
            CHECK(angel_lsp::utils::SliceSource(code, ctx0->calleeStartByte, ctx0->calleeEndByte) == fnDirect);

            const size_t posComma = code.find(", 20", pos);
            const uint32_t cursor1 = static_cast<uint32_t>(posComma + 3); // Inside "20"
            auto ctx1 = angel_lsp::analysis::FindCallContext(index, cursor1);
            REQUIRE(ctx1.has_value());
            CHECK(ctx1->activeParameter == 1);
        }

        // 2. Constructor declaration: ClsWidget wVar(30, 40)
        {
            const size_t pos = code.find(varName + "(30");
            REQUIRE(pos != std::string::npos);
            const uint32_t cursor0 = static_cast<uint32_t>(pos + varName.size() + 2); // Inside "30"
            auto ctx0 = angel_lsp::analysis::FindCallContext(index, cursor0);
            REQUIRE(ctx0.has_value());
            CHECK(ctx0->kind == angel_lsp::analysis::CallContextKind::ConstructorDeclaration);
            CHECK(ctx0->activeParameter == 0);
            CHECK(angel_lsp::utils::SliceSource(code, ctx0->calleeStartByte, ctx0->calleeEndByte) == clsName);

            const size_t posComma = code.find(", 40", pos);
            const uint32_t cursor1 = static_cast<uint32_t>(posComma + 3); // Inside "40"
            auto ctx1 = angel_lsp::analysis::FindCallContext(index, cursor1);
            REQUIRE(ctx1.has_value());
            CHECK(ctx1->activeParameter == 1);
        }

        // 3. Member call: wVar.fnMember(50, 60)
        {
            const size_t pos = code.find("." + fnMember + "(50");
            REQUIRE(pos != std::string::npos);
            const uint32_t cursor0 = static_cast<uint32_t>(pos + fnMember.size() + 3); // Inside "50"
            auto ctx0 = angel_lsp::analysis::FindCallContext(index, cursor0);
            REQUIRE(ctx0.has_value());
            CHECK(ctx0->kind == angel_lsp::analysis::CallContextKind::Member);
            CHECK(ctx0->activeParameter == 0);
            CHECK(angel_lsp::utils::SliceSource(code, ctx0->calleeStartByte, ctx0->calleeEndByte) == fnMember);
            CHECK(angel_lsp::utils::SliceSource(code, ctx0->receiverStartByte, ctx0->receiverEndByte) == varName);

            const size_t posComma = code.find(", 60", pos);
            const uint32_t cursor1 = static_cast<uint32_t>(posComma + 3); // Inside "60"
            auto ctx1 = angel_lsp::analysis::FindCallContext(index, cursor1);
            REQUIRE(ctx1.has_value());
            CHECK(ctx1->activeParameter == 1);
        }

        // 4. Qualified call: MyNs::fnQualified(70, 80)
        {
            const size_t pos = code.find("MyNs::" + fnQualified + "(70");
            REQUIRE(pos != std::string::npos);
            const uint32_t cursor0 = static_cast<uint32_t>(pos + 6 + fnQualified.size() + 2); // Inside "70"
            auto ctx0 = angel_lsp::analysis::FindCallContext(index, cursor0);
            REQUIRE(ctx0.has_value());
            CHECK(ctx0->kind == angel_lsp::analysis::CallContextKind::Qualified);
            CHECK(ctx0->activeParameter == 0);
            CHECK(angel_lsp::utils::SliceSource(code, ctx0->calleeStartByte, ctx0->calleeEndByte) == fnQualified);
            CHECK(angel_lsp::utils::SliceSource(code, ctx0->receiverStartByte, ctx0->receiverEndByte) == "MyNs");

            const size_t posComma = code.find(", 80", pos);
            const uint32_t cursor1 = static_cast<uint32_t>(posComma + 3); // Inside "80"
            auto ctx1 = angel_lsp::analysis::FindCallContext(index, cursor1);
            REQUIRE(ctx1.has_value());
            CHECK(ctx1->activeParameter == 1);
        }

        ts_tree_delete(tree);
    }

    TEST_CASE("CallContextFacts - Nested Calls and Inert Separator Scenarios")
    {
        const std::string outerFn = angel_lsp::test::GenerateRandomSymbolName("outerFn");
        const std::string innerFn = angel_lsp::test::GenerateRandomSymbolName("innerFn");

        const std::string code = "void " + outerFn +
                                 "(int a, int b) {}\n"
                                 "int " +
                                 innerFn +
                                 "(int x, int y) { return x + y; }\n"
                                 "void main() {\n"
                                 "    " +
                                 outerFn +
                                 "(\n"
                                 "        " +
                                 innerFn +
                                 "(1, 2),\n"
                                 "        3\n"
                                 "    );\n"
                                 "}\n";

        angel_lsp::parser::AngelScriptParser parser;
        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});

        // Cursor inside innerFn(1, 2) activeParam 0 -> resolves innerFn
        {
            const size_t pos = code.find(innerFn + "(1");
            const uint32_t cursor = static_cast<uint32_t>(pos + innerFn.size() + 2);
            auto ctx = angel_lsp::analysis::FindCallContext(index, cursor);
            REQUIRE(ctx.has_value());
            CHECK(angel_lsp::utils::SliceSource(code, ctx->calleeStartByte, ctx->calleeEndByte) == innerFn);
            CHECK(ctx->activeParameter == 0);
        }

        // Cursor inside innerFn(1, 2) activeParam 1 -> resolves innerFn
        {
            const size_t pos = code.find("2),");
            const uint32_t cursor = static_cast<uint32_t>(pos);
            auto ctx = angel_lsp::analysis::FindCallContext(index, cursor);
            REQUIRE(ctx.has_value());
            CHECK(angel_lsp::utils::SliceSource(code, ctx->calleeStartByte, ctx->calleeEndByte) == innerFn);
            CHECK(ctx->activeParameter == 1);
        }

        // Cursor after innerFn's comma, on argument '3' -> resolves outerFn activeParam 1
        {
            const size_t pos = code.find("3\n");
            const uint32_t cursor = static_cast<uint32_t>(pos);
            auto ctx = angel_lsp::analysis::FindCallContext(index, cursor);
            REQUIRE(ctx.has_value());
            CHECK(angel_lsp::utils::SliceSource(code, ctx->calleeStartByte, ctx->calleeEndByte) == outerFn);
            CHECK(ctx->activeParameter == 1);
        }

        // Cursor after ')' of outer call -> nullopt
        {
            const size_t pos = code.find(");");
            const uint32_t cursorAfter = static_cast<uint32_t>(pos + 2);
            auto ctx = angel_lsp::analysis::FindCallContext(index, cursorAfter);
            CHECK_FALSE(ctx.has_value());
        }

        ts_tree_delete(tree);
    }

    TEST_CASE("CallContextFacts - Separators in Arrays, Braces, Heredocs and Comments Ignored")
    {
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fnComplex");

        const std::string code = "void " + fnName +
                                 "(int p1, int p2) {}\n"
                                 "void main() {\n"
                                 "    " +
                                 fnName +
                                 "(\n"
                                 "        [1, 2, 3], // comma in array\n"
                                 "        \"hello, world!\" /* comma in string */\n"
                                 "    );\n"
                                 "}\n";

        angel_lsp::parser::AngelScriptParser parser;
        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});

        // Cursor inside the array [1, 2, 3] -> activeParameter must still be 0!
        {
            const size_t pos = code.find("2, 3");
            const uint32_t cursor = static_cast<uint32_t>(pos);
            auto ctx = angel_lsp::analysis::FindCallContext(index, cursor);
            REQUIRE(ctx.has_value());
            CHECK(ctx->activeParameter == 0);
        }

        // Cursor inside string literal after second argument -> activeParameter must be 1!
        {
            const size_t pos = code.find("world!");
            const uint32_t cursor = static_cast<uint32_t>(pos);
            auto ctx = angel_lsp::analysis::FindCallContext(index, cursor);
            REQUIRE(ctx.has_value());
            CHECK(ctx->activeParameter == 1);
        }

        ts_tree_delete(tree);
    }

    TEST_CASE("CallContextFacts - ERROR Fallback for Typing In Progress")
    {
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fnUnclosed");
        const std::string code = "void " + fnName +
                                 "(int a, float b) {}\n"
                                 "void main() {\n"
                                 "    " +
                                 fnName + "(42, \n";

        angel_lsp::parser::AngelScriptParser parser;
        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});

        const size_t pos = code.find("(42, \n");
        const uint32_t cursor = static_cast<uint32_t>(pos + 5); // after comma

        auto ctx = angel_lsp::analysis::FindCallContext(index, cursor);
        REQUIRE(ctx.has_value());
        CHECK(ctx->kind == angel_lsp::analysis::CallContextKind::Direct);
        CHECK(ctx->activeParameter == 1);
        CHECK(angel_lsp::utils::SliceSource(code, ctx->calleeStartByte, ctx->calleeEndByte) == fnName);

        ts_tree_delete(tree);
    }

    TEST_CASE("CallContextFacts - Global qualifier is empty and earlier errors do not capture later cursors")
    {
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fnGlobal");
        const std::string brokenFn = angel_lsp::test::GenerateRandomSymbolName("fnBroken");
        const std::string code = "void " + fnName +
                                 "(int a, int b) {}\n"
                                 "void " +
                                 brokenFn +
                                 "() { int x = (1 + ; }\n"
                                 "void main() {\n"
                                 "    ::" +
                                 fnName +
                                 "(1, 2);\n"
                                 "    int y = 3;\n"
                                 "}\n";

        angel_lsp::parser::AngelScriptParser parser;
        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);
        angel_lsp::analysis::NodeIndex index(ts_tree_root_node(tree),
                                             angel_lsp::analysis::NodeIndexOptions{.source = code});

        const auto globalCall = static_cast<uint32_t>(code.find("::" + fnName + "(1, ") + fnName.size() + 6);
        const auto ctx = angel_lsp::analysis::FindCallContext(index, globalCall);
        REQUIRE(ctx.has_value());
        CHECK(ctx->kind == angel_lsp::analysis::CallContextKind::Qualified);
        CHECK(ctx->activeParameter == 1);
        CHECK(ctx->receiverStartByte == ctx->receiverEndByte);
        CHECK(angel_lsp::utils::SliceSource(code, ctx->calleeStartByte, ctx->calleeEndByte) == fnName);

        const auto plainStatement = static_cast<uint32_t>(code.find("int y = 3") + 4);
        CHECK_FALSE(angel_lsp::analysis::FindCallContext(index, plainStatement).has_value());

        ts_tree_delete(tree);
    }

    TEST_CASE("SignatureFormatter - Parameter Offsets with Non-ASCII Default Value")
    {
        angel_lsp::analysis::Symbol sym;
        sym.name = "ShowMessage";
        sym.type = angel_lsp::analysis::SymbolType::Function;
        sym.containerName = "ChatManager";

        angel_lsp::analysis::FunctionSignature fn;
        fn.returnType = "void";

        angel_lsp::analysis::ParameterInformation p1;
        p1.typeName = "int";
        p1.name = "channel";

        angel_lsp::analysis::ParameterInformation p2;
        p2.typeName = "string";
        p2.name = "greeting";
        p2.defaultValue = "\"español\""; // Non-ASCII UTF-8 characters

        fn.parameters.push_back(p1);
        fn.parameters.push_back(p2);
        sym.signature = fn;

        auto formatted = angel_lsp::analysis::FormatSignatureWithParameterOffsets(sym);
        CHECK(formatted.label == "void ChatManager::ShowMessage(int channel, string greeting = \"español\")");
        REQUIRE(formatted.parameterOffsets.size() == 2);

        // Verify slicing label by UTF-16 offsets recovers parameter strings
        const std::string param1Expected = angel_lsp::analysis::FormatParameter(p1);
        const std::string param2Expected = angel_lsp::analysis::FormatParameter(p2);

        const auto offset1 = formatted.parameterOffsets[0];
        const auto offset2 = formatted.parameterOffsets[1];

        const uint32_t b1Start = angel_lsp::utils::LspCharToByteColumn(formatted.label, offset1.first,
                                                                       angel_lsp::utils::PositionEncoding::Utf16);
        const uint32_t b1End = angel_lsp::utils::LspCharToByteColumn(formatted.label, offset1.second,
                                                                     angel_lsp::utils::PositionEncoding::Utf16);
        CHECK(formatted.label.substr(b1Start, b1End - b1Start) == param1Expected);

        const uint32_t b2Start = angel_lsp::utils::LspCharToByteColumn(formatted.label, offset2.first,
                                                                       angel_lsp::utils::PositionEncoding::Utf16);
        const uint32_t b2End = angel_lsp::utils::LspCharToByteColumn(formatted.label, offset2.second,
                                                                     angel_lsp::utils::PositionEncoding::Utf16);
        CHECK(formatted.label.substr(b2Start, b2End - b2Start) == param2Expected);
    }

    TEST_CASE("SignatureFormatter - Namespaced Constructor Detection")
    {
        angel_lsp::analysis::Symbol ctorSym;
        ctorSym.name = "MyWidget";
        ctorSym.type = angel_lsp::analysis::SymbolType::Function;
        ctorSym.containerName = "UI::Controls::MyWidget";

        angel_lsp::analysis::FunctionSignature fn;
        fn.returnType = ""; // constructor return type is empty

        angel_lsp::analysis::ParameterInformation p;
        p.typeName = "int";
        p.name = "width";
        fn.parameters.push_back(p);
        ctorSym.signature = fn;

        auto formatted = angel_lsp::analysis::FormatSignatureWithParameterOffsets(ctorSym);
        // Must NOT contain "void"
        CHECK(formatted.label.find("void") == std::string::npos);
        CHECK(formatted.label == "UI::Controls::MyWidget::MyWidget(int width)");
        REQUIRE(formatted.parameterOffsets.size() == 1);
    }
}
