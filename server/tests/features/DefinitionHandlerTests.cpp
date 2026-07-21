#include <doctest/doctest.h>
#include <iostream>
#include <vector>
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "features/definition/DefinitionHandler.h"

using namespace angel_lsp;

static std::vector<lsp::Location> GetDefinitionLocations(const char *src, const std::string &searchStr, int offsetFromStart = 0)
{
    std::string uriStr = "file:///test_definition.as";
    Document doc(uriStr, src);
    analysis::SymbolTable table;
    analysis::SymbolCollector::CollectGlobals(doc, table);
    analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

    size_t offset = std::string(src).find(searchStr) + offsetFromStart;
    uint32_t line = 0, col = 0;
    for (size_t i = 0; i < offset; i++)
    {
        if (src[i] == '\n')
        {
            line++;
            col = 0;
        }
        else
        {
            col++;
        }
    }

    lsp::requests::TextDocument_Definition::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse(uriStr);
    req.position.line = line;
    req.position.character = col;

    auto result = features::ProcessDefinition(req, doc, table, nullptr);
    std::vector<lsp::Location> locs;
    if (!result.isNull())
    {
        if (const auto *def = std::get_if<lsp::Definition>(&*result))
        {
            if (const auto *loc = std::get_if<lsp::Location>(def))
            {
                locs.push_back(*loc);
            }
            else if (const auto *arr = std::get_if<lsp::Array<lsp::Location>>(def))
            {
                locs = *arr;
            }
        }
    }
    return locs;
}

static std::vector<lsp::Location> GetTypeDefinitionLocations(const char *src, const std::string &searchStr, int offsetFromStart = 0)
{
    std::string uriStr = "file:///test_typedefinition.as";
    Document doc(uriStr, src);
    analysis::SymbolTable table;
    analysis::SymbolCollector::CollectGlobals(doc, table);
    analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

    size_t offset = std::string(src).find(searchStr) + offsetFromStart;
    uint32_t line = 0, col = 0;
    for (size_t i = 0; i < offset; i++)
    {
        if (src[i] == '\n')
        {
            line++;
            col = 0;
        }
        else
        {
            col++;
        }
    }

    lsp::requests::TextDocument_TypeDefinition::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse(uriStr);
    req.position.line = line;
    req.position.character = col;

    auto result = features::ProcessTypeDefinition(req, doc, table, nullptr);
    std::vector<lsp::Location> locs;
    if (!result.isNull())
    {
        if (const auto *def = std::get_if<lsp::Definition>(&*result))
        {
            if (const auto *loc = std::get_if<lsp::Location>(def))
            {
                locs.push_back(*loc);
            }
            else if (const auto *arr = std::get_if<lsp::Array<lsp::Location>>(def))
            {
                locs = *arr;
            }
        }
    }
    return locs;
}

TEST_CASE("DefinitionHandler - Go to Definition for Variables, Parameters & Fields")
{
    const char *SRC = R"script(
class Vector3
{
    float x;
    float y;
    float z;

    Vector3(float ax, float ay, float az)
    {
        x = ax;
        y = ay;
        z = az;
    }
}

void ProcessPoint(Vector3 pt)
{
    float valX = pt.x;
    Vector3 copyVec = pt;
}
)script";

    // 1. Parameter usage `ax` in constructor body -> param declaration `float ax` (Line 7)
    auto locsParam = GetDefinitionLocations(SRC, "x = ax;", 4);
    REQUIRE(locsParam.size() == 1);
    CHECK(locsParam[0].range.start.line == 7);

    // 2. Field usage `x` in constructor body -> field `float x;` (Line 3)
    auto locsField = GetDefinitionLocations(SRC, "x = ax;", 0);
    REQUIRE(locsField.size() == 1);
    CHECK(locsField[0].range.start.line == 3);

    // 3. Local variable usage `copyVec` -> (Line 18)
    auto locsLocal = GetDefinitionLocations(SRC, "copyVec =", 0);
    REQUIRE(locsLocal.size() == 1);
    CHECK(locsLocal[0].range.start.line == 18);
}

