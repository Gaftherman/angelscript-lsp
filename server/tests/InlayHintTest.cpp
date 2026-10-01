#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "features/inlay_hint/InlayHintHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
std::string GetHintLabel(const lsp::InlayHint& hint)
{
    if (std::holds_alternative<std::string>(hint.label))
    {
        return std::get<std::string>(hint.label);
    }
    if (std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label))
    {
        std::string res;
        for (const auto& part : std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label))
        {
            res += part.value;
        }
        return res;
    }
    return "";
}

std::string GetTooltipText(const lsp::Opt<lsp::OneOf<std::string, lsp::MarkupContent>>& opt)
{
    if (!opt.has_value())
    {
        return "";
    }
    if (std::holds_alternative<std::string>(*opt))
    {
        return std::get<std::string>(*opt);
    }
    if (std::holds_alternative<lsp::MarkupContent>(*opt))
    {
        return std::get<lsp::MarkupContent>(*opt).value;
    }
    return "";
}

struct TestEnvironment
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;
    std::string uri = "file:///test.as";
    std::string sourceCode;
    TSTree* tree = nullptr;

    TestEnvironment(const std::string& code) : sourceCode(code)
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
        lsp::Range range = lsp::Range{{0, 0}, {0, 0}}, bool suppressWhenArgumentMatchesName = false,
        size_t maxParameters = 0, size_t maxLength = 0,
        config::OmittedDefaultArgumentsMode omittedDefaultArguments = config::OmittedDefaultArgumentsMode::NameAndValue)
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

    std::optional<std::vector<lsp::InlayHint>> InlayHintsWithConfig(const config::ServerConfig& config,
                                                                    lsp::Range range = lsp::Range{{0, 0}, {0, 0}})
    {
        InlayHintRequest req{uri,
                             sourceCode,
                             tree,
                             range,
                             symbolTable,
                             scopeIndex,
                             false,
                             nullptr,
                             0,
                             0,
                             config.features.inlayHintsOmittedDefaultArguments,
                             &config};
        return GetInlayHints(req);
    }
};
} // namespace

TEST_CASE("InlayHintHandler - Basic Function Call Parameter Hints")
{
    std::string code = "void Test(int a, float b) {}\n"
                       "void main() {\n"
                       "    Test(10, 2.5f);\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() >= 2);

    // First parameter hint: a:
    std::string label0 = GetHintLabel(hints->at(0));
    CHECK(label0 == "a:");
    CHECK(hints->at(0).kind.has_value());
    bool isParam0 = (hints->at(0).kind.value() == lsp::InlayHintKind::Parameter);
    CHECK(isParam0);

    // Second parameter hint: b:
    std::string label1 = GetHintLabel(hints->at(1));
    CHECK(label1 == "b:");
    CHECK(hints->at(1).kind.has_value());
    bool isParam1 = (hints->at(1).kind.value() == lsp::InlayHintKind::Parameter);
    CHECK(isParam1);
}

TEST_CASE("InlayHintHandler - Exclusion Rule: Named Arguments")
{
    std::string code = "void SetValues(int x, int y) {}\n"
                       "void main() {\n"
                       "    SetValues(x: 10, 20);\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    // Only 'y:' should be emitted since 'x' is already explicitly named in syntax
    REQUIRE(hints->size() == 1);
    std::string label = GetHintLabel(hints->at(0));
    CHECK(label == "y:");
}

