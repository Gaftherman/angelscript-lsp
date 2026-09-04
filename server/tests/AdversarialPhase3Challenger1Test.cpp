#include <doctest/doctest.h>

#include "features/hover/HoverHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/references/ReferencesHandler.h"
#include "features/rename/RenameHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "features/document_highlight/DocumentHighlightHandler.h"
#include "features/folding_range/FoldingRangeHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct Phase3AdversarialEnv
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///phase3_adversarial.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        Phase3AdversarialEnv(const std::string &code, const std::string &fileUri = "file:///phase3_adversarial.as")
            : uri(fileUri), sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        void AddAdditionalFile(const std::string &otherUri, const std::string &otherCode)
        {
            AngelScriptParser otherParser;
            TSTree *otherTree = otherParser.Parse(otherCode);
            symbolCollector.CollectSymbols(otherUri, otherCode, otherParser, symbolTable);
            auto otherScope = scopeCollector.CollectScopes(otherCode, otherParser);
            if (otherScope)
            {
                scopeIndex.SetScopeTree(otherUri, std::move(otherScope));
            }
            if (otherTree)
            {
                ts_tree_delete(otherTree);
            }
        }

        ~Phase3AdversarialEnv()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<lsp::Hover> Hover(uint32_t line, uint32_t col)
        {
            HoverRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, col } };
            return GetHover(req);
        }

        std::optional<std::vector<lsp::Location>> Def(uint32_t line, uint32_t col)
        {
            DefinitionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, col } };
            return GetDefinition(req);
        }

        std::optional<ReferencesResult> Refs(uint32_t line, uint32_t col, bool includeDecl = true)
        {
            ReferencesRequest req{ uri, sourceCode, tree, lsp::Position{ line, col }, includeDecl, symbolTable, scopeIndex };
            return GetReferences(req);
        }

        std::unordered_set<std::string> predefinedUris;

        std::optional<lsp::WorkspaceEdit> Rename(uint32_t line, uint32_t col, const std::string &newName)
        {
            RenameRequest req{ uri, sourceCode, tree, lsp::Position{ line, col }, newName, symbolTable, scopeIndex, predefinedUris };
            return features::Rename(req);
        }

        lsp::SemanticTokens Tokens()
        {
            SemanticTokensRequest req{ uri, sourceCode, tree, symbolTable };
            return GetSemanticTokens(req);
        }

        std::optional<DocumentHighlightResult> Highlights(uint32_t line, uint32_t col)
        {
            DocumentHighlightRequest req{ uri, sourceCode, tree, lsp::Position{ line, col }, symbolTable, scopeIndex };
            return GetDocumentHighlights(req);
        }

        std::optional<FoldingRangeResult> Folding()
        {
            FoldingRangeRequest req{ uri, sourceCode, tree };
            return GetFoldingRanges(req);
        }
    };

    struct DecodedSemToken
    {
        uint32_t line;
        uint32_t startCol;
        uint32_t length;
        uint32_t tokenType;
        uint32_t tokenMod;
    };

    std::vector<DecodedSemToken> DecodeTokens(const lsp::SemanticTokens &tokens)
    {
        std::vector<DecodedSemToken> decoded;
        uint32_t curLine = 0;
        uint32_t curCol = 0;
        for (size_t i = 0; i < tokens.data.size(); i += 5)
        {
            uint32_t deltaLine = tokens.data[i];
            uint32_t deltaCol  = tokens.data[i + 1];
            uint32_t len       = tokens.data[i + 2];
            uint32_t type      = tokens.data[i + 3];
            uint32_t mod       = tokens.data[i + 4];

            if (deltaLine > 0)
            {
                curLine += deltaLine;
                curCol = deltaCol;
            }
            else
            {
                curCol += deltaCol;
            }

            decoded.push_back({ curLine, curCol, len, type, mod });
        }
        return decoded;
    }
}

// =============================================================================
// AREA 1: DEEP INHERITANCE HIERARCHIES (R1)
// =============================================================================