TEST_CASE("DefinitionHandler - Go to Definition for Classes, Interfaces, Mixins & Inheritance")
{
    const char *SRC = R"script(
interface IComponent
{
    void Update(float dt);
}

mixin class Transformable
{
    float posX;
    float posY;
}

class BaseComponent : IComponent
{
    void Update(float dt) {}
    void Destroy() {}
}

class RigidBody : BaseComponent
{
    void Step()
    {
        Destroy();
    }
}

void TestApp()
{
    RigidBody body;
    body.Update(0.016f);
    body.Destroy();
}
)script";

    // 1. Interface reference `IComponent` -> interface declaration (Line 1)
    auto locsIfaces = GetDefinitionLocations(SRC, "BaseComponent : IComponent", 16);
    REQUIRE(locsIfaces.size() == 1);
    CHECK(locsIfaces[0].range.start.line == 1);

    // 2. Base class in inheritance list `BaseComponent` -> class declaration (Line 12)
    auto locsBase = GetDefinitionLocations(SRC, "RigidBody : BaseComponent", 12);
    REQUIRE(locsBase.size() == 1);
    CHECK(locsBase[0].range.start.line == 12);

    // 3. Inherited method call `Destroy()` inside subclass -> `void Destroy()` in BaseComponent (Line 15)
    auto locsInherited = GetDefinitionLocations(SRC, "Destroy();", 0);
    REQUIRE(locsInherited.size() == 1);
    CHECK(locsInherited[0].range.start.line == 15);

    // 4. Object instantiation `RigidBody body;` -> `class RigidBody` (Line 18)
    auto locsClassUse = GetDefinitionLocations(SRC, "RigidBody body;", 0);
    REQUIRE(locsClassUse.size() == 1);
    CHECK(locsClassUse[0].range.start.line == 18);
}

TEST_CASE("DefinitionHandler - Go to Definition for Enums, Enum Members, Typedefs & Funcdefs")
{
    const char *SRC = R"script(
typedef float Real;

enum RenderPriority
{
    PRIORITY_BACKGROUND = 0,
    PRIORITY_FOREGROUND = 100
}

funcdef void EventCallback(int eventId);

void Setup()
{
    Real speed = 5.0f;
    RenderPriority p = PRIORITY_FOREGROUND;
    EventCallback@ cb = null;
}
)script";

    // 1. Typedef `Real` -> `typedef float Real;` (Line 1)
    auto locsTypedef = GetDefinitionLocations(SRC, "Real speed", 0);
    REQUIRE(locsTypedef.size() == 1);
    CHECK(locsTypedef[0].range.start.line == 1);

    // 2. Enum type `RenderPriority` -> `enum RenderPriority` (Line 3)
    auto locsEnum = GetDefinitionLocations(SRC, "RenderPriority p", 0);
    REQUIRE(locsEnum.size() == 1);
    CHECK(locsEnum[0].range.start.line == 3);

    // 3. Enum member `PRIORITY_FOREGROUND` -> `PRIORITY_FOREGROUND = 100` (Line 6)
    auto locsEnumMem = GetDefinitionLocations(SRC, "PRIORITY_FOREGROUND;", 0);
    REQUIRE(locsEnumMem.size() == 1);
    CHECK(locsEnumMem[0].range.start.line == 6);

    // 4. Funcdef `EventCallback` -> `funcdef void EventCallback...` (Line 9)
    auto locsFuncdef = GetDefinitionLocations(SRC, "EventCallback@ cb", 0);
    REQUIRE(locsFuncdef.size() == 1);
    CHECK(locsFuncdef[0].range.start.line == 9);
}

TEST_CASE("DefinitionHandler - Go to Definition for Overloaded Methods & Nested Namespaces")
{
    const char *SRC = R"script(
namespace Engine
{
    namespace Math
    {
        class Vector3
        {
            Vector3() {}
            Vector3(float x, float y, float z) {}
        }
    }

    namespace Physics
    {
        class RigidBody
        {
            void ApplyImpulse(Math::Vector3 force) {}
            void ApplyImpulse(Math::Vector3 force, float duration) {}
            void ApplyImpulse(float fx, float fy, float fz) {}
        }
    }
}

using namespace Engine::Physics;

void RunPhysics()
{
    Engine::Math::Vector3 v;
    RigidBody body;
    body.ApplyImpulse(v);
    body.ApplyImpulse(v, 0.5f);
}
)script";

    // 1. Namespace `Engine` in `Engine::Math` -> `namespace Engine` (Line 1)
    auto locsNsEngine = GetDefinitionLocations(SRC, "Engine::Math", 0);
    REQUIRE(locsNsEngine.size() == 1);
    CHECK(locsNsEngine[0].range.start.line == 1);

    // 2. Namespace `Math` in `Engine::Math` -> `namespace Math` (Line 3)
    auto locsNsMath = GetDefinitionLocations(SRC, "Engine::Math", 8);
    REQUIRE(locsNsMath.size() == 1);
    CHECK(locsNsMath[0].range.start.line == 3);

    // 3. Method overload 1 call `body.ApplyImpulse(v);` -> 1-param overload (Line 16)
    auto locsOverload1 = GetDefinitionLocations(SRC, "ApplyImpulse(v);", 0);
    REQUIRE(locsOverload1.size() >= 1);
    CHECK(locsOverload1[0].range.start.line == 16);

    // 4. Method overload 2 call `body.ApplyImpulse(v, 0.5f);` -> 2-param overload (Line 17)
    auto locsOverload2 = GetDefinitionLocations(SRC, "ApplyImpulse(v, 0.5f);", 0);
    REQUIRE(locsOverload2.size() >= 1);
    CHECK(locsOverload2[0].range.start.line == 17);
}