TEST_CASE("InlayHintHandler - Exclusion Rule: Same-Name Arguments")
{
    std::string code = "void SetDimensions(int width, int height) {}\n"
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

        for (const auto& hint : *hints)
        {
            std::string l = GetHintLabel(hint);
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
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, true);
        REQUIRE(hints.has_value());
        bool foundWidthHint = false;
        bool foundHeightHint = false;

        for (const auto& hint : *hints)
        {
            std::string l = GetHintLabel(hint);
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
    std::string code = "class Base {\n"
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

    for (const auto& hint : *hints)
    {
        std::string l = GetHintLabel(hint);
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
    std::string code = "void main() {\n"
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
    for (const auto& hint : *hints)
    {
        std::string l = GetHintLabel(hint);
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
    std::string code = "class Actor {}\n"
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

    for (const auto& hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label) ? std::get<std::string>(hint.label) : "";
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

TEST_CASE("InlayHintHandler - Auto Handle Variable Deduction with auto@")
{
    const std::string clsName = test::GenerateRandomSymbolName("PlayerActor");
    const std::string funcName = test::GenerateRandomSymbolName("SpawnActor");
    const std::string varName = test::GenerateRandomSymbolName("pActor");

    const std::string code = "class " + clsName +
                             " {}\n"
                             "" +
                             clsName + "@ " + funcName +
                             "() { return null; }\n"
                             "void main() {\n"
                             "    auto@ " +
                             varName + " = " + funcName +
                             "();\n"
                             "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE_FALSE(hints->empty());

    bool foundHandleHint = false;
    for (const auto& hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label) ? std::get<std::string>(hint.label) : "";
        if (l == ": " + clsName + "@")
        {
            foundHandleHint = true;
        }
    }

    CHECK(foundHandleHint);
}

TEST_CASE("InlayHintHandler - Sub-range Filtering")
{
    std::string code = "void Foo(int x) {}\n"
                       "void Bar(int y) {}\n"
                       "void main() {\n"
                       "    Foo(1);\n"
                       "    Bar(2);\n"
                       "}\n";

    TestEnvironment env(code);

    // Range restricting to only line 3 (Foo(1))
    lsp::Range r{{3, 0}, {3, 20}};
    auto hints = env.InlayHints(r);

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 1);
    std::string l = GetHintLabel(hints->at(0));
    CHECK(l == "x:");
}

TEST_CASE("InlayHintHandler - Operator Overload Auto Type Deduction")
{
    std::string code = "class Matrix {\n"
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

    for (const auto& hint : *hints)
    {
        std::string l = std::holds_alternative<std::string>(hint.label) ? std::get<std::string>(hint.label) : "";
        if (l == ": Matrix")
        {
            foundMatrix = true;
            if (hint.tooltip.has_value())
            {
                std::string t = GetTooltipText(hint.tooltip);
                CHECK(t == "Deduced type: Matrix");
            }
        }
        if (l == ": Vector")
        {
            foundVector = true;
            if (hint.tooltip.has_value())
            {
                std::string t = GetTooltipText(hint.tooltip);
                CHECK(t == "Deduced type: Vector");
            }
        }
    }

    CHECK(foundMatrix);
    CHECK(foundVector);
}

TEST_CASE("InlayHintHandler - Robustness with Empty / Null Tree")
{
    InlayHintRequest req{"file:///empty.as", "", nullptr, lsp::Range{}, SymbolTable{}, ScopeIndex{}};
    auto hints = GetInlayHints(req);
    CHECK(!hints.has_value());
}

TEST_CASE("InlayHintHandler - ShootGrenade and trailing parameter hints")
{
    std::string code = "namespace INS2GLPROJECTILE {\n"
                       "    void ShootGrenade(int pevOwner, int vecStart, int vecVelocity, float dmg, string model, "
                       "bool bRocketExplosions = false, const string& in szName = \"proj_ins2gl\") {}\n"
                       "}\n"
                       "void main() {\n"
                       "    INS2GLPROJECTILE::ShootGrenade(1, 2, 3, 4.0f, \"gmodel\", false, \"proj_name\");\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());
    std::vector<std::string> labels;
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }
    CHECK(std::find(labels.begin(), labels.end(), "skiplocal:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "speed:") != labels.end());
}

TEST_CASE("InlayHintHandler - Namespaced class this.Method parameter hints")
{
    std::string code = "namespace INS2_L85A2 {\n"
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }
    CHECK(std::find(labels.begin(), labels.end(), "anim:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "body:") != labels.end());
}

TEST_CASE("InlayHintHandler - Constructor Direct-Initialization Parameter Hints")
{
    std::string code = "class NetworkMessage {\n"
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }

    CHECK(labels.size() == 3);
    CHECK(labels[0] == "msg_type:");
    CHECK(labels[1] == "svc_message:");
    CHECK(labels[2] == "pEdict:");
}

TEST_CASE("InlayHintHandler - Mixin Method Parameter Hints")
{
    std::string code = "mixin class PlayerMixin {\n"
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pPlayer:") != labels.end());
}

TEST_CASE("InlayHintHandler - Mixin Method Parameter Hints on Instance")
{
    std::string code = "mixin class PlayerMixin {\n"
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pPlayer:") != labels.end());
}

TEST_CASE("InlayHintHandler - BaseClass Method Parameter Hints with Mixin in Hierarchy")
{
    std::string code = "mixin class WeaponMixin {\n"
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pPlayer:") != labels.end());
}

TEST_CASE("InlayHintHandler - Math Utility Object Parameter Hints")
{
    std::string code = "class Math {\n"
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), "pitch:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "yaw:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "roll:") != labels.end());
}