TEST_CASE("Adversarial Phase 3 - Deep 4-Tier Inheritance Hierarchy Hover, Def, Refs, Rename")
{
    std::string code =
        "class GrandParent {\n"
        "    void CoreMethod() {}\n"       // Line 1: GrandParent::CoreMethod
        "    int coreField;\n"            // Line 2
        "}\n"
        "class Parent : GrandParent {\n"
        "    void ParentMethod() {}\n"     // Line 5: Parent::ParentMethod
        "}\n"
        "class Child : Parent {\n"
        "    void CoreMethod() override {}\n" // Line 8: Child::CoreMethod override
        "    void ChildMethod() {}\n"      // Line 9
        "}\n"
        "class GrandChild : Child {\n"
        "    void Test() {\n"
        "        CoreMethod();\n"          // Line 13, col 8: implicit call to CoreMethod
        "        ParentMethod();\n"        // Line 14, col 8: implicit call to ParentMethod
        "        this.CoreMethod();\n"     // Line 15, col 13: this.CoreMethod
        "    }\n"
        "}\n"
        "class Unrelated {\n"
        "    void CoreMethod() {}\n"       // Line 19: Unrelated::CoreMethod (must NOT be touched or matched)
        "}\n"
        "void main() {\n"
        "    GrandChild gc;\n"
        "    gc.CoreMethod();\n"           // Line 23, col 7
        "    gc.ParentMethod();\n"         // Line 24, col 7
        "    Unrelated u;\n"
        "    u.CoreMethod();\n"            // Line 26, col 6
        "}\n";

    Phase3AdversarialEnv env(code);

    SUBCASE("1. Hover on deep inherited methods")
    {
        // Hover on gc.ParentMethod() (defined 2 levels up in Parent)
        auto hParent = env.Hover(24, 7);
        REQUIRE(hParent.has_value());
        auto valParent = std::get<lsp::MarkupContent>(hParent->contents).value;
        CHECK(valParent.find("Parent::ParentMethod()") != std::string::npos);

        // Hover on implicit ParentMethod() inside GrandChild::Test
        auto hImplicitParent = env.Hover(14, 8);
        REQUIRE(hImplicitParent.has_value());
        auto valImp = std::get<lsp::MarkupContent>(hImplicitParent->contents).value;
        CHECK(valImp.find("ParentMethod") != std::string::npos);

        // Hover on this.CoreMethod()
        auto hThis = env.Hover(15, 13);
        REQUIRE(hThis.has_value());
        auto valThis = std::get<lsp::MarkupContent>(hThis->contents).value;
        CHECK(valThis.find("CoreMethod") != std::string::npos);
    }

    SUBCASE("2. Definition jump across deep inheritance")
    {
        // Jump to ParentMethod from gc.ParentMethod() -> line 5 in Parent
        auto defParent = env.Def(24, 7);
        REQUIRE(defParent.has_value());
        REQUIRE(!defParent->empty());
        CHECK((*defParent)[0].range.start.line == 5);

        // Jump to ParentMethod from inside GrandChild::Test -> line 5
        auto defImp = env.Def(14, 8);
        REQUIRE(defImp.has_value());
        REQUIRE(!defImp->empty());
        CHECK((*defImp)[0].range.start.line == 5);
    }

    SUBCASE("3. References across deep hierarchy isolated from unrelated class")
    {
        // References on GrandParent::CoreMethod (line 1, col 9)
        auto refs = env.Refs(1, 9, true);
        REQUIRE(refs.has_value());

        // Should include:
        // - GrandParent::CoreMethod declaration (line 1)
        // - Child::CoreMethod override (line 8)
        // - GrandChild::Test implicit call (line 13)
        // - GrandChild::Test this.CoreMethod call (line 15)
        // - main() gc.CoreMethod() call (line 23)
        // Must NEVER include Unrelated::CoreMethod (line 19) or u.CoreMethod() (line 26)!
        std::vector<uint32_t> refLines;
        for (const auto &loc : *refs)
        {
            refLines.push_back(loc.range.start.line);
        }

        CHECK(std::find(refLines.begin(), refLines.end(), 1u) != refLines.end());
        CHECK(std::find(refLines.begin(), refLines.end(), 8u) != refLines.end());
        CHECK(std::find(refLines.begin(), refLines.end(), 13u) != refLines.end());
        CHECK(std::find(refLines.begin(), refLines.end(), 15u) != refLines.end());
        CHECK(std::find(refLines.begin(), refLines.end(), 23u) != refLines.end());

        CHECK(std::find(refLines.begin(), refLines.end(), 19u) == refLines.end());
        CHECK(std::find(refLines.begin(), refLines.end(), 26u) == refLines.end());
    }

    SUBCASE("4. Rename across deep hierarchy updates overrides and calls, protects unrelated classes")
    {
        auto edit = env.Rename(1, 9, "RenamedCoreMethod");
        REQUIRE(edit.has_value());
        REQUIRE(edit->changes.has_value());
        auto &changesMap = *edit->changes;
        auto it = changesMap.find(lsp::DocumentUri::parse(env.uri));
        REQUIRE(it != changesMap.end());
        const auto &edits = it->second;
        REQUIRE(!edits.empty());

        std::vector<uint32_t> editLines;
        for (const auto &e : edits)
        {
            editLines.push_back(e.range.start.line);
            CHECK(e.newText == "RenamedCoreMethod");
        }

        CHECK(std::find(editLines.begin(), editLines.end(), 1u) != editLines.end());
        CHECK(std::find(editLines.begin(), editLines.end(), 8u) != editLines.end());
        CHECK(std::find(editLines.begin(), editLines.end(), 13u) != editLines.end());
        CHECK(std::find(editLines.begin(), editLines.end(), 15u) != editLines.end());
        CHECK(std::find(editLines.begin(), editLines.end(), 23u) != editLines.end());

        // Unrelated class method declaration and call must NOT be modified
        CHECK(std::find(editLines.begin(), editLines.end(), 19u) == editLines.end());
        CHECK(std::find(editLines.begin(), editLines.end(), 26u) == editLines.end());
    }
}

