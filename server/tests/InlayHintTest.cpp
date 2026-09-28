#include <doctest/doctest.h>

#include "features/inlay_hint/InlayHintHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct TestEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        TestEnvironment(const std::string &code)
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

        ~TestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<std::vector<lsp::InlayHint>> InlayHints(
            lsp::Range range = lsp::Range{{0, 0}, {0, 0}},
            bool suppressWhenArgumentMatchesName = false,
            size_t maxParameters = 0,
            size_t maxLength = 0,
            config::OmittedDefaultArgumentsMode omittedDefaultArguments =
                config::OmittedDefaultArgumentsMode::NameAndValue)
        {
            InlayHintRequest req{uri,
                                 sourceCode,
                                 tree,
                                 range,
                                 symbolTable,
                                 scopeIndex,
                                 suppressWhenArgumentMatchesName,
                                 nullptr,
                                 maxParameters,
                                 maxLength,
                                 omittedDefaultArguments};
            return GetInlayHints(req);
        }
    };
}

TEST_CASE("InlayHintHandler - Basic Function Call Parameter Hints")
{
    std::string code =
        "void Test(int a, float b) {}\n"
        "void main() {\n"
        "    Test(10, 2.5f);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() >= 2);

    // First parameter hint: a:
    std::string label0 = std::holds_alternative<std::string>(hints->at(0).label)
                             ? std::get<std::string>(hints->at(0).label)
                             : "";
    CHECK(label0 == "a:");
    CHECK(hints->at(0).kind.has_value());
    bool isParam0 = (hints->at(0).kind.value() == lsp::InlayHintKind::Parameter);
    CHECK(isParam0);

    // Second parameter hint: b:
    std::string label1 = std::holds_alternative<std::string>(hints->at(1).label)
                             ? std::get<std::string>(hints->at(1).label)
                             : "";
    CHECK(label1 == "b:");
    CHECK(hints->at(1).kind.has_value());
    bool isParam1 = (hints->at(1).kind.value() == lsp::InlayHintKind::Parameter);
    CHECK(isParam1);
}

TEST_CASE("InlayHintHandler - Exclusion Rule: Named Arguments")
{
    std::string code =
        "void SetValues(int x, int y) {}\n"
        "void main() {\n"
        "    SetValues(x: 10, 20);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    // Only 'y:' should be emitted since 'x' is already explicitly named in syntax
    REQUIRE(hints->size() == 1);
    std::string label = std::holds_alternative<std::string>(hints->at(0).label)
                            ? std::get<std::string>(hints->at(0).label)
                            : "";
    CHECK(label == "y:");
}

TEST_CASE("InlayHintHandler - Exclusion Rule: Same-Name Arguments")
{
    std::string code =
        "void SetDimensions(int width, int height) {}\n"
        "void main() {\n"
        "    int width = 100;\n"
        "    int h = 200;\n"
        "    SetDimensions(width, h);\n"
        "}\n";

    TestEnvironment env(code);

    // By default (suppressWhenArgumentMatchesName = false), 'width:' should be emitted
    {
        auto hints = env.InlayHints();
        REQUIRE(hints.has_value());
        bool foundWidthHint = false;
        bool foundHeightHint = false;

        for (const auto &hint : *hints)
        {
            std::string l = std::holds_alternative<std::string>(hint.label)
                                ? std::get<std::string>(hint.label)
                                : "";
            if (l == "width:")
            {
                foundWidthHint = true;
            }
            if (l == "height:")
            {
                foundHeightHint = true;
            }
        }

        CHECK(foundWidthHint);
        CHECK(foundHeightHint);
    }

    // When explicitly suppressed (suppressWhenArgumentMatchesName = true), 'width:' must be suppressed
    {
        auto hints = env.InlayHints(lsp::Range{ {0, 0}, {0, 0} }, true);
        REQUIRE(hints.has_value());
        bool foundWidthHint = false;
        bool foundHeightHint = false;

        for (const auto &hint : *hints)
        {
            std::string l = std::holds_alternative<std::string>(hint.label)
                                ? std::get<std::string>(hint.label)
                                : "";
            if (l == "width:")
            {
                foundWidthHint = true;
            }
            if (l == "height:")
            {
                foundHeightHint = true;
            }
        }

        CHECK(!foundWidthHint);
        CHECK(foundHeightHint);
    }
}