TEST_CASE("InlayHintHandler - Relaxed Parameter Name Matching Suppression")
{
    std::string code = "void DoSomething(int value) {}\n"
                       "void main() {\n"
                       "    int value = 5;\n"
                       "    DoSomething(value);\n"
                       "}\n";

    TestEnvironment env(code);
    // With explicit suppression (true), "value:" should be suppressed because arg.text == param.name
    auto suppressedHints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, true);
    REQUIRE(suppressedHints.has_value());
    bool foundSuppressed = false;
    for (const auto& h : *suppressedHints)
    {
        if (GetHintLabel(h) == "value:")
        {
            foundSuppressed = true;
        }
    }
    CHECK(!foundSuppressed);

    // With relaxed suppression (false), "value:" hint should be provided
    auto relaxedHints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false);
    REQUIRE(relaxedHints.has_value());
    bool foundRelaxed = false;
    for (const auto& h : *relaxedHints)
    {
        if (GetHintLabel(h) == "value:")
        {
            foundRelaxed = true;
        }
    }
    CHECK(foundRelaxed);

    // Default call without arguments also defaults to false (no suppression)
    auto defaultHints = env.InlayHints();
    REQUIRE(defaultHints.has_value());
    bool foundDefault = false;
    for (const auto& h : *defaultHints)
    {
        if (GetHintLabel(h) == "value:")
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
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
    for (const auto& h : *hints)
    {
        {

            std::string l = GetHintLabel(h);

            if (!l.empty())
                labels.push_back(l);
        }
    }
    CHECK(std::find(labels.begin(), labels.end(), "channel:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "pitch:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "low:") != labels.end());
    CHECK(std::find(labels.begin(), labels.end(), "high:") != labels.end());
}

TEST_CASE("InlayHintHandler - Call With Default Parameters Retains All Provided Argument Hints")
{
    std::string code = "void SetProperties(int width, int height, bool fullscreen = false, int refreshRate = 60) {}\n"
                       "void main() {\n"
                       "    SetProperties(1920, 1080, true);\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 4);

    std::string label0 = GetHintLabel(hints->at(0));
    std::string label1 = GetHintLabel(hints->at(1));
    std::string label2 = GetHintLabel(hints->at(2));
    std::string label3 = GetHintLabel(hints->at(3));

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
    std::string code = "void ConfigureLogger(bool shouldTrace, string longParameterIdentifier) {}\n"
                       "void main() {\n"
                       "    ConfigureLogger(true, \"test\");\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 2);

    std::string label0 = GetHintLabel(hints->at(0));
    std::string label1 = GetHintLabel(hints->at(1));

    CHECK(label0 == "shouldTrace:");
    CHECK(label1 == "longParameterIdentifier:");
}