TEST_CASE("Adversarial Phase 3 - Diamond Interface Hierarchy Method Resolution")
{
    std::string code =
        "interface IRoot {\n"
        "    void Action();\n"             // Line 1
        "}\n"
        "interface ILeft : IRoot {\n"
        "    void LeftAction();\n"         // Line 4
        "}\n"
        "interface IRight : IRoot {\n"
        "    void RightAction();\n"        // Line 7
        "}\n"
        "class DiamondImpl : ILeft, IRight {\n"
        "    void Action() override {}\n"  // Line 10
        "    void LeftAction() override {}\n" // Line 11
        "    void RightAction() override {}\n" // Line 12
        "}\n"
        "void main() {\n"
        "    DiamondImpl d;\n"
        "    d.Action();\n"                // Line 16, col 6
        "    d.LeftAction();\n"            // Line 17, col 6
        "    d.RightAction();\n"           // Line 18, col 6
        "}\n";

    Phase3AdversarialEnv env(code);

    // Hover on d.Action()
    auto hAction = env.Hover(16, 6);
    REQUIRE(hAction.has_value());
    CHECK(std::get<lsp::MarkupContent>(hAction->contents).value.find("Action") != std::string::npos);

    // Definition on d.Action()
    auto defAction = env.Def(16, 6);
    REQUIRE(defAction.has_value());
    REQUIRE(!defAction->empty());
    // Should point to IRoot::Action or DiamondImpl::Action
    uint32_t line = (*defAction)[0].range.start.line;
    CHECK((line == 1 || line == 10));

    // References on IRoot::Action (line 1, col 9)
    auto refs = env.Refs(1, 9, true);
    REQUIRE(refs.has_value());
    std::vector<uint32_t> refLines;
    for (const auto &loc : *refs)
    {
        refLines.push_back(loc.range.start.line);
    }
    CHECK(std::find(refLines.begin(), refLines.end(), 1u) != refLines.end());
    CHECK(std::find(refLines.begin(), refLines.end(), 10u) != refLines.end());
    CHECK(std::find(refLines.begin(), refLines.end(), 16u) != refLines.end());
}