TEST_CASE("DefinitionHandler - Go to Type Definition for Variables, Parameters & Auto")
{
    const char *SRC = R"script(
class Vector3
{
    float x;
    float y;
    float z;
}

class RigidBody
{
    Vector3 pos;

    Vector3 GetPosition()
    {
        return pos;
    }
}

void ProcessBody(RigidBody bodyArg)
{
    RigidBody localBody;
    auto autoPos = localBody.GetPosition();
}
)script";

    // 1. Go to Type Definition on `bodyArg` parameter -> `class RigidBody` (Line 8)
    auto locsTypeParam = GetTypeDefinitionLocations(SRC, "bodyArg)", 0);
    REQUIRE(locsTypeParam.size() == 1);
    CHECK(locsTypeParam[0].range.start.line == 8);

    // 2. Go to Type Definition on `localBody` variable -> `class RigidBody` (Line 8)
    auto locsTypeLocal = GetTypeDefinitionLocations(SRC, "localBody;", 0);
    REQUIRE(locsTypeLocal.size() == 1);
    CHECK(locsTypeLocal[0].range.start.line == 8);

    // 3. Go to Type Definition on `pos` field -> `class Vector3` (Line 1)
    auto locsTypeField = GetTypeDefinitionLocations(SRC, "pos;", 0);
    REQUIRE(locsTypeField.size() == 1);
    CHECK(locsTypeField[0].range.start.line == 1);

    // 4. Go to Type Definition directly on `class RigidBody` -> `class RigidBody` (Line 8)
    auto locsTypeClass = GetTypeDefinitionLocations(SRC, "class RigidBody", 6);
    REQUIRE(locsTypeClass.size() == 1);
    CHECK(locsTypeClass[0].range.start.line == 8);
}

TEST_CASE("DefinitionHandler - Deep Edge Cases: Cast, Multi-Inheritance, Shadowing, This & Using Namespaces")
{
    const char *SRC = R"script(
namespace Audio { class Sound {} }
namespace Graphics { class Sprite {} }
using namespace Audio;
using namespace Graphics;

class GrandParent
{
    void RootAction() {}
}

class Parent : GrandParent
{
}

class Child : Parent
{
    float val = 1.0f;

    void Perform()
    {
        this.val = 2.0f;
        RootAction();
    }
}

void EdgeCaseTest()
{
    Sound snd;
    Sprite sp;
    GrandParent@ gp = cast<GrandParent>(Child());

    int x = 100;
    for (int x = 0; x < 5; x++)
    {
        int y = x + 1;
    }
}
)script";

    // 1. Sound via using namespace Audio -> `class Sound` (Line 1)
    auto locsSound = GetDefinitionLocations(SRC, "Sound snd;", 0);
    REQUIRE(locsSound.size() == 1);
    CHECK(locsSound[0].range.start.line == 1);

    // 2. Sprite via using namespace Graphics -> `class Sprite` (Line 2)
    auto locsSprite = GetDefinitionLocations(SRC, "Sprite sp;", 0);
    REQUIRE(locsSprite.size() == 1);
    CHECK(locsSprite[0].range.start.line == 2);

    // 3. Cast target `GrandParent` -> `class GrandParent` (Line 6)
    auto locsCast = GetDefinitionLocations(SRC, "cast<GrandParent>", 5);
    REQUIRE(locsCast.size() == 1);
    CHECK(locsCast[0].range.start.line == 6);

    // 4. Member access on `this.val` -> `float val` (Line 17)
    auto locsThisVal = GetDefinitionLocations(SRC, "this.val", 5);
    REQUIRE(locsThisVal.size() == 1);
    CHECK(locsThisVal[0].range.start.line == 17);

    // 5. 3-Level Deep Inheritance `RootAction()` -> `void RootAction()` in GrandParent (Line 8)
    auto locsGrandParentAction = GetDefinitionLocations(SRC, "RootAction();", 0);
    REQUIRE(locsGrandParentAction.size() == 1);
    CHECK(locsGrandParentAction[0].range.start.line == 8);

    // 6. For-loop Variable Shadowing `x + 1` -> `for (int x = 0` (Line 33)
    auto locsShadow = GetDefinitionLocations(SRC, "x + 1", 0);
    REQUIRE(locsShadow.size() == 1);
    CHECK(locsShadow[0].range.start.line == 33);
}