TEST_CASE("InlayHintHandler - ShootProp with 13 parameters returns all parameter hints by default")
{
    std::string code = "namespace HCASPROP {\n"
                       "    CHCASProp@ ShootProp( entvars_t@ pevOwner, Vector& in vecOrigin, Vector& in vecVelocity, "
                       "Vector& in vecDropAngle, Vector& in vecAngVelocity, string szModel, array<string> "
                       "BounceSounds, float flVelFriction = 0.4f, float flAVelFriction = 0.7f, int iBodygroup = 0, int "
                       "iSkingroup = 0, float flStartFadeOutTime = 5.0f, string szPropName = \"proj_hcasprop\" ) {}\n"
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

    auto getLabel = [&](size_t idx) -> std::string { return GetHintLabel(hints->at(idx)); };

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
    std::string code = "namespace HCASPROP {\n"
                       "    CHCASProp@ ShootProp( entvars_t@ pevOwner, Vector& in vecOrigin, Vector& in vecVelocity, "
                       "Vector& in vecDropAngle, Vector& in vecAngVelocity, string szModel, array<string> "
                       "BounceSounds, float flVelFriction = 0.4f, float flAVelFriction = 0.7f, int iBodygroup = 0, int "
                       "iSkingroup = 0, float flStartFadeOutTime = 5.0f, string szPropName = \"proj_hcasprop\" ) {}\n"
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

    auto getLabel = [&](size_t idx) -> std::string { return GetHintLabel(hints->at(idx)); };

    CHECK(getLabel(1) == "pevOwner:");
    CHECK(getLabel(5) == "vecAngVelocity:");
}

TEST_CASE("InlayHintHandler - maxLength truncates parameter labels")
{
    std::string code = "namespace HCASPROP {\n"
                       "    CHCASProp@ ShootProp( entvars_t@ pevOwner, Vector& in vecOrigin, Vector& in vecVelocity, "
                       "Vector& in vecDropAngle, Vector& in vecAngVelocity, string szModel, array<string> "
                       "BounceSounds, float flVelFriction = 0.4f, float flAVelFriction = 0.7f, int iBodygroup = 0, int "
                       "iSkingroup = 0, float flStartFadeOutTime = 5.0f, string szPropName = \"proj_hcasprop\" ) {}\n"
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

    auto getLabel = [&](size_t idx) -> std::string { return GetHintLabel(hints->at(idx)); };

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

    std::string code = "void " + fnName + "(int " + p0 + ", int " + p1 + ", int " + p2 +
                       ") {}\n"
                       "void main() {\n"
                       "    " +
                       fnName +
                       "(1, 2, 3);\n"
                       "}\n";

    TestEnvironment env(code);

    // 1. Unlimited by default
    {
        auto hints = env.InlayHints();
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 3);
        CHECK(GetHintLabel(hints->at(0)) == p0 + ":");
        CHECK(GetHintLabel(hints->at(1)) == p1 + ":");
        CHECK(GetHintLabel(hints->at(2)) == p2 + ":");
    }

    // 2. maxParameters = 2
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 2, 0);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(GetHintLabel(hints->at(0)) == p0 + ":");
        CHECK(GetHintLabel(hints->at(1)) == p1 + ":");
    }

    // 3. maxLength = 5
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 5);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 3);
        CHECK(GetHintLabel(hints->at(0)) == p0.substr(0, 5) + "...:");
        CHECK(GetHintLabel(hints->at(1)) == p1.substr(0, 5) + "...:");
    }
}