// =============================================================================
// AREA 2: PRIMITIVE TYPES SEMANTIC HIGHLIGHTING (R2)
// =============================================================================

TEST_CASE("Adversarial Phase 3 - All 15 Primitive Types Tokenized as Type_Keyword without Mod_DefaultLibrary")
{
    // List of all 15 primitive types specified in R2:
    // int, float, uint, bool, double, void, int8, uint8, int16, uint16, int32, uint32, int64, uint64, auto
    std::string code =
        "int v1 = 1;\n"
        "float v2 = 2.0f;\n"
        "uint v3 = 3u;\n"
        "bool v4 = true;\n"
        "double v5 = 4.0;\n"
        "void Func() {}\n"
        "int8 v6 = 5;\n"
        "uint8 v7 = 6;\n"
        "int16 v8 = 7;\n"
        "uint16 v9 = 8;\n"
        "int32 v10 = 9;\n"
        "uint32 v11 = 10;\n"
        "int64 v12 = 11;\n"
        "uint64 v13 = 12;\n"
        "auto v14 = 13;\n"
        "class CustomClass {}\n"
        "CustomClass userObj;\n"; // Line 16: CustomClass should be Type_Type (1), NOT Type_Keyword (15)

    Phase3AdversarialEnv env(code);
    auto tokens = env.Tokens();

    REQUIRE(tokens.data.size() % 5 == 0);
    auto decoded = DecodeTokens(tokens);

    // TokenTypeIndex enum: Type_Type = 1, Type_Keyword = 15
    // Mod_DefaultLibrary = 1 << 9 (512)

    struct ExpectedPrim
    {
        uint32_t line;
        uint32_t startCol;
        uint32_t length;
        std::string name;
    };

    std::vector<ExpectedPrim> expectedPrims = {
        { 0,  0, 3,  "int" },
        { 1,  0, 5,  "float" },
        { 2,  0, 4,  "uint" },
        { 3,  0, 4,  "bool" },
        { 4,  0, 6,  "double" },
        { 5,  0, 4,  "void" },
        { 6,  0, 4,  "int8" },
        { 7,  0, 5,  "uint8" },
        { 8,  0, 5,  "int16" },
        { 9,  0, 6,  "uint16" },
        { 10, 0, 5,  "int32" },
        { 11, 0, 6,  "uint32" },
        { 12, 0, 5,  "int64" },
        { 13, 0, 6,  "uint64" },
        { 14, 0, 4,  "auto" }
    };

    for (const auto &ep : expectedPrims)
    {
        auto it = std::find_if(decoded.begin(), decoded.end(),
            [&](const DecodedSemToken &t) {
                return t.line == ep.line && t.startCol == ep.startCol && t.length == ep.length;
            });

        INFO("Checking primitive type: " << ep.name << " at line " << ep.line);
        REQUIRE(it != decoded.end());
        // Must be tokenType 15 (Type_Keyword)
        CHECK(it->tokenType == 15);
        // Must NEVER have Mod_DefaultLibrary (bit 9)
        CHECK((it->tokenMod & (1 << 9)) == 0);
    }

    // Verify user-defined class CustomClass on line 16 is a type of its own and NOT Type_Keyword
    // (15), which is what this test exists to prove: a primitive is a keyword, a user type is not.
    //
    // It used to require Type_Type (1) exactly. That was the value the handler happened to produce,
    // not the point being made - the token now carries Type_Class (2), because the handler resolves
    // the name against the symbol table and a class colours as a class. Editor themes give those two
    // different colours, so the narrower answer is the better one.
    auto itUser = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedSemToken &t) {
            return t.line == 16 && t.startCol == 0 && t.length == 11;
        });
    REQUIRE(itUser != decoded.end());
    CHECK(itUser->tokenType == 2); // Type_Class
    CHECK(itUser->tokenType != 15); // and never Type_Keyword
}