TEST_CASE("InlayHintHandler - Class Method Call with Inheritance")
{
    std::string code =
        "class Base {\n"
        "    void Attack(int damage, float radius) {}\n"
        "}\n"
        "class Player : Base {}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.Attack(50, 10.0f);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    bool foundDamage = false;
    bool foundRadius = false;

    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        if (l == "damage:")
        {
            foundDamage = true;
        }
        if (l == "radius:")
        {
            foundRadius = true;
        }
    }

    CHECK(foundDamage);
    CHECK(foundRadius);
}

TEST_CASE("InlayHintHandler - Auto Variable Type Deduction for Literals")
{
    std::string code =
        "void main() {\n"
        "    auto a = 42;\n"
        "    auto b = 3.14f;\n"
        "    auto c = 2.718;\n"
        "    auto d = true;\n"
        "    auto e = \"hello\";\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 5);

    std::vector<std::string> labels;
    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        labels.push_back(l);
        CHECK(hint.kind.has_value());
        bool isType = (hint.kind.value() == lsp::InlayHintKind::Type);
        CHECK(isType);
    }

    CHECK(labels[0] == ": int");
    CHECK(labels[1] == ": float");
    CHECK(labels[2] == ": double");
    CHECK(labels[3] == ": bool");
    CHECK(labels[4] == ": string");
}

TEST_CASE("InlayHintHandler - Auto Variable Deduction for Function Calls and Casts")
{
    std::string code =
        "class Actor {}\n"
        "Actor@ SpawnActor() { return null; }\n"
        "interface IWeapon {}\n"
        "void main() {\n"
        "    auto actor = SpawnActor();\n"
        "    auto weapon = cast<IWeapon>(null);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() >= 2);

    bool foundActorType = false;
    bool foundWeaponType = false;

    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        if (l == ": Actor@")
        {
            foundActorType = true;
        }
        if (l == ": IWeapon" || l == ": IWeapon@")
        {
            foundWeaponType = true;
        }
    }

    CHECK(foundActorType);
    CHECK(foundWeaponType);
}

TEST_CASE("InlayHintHandler - Sub-range Filtering")
{
    std::string code =
        "void Foo(int x) {}\n"
        "void Bar(int y) {}\n"
        "void main() {\n"
        "    Foo(1);\n"
        "    Bar(2);\n"
        "}\n";

    TestEnvironment env(code);

    // Range restricting to only line 3 (Foo(1))
    lsp::Range r{ {3, 0}, {3, 20} };
    auto hints = env.InlayHints(r);

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 1);
    std::string l = std::holds_alternative<std::string>(hints->at(0).label)
                        ? std::get<std::string>(hints->at(0).label)
                        : "";
    CHECK(l == "x:");
}

TEST_CASE("InlayHintHandler - Operator Overload Auto Type Deduction")
{
    std::string code =
        "class Matrix {\n"
        "    Matrix opMul(float scalar) const { return Matrix(); }\n"
        "}\n"
        "class Vector {\n"
        "    Vector opMul_r(const Matrix &in m) const { return Vector(); }\n"
        "}\n"
        "void Main() {\n"
        "    Matrix m;\n"
        "    Vector v;\n"
        "    auto res1 = m * 2.0f;\n"
        "    auto res2 = m * v;\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    bool foundMatrix = false;
    bool foundVector = false;

    for (const auto &hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label)
                            ? std::get<std::string>(hint.label)
                            : "";
        if (l == ": Matrix")
        {
            foundMatrix = true;
            if (hint.tooltip.has_value())
            {
                std::string t = std::holds_alternative<std::string>(*hint.tooltip)
                                    ? std::get<std::string>(*hint.tooltip)
                                    : "";
                CHECK(t == "Deduced type: Matrix");
            }
        }
        if (l == ": Vector")
        {
            foundVector = true;
            if (hint.tooltip.has_value())
            {
                std::string t = std::holds_alternative<std::string>(*hint.tooltip)
                                    ? std::get<std::string>(*hint.tooltip)
                                    : "";
                CHECK(t == "Deduced type: Vector");
            }
        }
    }

    CHECK(foundMatrix);
    CHECK(foundVector);
}