TEST_CASE("InlayHintHandler - Omitted Default Arguments Mode NameAndValue vs Declaration vs Off")
{
    std::string code = "void funct(int id = 0, bool f = true, array<string> argS = array<string>()) {}\n"
                       "void main() {\n"
                       "    funct(f: false);\n"
                       "}\n";

    TestEnvironment env(code);

    // 1. NameAndValue mode (default)
    {
        auto hints = env.InlayHints();
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(GetHintLabel(hints->at(0)) == "id: 0, ");
        CHECK(GetHintLabel(hints->at(1)) == ", argS: array<string>()");
    }

    // 2. Declaration mode
    {
        auto hints =
            env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 0, config::OmittedDefaultArgumentsMode::Declaration);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(GetHintLabel(hints->at(0)) == "int id = 0, ");
        CHECK(GetHintLabel(hints->at(1)) == ", array<string> argS = array<string>()");
    }

    // 3. Off mode
    {
        auto hints = env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 0, config::OmittedDefaultArgumentsMode::Off);
        REQUIRE(hints.has_value());
        CHECK(hints->empty());
    }
}

TEST_CASE("InlayHintHandler - Omitted Default Arguments In Empty Call")
{
    std::string code = "void testEmpty(int a = 1, float b = 2.0f) {}\n"
                       "void main() {\n"
                       "    testEmpty();\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE(hints->size() == 2);
    CHECK(GetHintLabel(hints->at(0)) == "a: 1");
    CHECK(GetHintLabel(hints->at(1)) == ", b: 2.0f");
}

TEST_CASE("InlayHintHandler - Invariant Randomized Omitted Default Arguments")
{
    const std::string fnName = test::GenerateRandomSymbolName("func");
    const std::string p0 = test::GenerateRandomSymbolName("paramFirst");
    const std::string p1 = test::GenerateRandomSymbolName("paramMid");
    const std::string p2 = test::GenerateRandomSymbolName("paramLast");

    std::string code = "void " + fnName + "(int " + p0 + " = 10, bool " + p1 + " = true, string " + p2 +
                       " = \"default\") {}\n"
                       "void main() {\n"
                       "    " +
                       fnName + "(" + p1 +
                       ": false);\n"
                       "}\n";

    TestEnvironment env(code);

    // NameAndValue mode
    {
        auto hints = env.InlayHints();
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(GetHintLabel(hints->at(0)) == p0 + ": 10, ");
        CHECK(GetHintLabel(hints->at(1)) == ", " + p2 + ": \"default\"");
    }

    // Declaration mode
    {
        auto hints =
            env.InlayHints(lsp::Range{{0, 0}, {0, 0}}, false, 0, 0, config::OmittedDefaultArgumentsMode::Declaration);
        REQUIRE(hints.has_value());
        REQUIRE(hints->size() == 2);
        CHECK(GetHintLabel(hints->at(0)) == "int " + p0 + " = 10, ");
        CHECK(GetHintLabel(hints->at(1)) == ", string " + p2 + " = \"default\"");
    }
}