TEST_CASE("Adversarial Phase 3 - Primitive Types in Signatures, Casts, and Modifiers")
{
    std::string code =
        "int64 ProcessData(const uint32 &in id, double val, bool flag) {\n"
        "    uint64 casted = cast<uint64>(val);\n"
        "    return int64(id);\n"
        "}\n";

    Phase3AdversarialEnv env(code);
    auto tokens = env.Tokens();
    REQUIRE(tokens.data.size() % 5 == 0);
    auto decoded = DecodeTokens(tokens);

    // Return type 'int64' (line 0, col 0, len 5)
    auto itRet = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedSemToken &t) { return t.line == 0 && t.startCol == 0 && t.length == 5; });
    REQUIRE(itRet != decoded.end());
    CHECK(itRet->tokenType == 15);

    // Parameter 'uint32' (line 0, col 24, len 6)
    auto itParam1 = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedSemToken &t) { return t.line == 0 && t.startCol == 24 && t.length == 6; });
    REQUIRE(itParam1 != decoded.end());
    CHECK(itParam1->tokenType == 15);

    // Parameter 'double' (line 0, col 39, len 6)
    auto itParam2 = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedSemToken &t) { return t.line == 0 && t.startCol == 39 && t.length == 6; });
    REQUIRE(itParam2 != decoded.end());
    CHECK(itParam2->tokenType == 15);

    // Parameter 'bool' (line 0, col 51, len 4)
    auto itParam3 = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedSemToken &t) { return t.line == 0 && t.startCol == 51 && t.length == 4; });
    REQUIRE(itParam3 != decoded.end());
    CHECK(itParam3->tokenType == 15);
}

// =============================================================================
// AREA 3: DOCUMENT HIGHLIGHTS (R3)
// =============================================================================