TEST_CASE("InlayHintHandler - Robustness with Empty / Null Tree")
{
    InlayHintRequest req{ "file:///empty.as", "", nullptr, lsp::Range{}, SymbolTable{}, ScopeIndex{} };
    auto hints = GetInlayHints(req);
    CHECK(!hints.has_value());
}

TEST_CASE("InlayHintHandler - ShootGrenade and trailing parameter hints")
{
    std::string code =
        "namespace INS2GLPROJECTILE {\n"
        "    void ShootGrenade(int pevOwner, int vecStart, int vecVelocity, float dmg, string model, bool bRocketExplosions = false, const string& in szName = \"proj_ins2gl\") {}\n"
        "}\n"
        "void main() {\n"
        "    INS2GLPROJECTILE::ShootGrenade(1, 2, 3, 4.0f, \"gmodel\", false, \"proj_name\");\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());
    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }
    CHECK(labels.size() == 7);
}

TEST_CASE("InlayHintHandler - BaseClass and inherited unqualified call parameter hints")
{
    std::string code =
        "class WeaponBase {\n"
        "    WeaponBase@ BaseClass;\n"
        "    void Holster(int skiplocal = 0) {}\n"
        "    bool Deploy(string v, string p, int draw, string model, int body, float speed) { return true; }\n"
        "}\n"
        "class weapon_ins2l85a2 : WeaponBase {\n"
        "    bool Deploy() { return true; }\n"
        "    void Holster(int skipLocal = 0) {\n"
        "        BaseClass.Holster(skipLocal);\n"
        "        Deploy(\"v\", \"p\", 1, \"model\", 0, 1.5f);\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());
    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }
    CHECK(std::find(labels.begin(), labels.end(), "skiplocal:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "speed:") != labels.end());
}

TEST_CASE("InlayHintHandler - Namespaced class this.Method parameter hints")
{
    std::string code =
        "namespace INS2_L85A2 {\n"
        "    class weapon_ins2l85a2 {\n"
        "        void SendWeaponAnim(int anim, int body) {}\n"
        "        void Test() {\n"
        "            this.SendWeaponAnim(1, 2);\n"
        "        }\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());
    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }
    CHECK(std::find(labels.begin(), labels.end(), "anim:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "body:") != labels.end());
}

TEST_CASE("InlayHintHandler - Constructor Direct-Initialization Parameter Hints")
{
    std::string code =
        "class NetworkMessage {\n"
        "    NetworkMessage(int msg_type, int svc_message, int pEdict) {}\n"
        "}\n"
        "void main() {\n"
        "    int edict = 1;\n"
        "    NetworkMessage weapon(100, 200, edict);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());

    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }

    CHECK(labels.size() == 3);
    CHECK(labels[0] == "msg_type:");
    CHECK(labels[1] == "svc_message:");
    CHECK(labels[2] == "pEdict:");
}

TEST_CASE("InlayHintHandler - Mixin Method Parameter Hints")
{
    std::string code =
        "mixin class PlayerMixin {\n"
        "    void CommonAddToPlayer(int pPlayer) {}\n"
        "}\n"
        "class MyPlayer : PlayerMixin {\n"
        "    void AddToPlayer() {\n"
        "        CommonAddToPlayer(42);\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());

    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pPlayer:") != labels.end());
}

TEST_CASE("InlayHintHandler - Mixin Method Parameter Hints on Instance")
{
    std::string code =
        "mixin class PlayerMixin {\n"
        "    void CommonAddToPlayer(int pPlayer) {}\n"
        "}\n"
        "class MyPlayer : PlayerMixin {}\n"
        "void main() {\n"
        "    MyPlayer p;\n"
        "    p.CommonAddToPlayer(42);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());

    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pPlayer:") != labels.end());
}

TEST_CASE("InlayHintHandler - BaseClass Method Parameter Hints with Mixin in Hierarchy")
{
    std::string code =
        "mixin class WeaponMixin {\n"
        "    void MixinMethod() {}\n"
        "}\n"
        "class BasePlayerWeapon {\n"
        "    BasePlayerWeapon@ BaseClass;\n"
        "    void Holster(int pPlayer) {}\n"
        "}\n"
        "class MyWeapon : BasePlayerWeapon, WeaponMixin {\n"
        "    void Holster() {\n"
        "        BaseClass.Holster(42);\n"
        "    }\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());

    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pPlayer:") != labels.end());
}

TEST_CASE("InlayHintHandler - Math Utility Object Parameter Hints")
{
    std::string code =
        "class Math {\n"
        "    void MakeVectors(float pitch, float yaw, float roll) {}\n"
        "}\n"
        "void main() {\n"
        "    Math math;\n"
        "    math.MakeVectors(10.0f, 20.0f, 30.0f);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());

    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pitch:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "yaw:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "roll:") != labels.end());
}

TEST_CASE("InlayHintHandler - Relaxed Parameter Name Matching Suppression")
{
    std::string code =
        "void DoSomething(int value) {}\n"
        "void main() {\n"
        "    int value = 5;\n"
        "    DoSomething(value);\n"
        "}\n";

    TestEnvironment env(code);
    // With explicit suppression (true), "value:" should be suppressed because arg.text == param.name
    auto suppressedHints = env.InlayHints(lsp::Range{ {0, 0}, {0, 0} }, true);
    REQUIRE(suppressedHints.has_value());
    bool foundSuppressed = false;
    for (const auto &h : *suppressedHints)
    {
        if (std::holds_alternative<std::string>(h.label) && std::get<std::string>(h.label) == "value:")
        {
            foundSuppressed = true;
        }
    }
    CHECK(!foundSuppressed);

    // With relaxed suppression (false), "value:" hint should be provided
    auto relaxedHints = env.InlayHints(lsp::Range{ {0, 0}, {0, 0} }, false);
    REQUIRE(relaxedHints.has_value());
    bool foundRelaxed = false;
    for (const auto &h : *relaxedHints)
    {
        if (std::holds_alternative<std::string>(h.label) && std::get<std::string>(h.label) == "value:")
        {
            foundRelaxed = true;
        }
    }
    CHECK(foundRelaxed);

    // Default call without arguments also defaults to false (no suppression)
    auto defaultHints = env.InlayHints();
    REQUIRE(defaultHints.has_value());
    bool foundDefault = false;
    for (const auto &h : *defaultHints)
    {
        if (std::holds_alternative<std::string>(h.label) && std::get<std::string>(h.label) == "value:")
        {
            foundDefault = true;
        }
    }
    CHECK(foundDefault);
}

TEST_CASE("InlayHintHandler - Complex Call Involving Namespace Member and Overload Resolution")
{
    std::string code =
        "namespace Math {\n"
        "    int RandomLong(int low, int high) { return low; }\n"
        "}\n"
        "void EmitSoundDyn(int channel, string sample, float volume, float attenuation, int flags, int pitch) {}\n"
        "void main() {\n"
        "    EmitSoundDyn(1, \"sound.wav\", 1.0f, 0.8f, 0, Math::RandomLong(90, 110));\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }
    CHECK(std::find(labels.begin(), labels.end(), "channel:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "sample:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "volume:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "attenuation:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "flags:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "pitch:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "low:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "high:") != labels.end());
}

TEST_CASE("InlayHintHandler - Complex Call Involving Dot-Accessed Namespace Method")
{
    std::string code =
        "namespace Math {\n"
        "    int RandomLong(int low, int high) { return low; }\n"
        "}\n"
        "void EmitSoundDyn(int channel, string sample, float volume, float attenuation, int flags, int pitch) {}\n"
        "void main() {\n"
        "    EmitSoundDyn(1, \"sound.wav\", 1.0f, 0.8f, 0, Math.RandomLong(90, 110));\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    std::vector<std::string> labels;
    for (const auto &h : *hints)
    {
        if (std::holds_alternative<std::string>(h.label))
        {
            labels.push_back(std::get<std::string>(h.label));
        }
    }
    CHECK(std::find(labels.begin(), labels.end(), "channel:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "pitch:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "low:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "high:") != labels.end());
}

TEST_CASE("InlayHintHandler - Call With Default Parameters Retains All Provided Argument Hints")
{
    std::string code =
        "void SetProperties(int width, int height, bool fullscreen = false, int refreshRate = 60) {}\n"
        "void main() {\n"
        "    SetProperties(1920, 1080, true);\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 4);

    std::string label0 = std::holds_alternative<std::string>(hints->at(0).label)
                             ? std::get<std::string>(hints->at(0).label)
                             : "";
    std::string label1 = std::holds_alternative<std::string>(hints->at(1).label)
                             ? std::get<std::string>(hints->at(1).label)
                             : "";
    std::string label2 = std::holds_alternative<std::string>(hints->at(2).label)
                             ? std::get<std::string>(hints->at(2).label)
                             : "";
    std::string label3 = std::holds_alternative<std::string>(hints->at(3).label)
                             ? std::get<std::string>(hints->at(3).label)
                             : "";

    CHECK(label0 == "width:");
    CHECK(label1 == "height:");
    CHECK(label2 == "fullscreen:");
    CHECK(label3 == ", refreshRate: 60");

    // When disabled, only the 3 provided argument hints are returned
    auto hintsOff = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 0, config::OmittedDefaultArgumentsMode::Off);
    REQUIRE(hintsOff.has_value());
    CHECK(hintsOff->size() == 3);
}

TEST_CASE("InlayHintHandler - Parameter labels are never truncated")
{
    std::string code =
        "void ConfigureLogger(bool shouldTrace, string longParameterIdentifier) {}\n"
        "void main() {\n"
        "    ConfigureLogger(true, \"test\");\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 2);

    std::string label0 = std::holds_alternative<std::string>(hints->at(0).label)
                             ? std::get<std::string>(hints->at(0).label)
                             : "";
    std::string label1 = std::holds_alternative<std::string>(hints->at(1).label)
                             ? std::get<std::string>(hints->at(1).label)
                             : "";

    CHECK(label0 == "shouldTrace:");
    CHECK(label1 == "longParameterIdentifier:");
}

TEST_CASE("InlayHintHandler - ShootProp with 13 parameters returns all parameter hints by default")
{
    std::string code =
        "namespace HCASPROP {\n"
        "    CHCASProp@ ShootProp( entvars_t@ pevOwner, Vector& in vecOrigin, Vector& in vecVelocity, Vector& in vecDropAngle, Vector& in vecAngVelocity, string szModel, array<string> BounceSounds, float flVelFriction = 0.4f, float flAVelFriction = 0.7f, int iBodygroup = 0, int iSkingroup = 0, float flStartFadeOutTime = 5.0f, string szPropName = \"proj_hcasprop\" ) {}\n"
        "}\n"
        "void main() {\n"
        "    auto pProp = HCASPROP::ShootProp( pev, vecOrigin, vecVelocity,\n"
        "        Vector( -45, vecAngles.y - 65, 0 ), Vector( 0, 0, 0 ), \"model\", sounds,\n"
        "        0.4f, 0.7f, TOS_BDYGRP, 0, 5.0f, DROP_NAME );\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    // 1 type hint for 'auto' + 13 parameter hints = 14 hints total
    REQUIRE(hints->size() == 14);

    auto getLabel = [&](size_t idx) -> std::string {
        return std::holds_alternative<std::string>(hints->at(idx).label)
                   ? std::get<std::string>(hints->at(idx).label)
                   : "";
    };

    CHECK(getLabel(1) == "pevOwner:");
    CHECK(getLabel(2) == "vecOrigin:");
    CHECK(getLabel(3) == "vecVelocity:");
    CHECK(getLabel(4) == "vecDropAngle:");
    CHECK(getLabel(5) == "vecAngVelocity:");
    CHECK(getLabel(6) == "szModel:");
    CHECK(getLabel(7) == "BounceSounds:");
    CHECK(getLabel(8) == "flVelFriction:");
    CHECK(getLabel(9) == "flAVelFriction:");
    CHECK(getLabel(10) == "iBodygroup:");
    CHECK(getLabel(11) == "iSkingroup:");
    CHECK(getLabel(12) == "flStartFadeOutTime:");
    CHECK(getLabel(13) == "szPropName:");
}

TEST_CASE("InlayHintHandler - maxParameters limits parameter hint count")
{
    std::string code =
        "namespace HCASPROP {\n"
        "    CHCASProp@ ShootProp( entvars_t@ pevOwner, Vector& in vecOrigin, Vector& in vecVelocity, Vector& in vecDropAngle, Vector& in vecAngVelocity, string szModel, array<string> BounceSounds, float flVelFriction = 0.4f, float flAVelFriction = 0.7f, int iBodygroup = 0, int iSkingroup = 0, float flStartFadeOutTime = 5.0f, string szPropName = \"proj_hcasprop\" ) {}\n"
        "}\n"
        "void main() {\n"
        "    auto pProp = HCASPROP::ShootProp( pev, vecOrigin, vecVelocity,\n"
        "        Vector( -45, vecAngles.y - 65, 0 ), Vector( 0, 0, 0 ), \"model\", sounds,\n"
        "        0.4f, 0.7f, TOS_BDYGRP, 0, 5.0f, DROP_NAME );\n"
        "}\n";

    TestEnvironment env(code);
    // Request with maxParameters = 5
    auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 5, 0);

    REQUIRE(hints.has_value());
    // 1 type hint for 'auto' + 5 parameter hints = 6 hints total
    REQUIRE(hints->size() == 6);

    auto getLabel = [&](size_t idx) -> std::string {
        return std::holds_alternative<std::string>(hints->at(idx).label)
                   ? std::get<std::string>(hints->at(idx).label)
                   : "";
    };

    CHECK(getLabel(1) == "pevOwner:");
    CHECK(getLabel(5) == "vecAngVelocity:");
}

TEST_CASE("InlayHintHandler - maxLength truncates parameter labels")
{
    std::string code =
        "namespace HCASPROP {\n"
        "    CHCASProp@ ShootProp( entvars_t@ pevOwner, Vector& in vecOrigin, Vector& in vecVelocity, Vector& in vecDropAngle, Vector& in vecAngVelocity, string szModel, array<string> BounceSounds, float flVelFriction = 0.4f, float flAVelFriction = 0.7f, int iBodygroup = 0, int iSkingroup = 0, float flStartFadeOutTime = 5.0f, string szPropName = \"proj_hcasprop\" ) {}\n"
        "}\n"
        "void main() {\n"
        "    auto pProp = HCASPROP::ShootProp( pev, vecOrigin, vecVelocity,\n"
        "        Vector( -45, vecAngles.y - 65, 0 ), Vector( 0, 0, 0 ), \"model\", sounds,\n"
        "        0.4f, 0.7f, TOS_BDYGRP, 0, 5.0f, DROP_NAME );\n"
        "}\n";

    TestEnvironment env(code);
    // Request with maxLength = 8
    auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 8);

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 14);

    auto getLabel = [&](size_t idx) -> std::string {
        return std::holds_alternative<std::string>(hints->at(idx).label)
                   ? std::get<std::string>(hints->at(idx).label)
                   : "";
    };

    // 'pevOwner' length is 8 -> not truncated: 'pevOwner:'
    CHECK(getLabel(1) == "pevOwner:");
    // 'vecDropAngle' length is 12 -> truncated to 8 chars: 'vecDropA...:'
    CHECK(getLabel(4) == "vecDropA...:");
    // 'vecAngVelocity' length is 14 -> truncated to 8 chars: 'vecAngVe...:'
    CHECK(getLabel(5) == "vecAngVe...:");
    // 'flStartFadeOutTime' length is 18 -> truncated to 8 chars: 'flStartF...:'
    CHECK(getLabel(12) == "flStartF...:");
}

TEST_CASE("InlayHintHandler - Invariant randomized symbols with maxParameters and maxLength")
{
    const std::string fnName = test::GenerateRandomSymbolName("func");
    const std::string p0 = test::GenerateRandomSymbolName("paramZero");
    const std::string p1 = test::GenerateRandomSymbolName("paramOne");
    const std::string p2 = test::GenerateRandomSymbolName("paramTwo");

    std::string code =
        "void " + fnName + "(int " + p0 + ", int " + p1 + ", int " + p2 + ") {}\n"
        "void main() {\n"
        "    " + fnName + "(1, 2, 3);\n"
        "}\n";

    TestEnvironment env(code);

    // 1. Unlimited by default
    {
        auto hints = env.InlayHints();
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 3);
        CHECK(std::get<std::string>(hints->at(0).label) == p0 + ":");
        CHECK(std::get<std::string>(hints->at(1).label) == p1 + ":");
        CHECK(std::get<std::string>(hints->at(2).label) == p2 + ":");
    }

    // 2. maxParameters = 2
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 2, 0);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(std::get<std::string>(hints->at(0).label) == p0 + ":");
        CHECK(std::get<std::string>(hints->at(1).label) == p1 + ":");
    }

    // 3. maxLength = 5
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 5);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 3);
        CHECK(std::get<std::string>(hints->at(0).label) == p0.substr(0, 5) + "...:");
        CHECK(std::get<std::string>(hints->at(1).label) == p1.substr(0, 5) + "...:");
    }
}