TEST_CASE("InlayHint - Scoped calls and enum arguments")
{
    const std::string nsHud = test::GenerateRandomSymbolName("HUD");
    const std::string nsMoney = test::GenerateRandomSymbolName("MONEY");
    const std::string fnUpdate = test::GenerateRandomSymbolName("Update");
    const std::string nsStore = test::GenerateRandomSymbolName("Store");
    const std::string enumName = test::GenerateRandomSymbolName("KeyType");
    const std::string enumMember = test::GenerateRandomSymbolName("KEY_MONEY");
    const std::string fnGet = test::GenerateRandomSymbolName("Get");
    const std::string playerClass = test::GenerateRandomSymbolName("CBasePlayer");
    const std::string paramPlayer = test::GenerateRandomSymbolName("pPlayer");
    const std::string paramVal = test::GenerateRandomSymbolName("iValue");
    const std::string paramKey = test::GenerateRandomSymbolName("key");

    std::string code = "class " + playerClass +
                       " {}\n"
                       "enum " +
                       enumName + " { " + enumMember +
                       " }\n"
                       "namespace " +
                       nsStore +
                       " {\n"
                       "    int " +
                       fnGet + "(" + enumName + " " + paramKey + ", " + playerClass + "@ " + paramPlayer +
                       ") { return 0; }\n"
                       "}\n"
                       "namespace " +
                       nsHud +
                       " {\n"
                       "    namespace " +
                       nsMoney +
                       " {\n"
                       "        void " +
                       fnUpdate + "(" + playerClass + "@ " + paramPlayer + ", int " + paramVal +
                       ") {}\n"
                       "    }\n"
                       "}\n"
                       "void main(" +
                       playerClass +
                       "@ target) {\n"
                       "    " +
                       nsHud + "::" + nsMoney + "::" + fnUpdate + "(target, " + nsStore + "::" + fnGet + "(" +
                       enumName + "::" + enumMember +
                       ", target));\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());
    REQUIRE(!hints->empty());

    bool foundUpdatePlayer = false;
    bool foundUpdateVal = false;
    bool foundGetKey = false;

    for (const auto& hint : *hints)
    {
        {
            std::string label = GetHintLabel(hint);
            if (label == paramPlayer + ":")
            {
                foundUpdatePlayer = true;
            }
            else if (label == paramVal + ":")
            {
                foundUpdateVal = true;
            }
            else if (label == paramKey + ":")
            {
                foundGetKey = true;
            }
        }
    }

    CHECK(foundUpdatePlayer);
    CHECK(foundUpdateVal);
    CHECK(foundGetKey);
}

TEST_CASE("InlayHintHandler - Omitted default parameter in non-empty argument list carries hover tooltip")
{
    const std::string loggerClass = test::GenerateRandomSymbolName("ASLogger");
    const std::string msgParam = test::GenerateRandomSymbolName("message");
    const std::string argsParam = test::GenerateRandomSymbolName("arguments");

    const std::string code = "class " + loggerClass +
                             " {\n"
                             "    void print(const string &in " +
                             msgParam + ", array<string>@ " + argsParam +
                             " = null) const {}\n"
                             "};\n"
                             "void main() {\n"
                             "    " +
                             loggerClass +
                             " logger;\n"
                             "    logger.print(\"hello\");\n"
                             "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE_FALSE(hints->empty());

    bool foundOmittedHintWithTooltip = false;
    for (const auto& hint : *hints)
    {
        std::string label = GetHintLabel(hint);
        if (label.find(argsParam + ": null") != std::string::npos)
        {
            REQUIRE(std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label));
            const auto& parts = std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label);
            REQUIRE_FALSE(parts.empty());
            REQUIRE(parts[0].tooltip.has_value());
            std::string tooltip = GetTooltipText(parts[0].tooltip);
            CHECK(tooltip.find("Default parameter:") != std::string::npos);
            CHECK(tooltip.find(argsParam) != std::string::npos);
            CHECK(tooltip.find("null") != std::string::npos);
            REQUIRE(parts[0].location.has_value());
            CHECK(parts[0].location->range.start.line == 1);
            CHECK(parts[0].location->range.end.line == 1);
            CHECK_FALSE(hint.tooltip.has_value());
            foundOmittedHintWithTooltip = true;
        }
    }

    CHECK(foundOmittedHintWithTooltip);
}