TEST_CASE("Adversarial Phase 3 - Document Highlights All Compound and Mutating Operators")
{
    std::string code =
        "void TestOperators() {\n"
        "    int target = 100;\n"         // Line 1: declaration -> Write
        "    target += 1;\n"              // Line 2: += -> Write
        "    target -= 2;\n"              // Line 3: -= -> Write
        "    target *= 3;\n"              // Line 4: *= -> Write
        "    target /= 4;\n"              // Line 5: /= -> Write
        "    target %= 5;\n"              // Line 6: %= -> Write
        "    target &= 6;\n"              // Line 7: &= -> Write
        "    target |= 7;\n"              // Line 8: |= -> Write
        "    target ^= 8;\n"              // Line 9: ^= -> Write
        "    target <<= 1;\n"             // Line 10: <<= -> Write
        "    target >>= 1;\n"             // Line 11: >>= -> Write
        "    target >>>= 1;\n"            // Line 12: >>>= -> Write
        "    ++target;\n"                 // Line 13: ++prefix -> Write
        "    --target;\n"                 // Line 14: --prefix -> Write
        "    target++;\n"                 // Line 15: postfix++ -> Write
        "    target--;\n"                 // Line 16: postfix-- -> Write
        "    int readVal = target + 5;\n" // Line 17: Read
        "}\n";

    Phase3AdversarialEnv env(code);

    auto hls = env.Highlights(1, 8); // Cursor on 'target' declaration
    REQUIRE(hls.has_value());
    REQUIRE(hls->size() == 17);

    // All lines 1 to 16 must be Write
    for (size_t i = 0; i < 16; ++i)
    {
        INFO("Checking target highlight index " << i << " at line " << (*hls)[i].range.start.line);
        CHECK((*hls)[i].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    }

    // Line 17 must be Read
    CHECK((*hls)[16].range.start.line == 17);
    CHECK((*hls)[16].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
}

TEST_CASE("Adversarial Phase 3 - Document Highlights Complex LHS Member and Array Indices")
{
    std::string code =
        "class SubItem {\n"
        "    int value;\n"
        "}\n"
        "class Container {\n"
        "    SubItem item;\n"
        "}\n"
        "void main() {\n"
        "    Container c;\n"              // Line 7
        "    int idx = 0;\n"              // Line 8
        "    c.item.value = 42;\n"        // Line 9
        "    int r = c.item.value;\n"     // Line 10
        "}\n";

    Phase3AdversarialEnv env(code);

    SUBCASE("c in c.item.value = 42 is Read (receiver), not Write")
    {
        auto hlsC = env.Highlights(7, 14); // Container c;
        REQUIRE(hlsC.has_value());
        REQUIRE(hlsC->size() == 3);

        // Line 7: Container c -> Write (declaration)
        CHECK((*hlsC)[0].range.start.line == 7);
        CHECK((*hlsC)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 9: c.item.value = 42 -> c is Read
        CHECK((*hlsC)[1].range.start.line == 9);
        CHECK((*hlsC)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);

        // Line 10: int r = c.item.value -> c is Read
        CHECK((*hlsC)[2].range.start.line == 10);
        CHECK((*hlsC)[2].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }

    SUBCASE("value in c.item.value = 42 is Write (assigned member)")
    {
        auto hlsVal = env.Highlights(1, 8); // SubItem::value
        REQUIRE(hlsVal.has_value());
        REQUIRE(hlsVal->size() == 3);

        // Line 1: int value -> Write (declaration)
        CHECK((*hlsVal)[0].range.start.line == 1);
        CHECK((*hlsVal)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 9: c.item.value = 42 -> value is Write
        CHECK((*hlsVal)[1].range.start.line == 9);
        CHECK((*hlsVal)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 10: int r = c.item.value -> value is Read
        CHECK((*hlsVal)[2].range.start.line == 10);
        CHECK((*hlsVal)[2].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }
}

TEST_CASE("Adversarial Phase 3 - Document Highlights Method Call with &out and &inout Ref Arguments")
{
    std::string code =
        "class Processor {\n"
        "    void Transform(int &out dst, int &inout accum, const int &in factor, int bias) {\n"
        "        dst = accum * factor + bias;\n"
        "        accum += dst;\n"
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Processor proc;\n"
        "    int outResult = 0;\n"        // Line 8: outResult decl -> Write
        "    int runningSum = 10;\n"       // Line 9: runningSum decl -> Write
        "    int multFactor = 2;\n"        // Line 10: multFactor decl -> Write
        "    int biasVal = 5;\n"           // Line 11: biasVal decl -> Write
        "    proc.Transform(outResult, runningSum, multFactor, biasVal);\n" // Line 12
        "}\n";

    Phase3AdversarialEnv env(code);

    // outResult passed to dst (&out) -> Write on line 12
    auto hlsOut = env.Highlights(8, 8);
    REQUIRE(hlsOut.has_value());
    REQUIRE(hlsOut->size() == 2);
    CHECK((*hlsOut)[0].range.start.line == 8);
    CHECK((*hlsOut)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    CHECK((*hlsOut)[1].range.start.line == 12);
    CHECK((*hlsOut)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

    // runningSum passed to accum (&inout) -> Write on line 12
    auto hlsInOut = env.Highlights(9, 8);
    REQUIRE(hlsInOut.has_value());
    REQUIRE(hlsInOut->size() == 2);
    CHECK((*hlsInOut)[0].range.start.line == 9);
    CHECK((*hlsInOut)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    CHECK((*hlsInOut)[1].range.start.line == 12);
    CHECK((*hlsInOut)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

    // multFactor passed to factor (const &in) -> Read on line 12
    auto hlsFactor = env.Highlights(10, 8);
    REQUIRE(hlsFactor.has_value());
    REQUIRE(hlsFactor->size() == 2);
    CHECK((*hlsFactor)[0].range.start.line == 10);
    CHECK((*hlsFactor)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    CHECK((*hlsFactor)[1].range.start.line == 12);
    CHECK((*hlsFactor)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);

    // biasVal passed by value -> Read on line 12
    auto hlsBias = env.Highlights(11, 8);
    REQUIRE(hlsBias.has_value());
    REQUIRE(hlsBias->size() == 2);
    CHECK((*hlsBias)[0].range.start.line == 11);
    CHECK((*hlsBias)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    CHECK((*hlsBias)[1].range.start.line == 12);
    CHECK((*hlsBias)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
}

// =============================================================================
// AREA 4: FOLDING RANGES (R4)
// =============================================================================

TEST_CASE("Adversarial Phase 3 - Folding Ranges on Deeply Nested Namespaces, Classes, and Preprocessor")
{
    std::string code =
        "namespace Tier1\n"
        "{\n"
        "    namespace Tier2\n"
        "    {\n"
        "        #region ConfigSection\n"
        "        #if FEATURE_X\n"
        "        #include \"tier2_a.as\"\n"
        "        #include \"tier2_b.as\"\n"
        "        #endif\n"
        "        #endregion\n"
        "\n"
        "        /*\n"
        "         * Multiline Doxygen block\n"
        "         * for Tier2Class\n"
        "         */\n"
        "        class Tier2Class\n"
        "        {\n"
        "            // Single line 1\n"
        "            // Single line 2\n"
        "            // Single line 3\n"
        "            void DeepMethod(\n"
        "                int param1,\n"
        "                int param2\n"
        "            )\n"
        "            {\n"
        "                if (param1 > 0)\n"
        "                {\n"
        "                    while (param2 > 0)\n"
        "                    {\n"
        "                        param2--;\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";

    Phase3AdversarialEnv env(code);
    auto folding = env.Folding();
    REQUIRE(folding.has_value());
    REQUIRE(!folding->empty());

    // Invariant: every folding range must satisfy startLine < endLine
    for (const auto &fr : *folding)
    {
        CHECK(fr.startLine < fr.endLine);
    }

    bool hasTier1Ns = false;
    bool hasTier2Ns = false;
    bool hasRegion = false;
    bool hasIfDef = false;
    bool hasIncludes = false;
    bool hasBlockComment = false;
    bool hasLineComments = false;
    bool hasClass = false;
    bool hasDeepMethod = false;
    bool hasIfBlock = false;
    bool hasWhileBlock = false;

    for (const auto &fr : *folding)
    {
        if (fr.startLine <= 1 && fr.endLine == 35) hasTier1Ns = true;
        if (fr.startLine >= 2 && fr.startLine <= 3 && fr.endLine == 34) hasTier2Ns = true;
        if (fr.kind == lsp::FoldingRangeKind::Region && fr.startLine == 4 && fr.endLine == 9) hasRegion = true;
        if (fr.startLine == 5 && fr.endLine == 8) hasIfDef = true;
        if (fr.kind == lsp::FoldingRangeKind::Imports && fr.startLine == 6 && fr.endLine == 7) hasIncludes = true;
        if (fr.kind == lsp::FoldingRangeKind::Comment && fr.startLine == 11 && fr.endLine == 14) hasBlockComment = true;
        if (fr.kind == lsp::FoldingRangeKind::Comment && fr.startLine == 17 && fr.endLine == 19) hasLineComments = true;
        if (fr.startLine >= 15 && fr.startLine <= 16 && fr.endLine == 33) hasClass = true;
        if (fr.startLine >= 20 && fr.startLine <= 24 && fr.endLine == 32) hasDeepMethod = true;
        if (fr.startLine >= 25 && fr.startLine <= 26 && fr.endLine == 31) hasIfBlock = true;
        if (fr.startLine >= 27 && fr.startLine <= 28 && fr.endLine == 30) hasWhileBlock = true;
    }

    CHECK(hasTier1Ns);
    CHECK(hasTier2Ns);
    CHECK(hasRegion);
    CHECK(hasIfDef);
    CHECK(hasIncludes);
    CHECK(hasBlockComment);
    CHECK(hasLineComments);
    CHECK(hasClass);
    CHECK(hasDeepMethod);
    CHECK(hasIfBlock);
    CHECK(hasWhileBlock);
}