TEST_CASE("InlayHintHandler - Omitted Default Arguments Mode NameAndValue vs Declaration vs Off")
{
    std::string code =
        "void funct(int id = 0, bool f = true, array<string> argS = array<string>()) {}\n"
        "void main() {\n"
        "    funct(f: false);\n"
        "}\n";

    TestEnvironment env(code);

    // 1. NameAndValue mode (default)
    {
        auto hints = env.InlayHints();
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(std::get<std::string>(hints->at(0).label) == "id: 0, ");
        CHECK(std::get<std::string>(hints->at(1).label) == ", argS: array<string>()");
    }

    // 2. Declaration mode
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 0,
                                   config::OmittedDefaultArgumentsMode::Declaration);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(std::get<std::string>(hints->at(0).label) == "int id = 0, ");
        CHECK(std::get<std::string>(hints->at(1).label) == ", array<string> argS = array<string>()");
    }

    // 3. Off mode
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 0,
                                   config::OmittedDefaultArgumentsMode::Off);
        REQUIRE(hints.has_value());
        CHECK(hints->empty());
    }
}

TEST_CASE("InlayHintHandler - Omitted Default Arguments In Empty Call")
{
    std::string code =
        "void testEmpty(int a = 1, float b = 2.0f) {}\n"
        "void main() {\n"
        "    testEmpty();\n"
        "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 2);
    CHECK(std::get<std::string>(hints->at(0).label) == "a: 1");
    CHECK(std::get<std::string>(hints->at(1).label) == ", b: 2.0f");
}

TEST_CASE("InlayHintHandler - Invariant Randomized Omitted Default Arguments")
{
    const std::string fnName = test::GenerateRandomSymbolName("func");
    const std::string p0 = test::GenerateRandomSymbolName("paramFirst");
    const std::string p1 = test::GenerateRandomSymbolName("paramMid");
    const std::string p2 = test::GenerateRandomSymbolName("paramLast");

    std::string code =
        "void " + fnName + "(int " + p0 + " = 10, bool " + p1 + " = true, string " + p2 + " = \"default\") {}\n"
        "void main() {\n"
        "    " + fnName + "(" + p1 + ": false);\n"
        "}\n";

    TestEnvironment env(code);

    // NameAndValue mode
    {
        auto hints = env.InlayHints();
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(std::get<std::string>(hints->at(0).label) == p0 + ": 10, ");
        CHECK(std::get<std::string>(hints->at(1).label) == ", " + p2 + ": \"default\"");
    }

    // Declaration mode
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 0,
                                   config::OmittedDefaultArgumentsMode::Declaration);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(std::get<std::string>(hints->at(0).label) == "int " + p0 + " = 10, ");
        CHECK(std::get<std::string>(hints->at(1).label) == ", string " + p2 + " = \"default\"");
    }
}