TEST_CASE("InlayHintHandler - Nameless wildcard parameter ?& in generates fallback type inlay hint")
{
    const std::string funcName = test::GenerateRandomSymbolName("my_snprintf");
    const std::string bufParam = test::GenerateRandomSymbolName("szOutBuffer");
    const std::string fmtParam = test::GenerateRandomSymbolName("szFormat");
    const std::string keyVar = test::GenerateRandomSymbolName("keyName");

    const std::string code = "bool " + funcName + "(string& out " + bufParam + ", const string& in " + fmtParam +
                             ", ?& in) { return true; }\n"
                             "void main() {\n"
                             "    string outBuf;\n"
                             "    string " +
                             keyVar +
                             " = \"myKey\";\n"
                             "    " +
                             funcName + "(outBuf, \"format %1\", " + keyVar +
                             ");\n"
                             "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();

    REQUIRE(hints.has_value());
    REQUIRE_FALSE(hints->empty());

    bool foundWildcardHint = false;
    for (const auto& hint : *hints)
    {
        std::string label = GetHintLabel(hint);
        if (label.find("?:") != std::string::npos || label.find("?& in:") != std::string::npos ||
            label.find("?&in:") != std::string::npos)
        {
            foundWildcardHint = true;
            REQUIRE(std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label));
            const auto& parts = std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label);
            REQUIRE_FALSE(parts.empty());
            REQUIRE(parts[0].location.has_value());
            CHECK_FALSE(parts[0].tooltip.has_value());
            CHECK_FALSE(hint.tooltip.has_value());
        }
    }

    CHECK(foundWildcardHint);
}

TEST_CASE("InlayHintHandler - Parameter hint label part location navigates to callee parameter declaration")
{
    const std::string msgClass = test::GenerateRandomSymbolName("NetworkMsg");
    const std::string nsName = test::GenerateRandomSymbolName("MsgTypes");
    const std::string enumVal = test::GenerateRandomSymbolName("ShieldRic");
    const std::string destParam = test::GenerateRandomSymbolName("dest");
    const std::string typeParam = test::GenerateRandomSymbolName("type");
    const std::string edictParam = test::GenerateRandomSymbolName("pEdict");

    const std::string code = "namespace " + nsName +
                             " {\n"
                             "    enum Type { " +
                             enumVal +
                             " = 1 };\n"
                             "}\n"
                             "class " +
                             msgClass +
                             " {\n"
                             "    " +
                             msgClass + "(int " + destParam + ", " + nsName + "::Type " + typeParam + ", int " +
                             edictParam +
                             " = 0) {}\n"
                             "}\n"
                             "void main() {\n"
                             "    " +
                             msgClass + " m(1, " + nsName + "::" + enumVal +
                             ");\n"
                             "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());

    bool foundTypeHintWithDeclLocation = false;
    for (const auto& hint : *hints)
    {
        if (!std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label))
        {
            continue;
        }
        const auto& parts = std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label);
        for (const auto& part : parts)
        {
            if (part.value == typeParam + ":")
            {
                REQUIRE(part.location.has_value());
                const auto& range = part.location->range;
                CHECK(range.start.line == 4);
                CHECK(range.end.line == 4);
                CHECK_FALSE(part.tooltip.has_value());
                CHECK_FALSE(hint.tooltip.has_value());
                foundTypeHintWithDeclLocation = true;
            }
        }
    }
    CHECK(foundTypeHintWithDeclLocation);
}

TEST_CASE("InlayHintHandler - Tooltip uses angelscript code block and respects config toggles")
{
    const std::string funcName = test::GenerateRandomSymbolName("SpawnGrenade");
    const std::string paramName = test::GenerateRandomSymbolName("startEntity");

    const std::string code = "class CBaseEntity {};\n"
                             "void " +
                             funcName + "(CBaseEntity@ " + paramName +
                             ") {}\n"
                             "void main() {\n"
                             "    CBaseEntity@ ent = null;\n"
                             "    " +
                             funcName +
                             "(ent);\n"
                             "}\n";

    TestEnvironment env(code);

    // 1. Default config: tooltip should be wrapped in ```angelscript code block
    auto defaultHints = env.InlayHints();
    REQUIRE(defaultHints.has_value());
    REQUIRE_FALSE(defaultHints->empty());

    bool foundParamHint = false;
    for (const auto& hint : *defaultHints)
    {
        if (!std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label))
        {
            continue;
        }
        const auto& parts = std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label);
        for (const auto& part : parts)
        {
            if (part.value == paramName + ":")
            {
                foundParamHint = true;
                // With location available, tooltip is omitted to prevent duplicate cards in VS Code
                CHECK(part.location.has_value());
                CHECK_FALSE(part.tooltip.has_value());
            }
        }
    }
    CHECK(foundParamHint);

    // 2. Disabled tooltip: part.tooltip should be nullopt
    config::ServerConfig noTooltipConfig;
    noTooltipConfig.features.inlayHintsEnableTooltip = false;
    auto noTooltipHints = env.InlayHintsWithConfig(noTooltipConfig);
    REQUIRE(noTooltipHints.has_value());
    for (const auto& hint : *noTooltipHints)
    {
        if (std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label))
        {
            for (const auto& part : std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label))
            {
                CHECK_FALSE(part.tooltip.has_value());
            }
        }
    }

    // 3. Disabled location: part.location is nullopt and fallback part.tooltip is provided with (parameter) format
    config::ServerConfig noLocationConfig;
    noLocationConfig.features.inlayHintsEnableLocation = false;
    auto noLocationHints = env.InlayHintsWithConfig(noLocationConfig);
    REQUIRE(noLocationHints.has_value());
    bool foundNoLocParamHint = false;
    for (const auto& hint : *noLocationHints)
    {
        if (std::holds_alternative<std::vector<lsp::InlayHintLabelPart>>(hint.label))
        {
            for (const auto& part : std::get<std::vector<lsp::InlayHintLabelPart>>(hint.label))
            {
                CHECK_FALSE(part.location.has_value());
                if (part.value == paramName + ":")
                {
                    foundNoLocParamHint = true;
                    REQUIRE(part.tooltip.has_value());
                    std::string tooltip = GetTooltipText(part.tooltip);
                    CHECK(tooltip.find("(parameter) CBaseEntity@ " + paramName) != std::string::npos);
                }
            }
        }
    }
    CHECK(foundNoLocParamHint);
}

TEST_CASE("InlayHintHandler - Constructor call expression parameter hints")
{
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("MenuOption");
    const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("owner");
    const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Menu");

    std::string code = "class " + typeName +
                       " {}\n"
                       "class " +
                       className +
                       " {\n"
                       "    " +
                       className + "(" + typeName + "@ " + paramName +
                       ") {}\n"
                       "}\n"
                       "void main() {\n"
                       "    " +
                       typeName +
                       "@ m;\n"
                       "    " +
                       className + "@ opt = " + className +
                       "(m);\n"
                       "}\n";

    TestEnvironment env(code);
    auto hints = env.InlayHints();
    REQUIRE(hints.has_value());

    std::vector<std::string> labels;
    for (const auto& h : *hints)
    {
        std::string l = GetHintLabel(h);
        if (!l.empty())
        {
            labels.push_back(l);
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), paramName + ":") != labels.end());
}

TEST_CASE("InlayHintHandler - Omitted default arguments disabled by default")
{
    const std::string funcName = angel_lsp::test::GenerateRandomSymbolName("PlayAnim");
    const std::string param1 = angel_lsp::test::GenerateRandomSymbolName("anim");
    const std::string param2 = angel_lsp::test::GenerateRandomSymbolName("player_anim");

    std::string code = "void " + funcName + "(int " + param1 + ", int " + param2 +
                       " = 42) {}\n"
                       "void main() {\n"
                       "    " +
                       funcName +
                       "(10);\n"
                       "}\n";

    TestEnvironment env(code);
    config::ServerConfig defaultConfig;
    auto hints = env.InlayHintsWithConfig(defaultConfig);
    REQUIRE(hints.has_value());

    std::vector<std::string> labels;
    for (const auto& h : *hints)
    {
        std::string l = GetHintLabel(h);
        if (!l.empty())
        {
            labels.push_back(l);
        }
    }

    CHECK(std::find(labels.begin(), labels.end(), param1 + ":") != labels.end());
    CHECK(std::find_if(labels.begin(), labels.end(),
                       [&](const std::string& l) { return l.find(param2) != std::string::npos; }) == labels.end());
}
