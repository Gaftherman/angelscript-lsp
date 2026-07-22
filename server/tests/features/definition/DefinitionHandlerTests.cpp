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

    auto result = features::ProcessDefinition(req, doc, table);
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

    auto result = features::ProcessTypeDefinition(req, doc, table);
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

TEST_CASE("DefinitionHandler - 5-Level Deep Namespaces & Complex Control Flow (Switch, Nested For, Do-While)")
{
    const char *SRC = R"script(
namespace N1 {
    namespace N2 {
        namespace N3 {
            namespace N4 {
                namespace N5 {
                    class DeepComponent
                    {
                        void Execute() {}
                    }
                }
            }
        }
    }
}

enum ExecutionMode { MODE_INIT, MODE_RUN }

void ControlFlowTest(ExecutionMode mode)
{
    N1::N2::N3::N4::N5::DeepComponent comp;
    comp.Execute();

    switch (mode)
    {
    case MODE_RUN:
        int switchVar = 42;
        int result = switchVar * 2;
        break;
    }

    for (int i = 0; i < 10; i++)
    {
        for (int j = 0; j < 5; j++)
        {
            int sum = i + j;
        }
    }

    int count = 0;
    do
    {
        int step = count + 1;
        count++;
    } while (count < 5);
}
)script";

    // 1. 5-level deep namespace N1 -> Line 1
    auto locsN1 = GetDefinitionLocations(SRC, "N1::N2::N3::N4::N5::DeepComponent", 0);
    REQUIRE(locsN1.size() == 1);
    CHECK(locsN1[0].range.start.line == 1);

    // 2. 5-level deep namespace N3 -> Line 3
    auto locsN3 = GetDefinitionLocations(SRC, "N1::N2::N3::N4::N5::DeepComponent", 8);
    REQUIRE(locsN3.size() == 1);
    CHECK(locsN3[0].range.start.line == 3);

    // 3. 5-level deep namespace N5 -> Line 5
    auto locsN5 = GetDefinitionLocations(SRC, "N1::N2::N3::N4::N5::DeepComponent", 16);
    REQUIRE(locsN5.size() == 1);
    CHECK(locsN5[0].range.start.line == 5);

    // 4. 5-level deep class DeepComponent -> Line 6
    auto locsDeepClass = GetDefinitionLocations(SRC, "N1::N2::N3::N4::N5::DeepComponent", 20);
    REQUIRE(locsDeepClass.size() == 1);
    CHECK(locsDeepClass[0].range.start.line == 6);

    // 5. Method call `comp.Execute()` -> Line 8
    auto locsDeepMethod = GetDefinitionLocations(SRC, "comp.Execute();", 5);
    REQUIRE(locsDeepMethod.size() == 1);
    CHECK(locsDeepMethod[0].range.start.line == 8);

    // 6. Switch-case variable `switchVar` -> Line 26
    auto locsSwitchVar = GetDefinitionLocations(SRC, "switchVar * 2", 0);
    REQUIRE(locsSwitchVar.size() == 1);
    CHECK(locsSwitchVar[0].range.start.line == 26);

    // 7. Nested for-loop outer variable `i` -> Line 31
    auto locsOuterI = GetDefinitionLocations(SRC, "i + j;", 0);
    REQUIRE(locsOuterI.size() == 1);
    CHECK(locsOuterI[0].range.start.line == 31);

    // 8. Nested for-loop inner variable `j` -> Line 33
    auto locsInnerJ = GetDefinitionLocations(SRC, "i + j;", 4);
    REQUIRE(locsInnerJ.size() == 1);
    CHECK(locsInnerJ[0].range.start.line == 33);

    // 9. Do-while loop variable `count` -> Line 39
    auto locsDoWhileCount = GetDefinitionLocations(SRC, "count + 1", 0);
    REQUIRE(locsDoWhileCount.size() == 1);
    CHECK(locsDoWhileCount[0].range.start.line == 39);
}

TEST_CASE("DefinitionHandler - Cross-File Resolution & as.predefined Headers")
{
    const char *PREDEFINED_SRC = R"script(
class string
{
    uint length() const;
    string opAdd(const string &in) const;
}

class Vector3
{
    float x;
    float y;
    float z;

    Vector3(float ax, float ay, float az);
}
)script";

    const char *MAIN_SRC = R"script(
void Main()
{
    string msg = "Hello AngelScript";
    uint len = msg.length();
    Vector3 vec(1.0f, 2.0f, 3.0f);
}
)script";

    std::string predefinedUri = "file:///as.predefined";
    Document predefinedDoc(predefinedUri, PREDEFINED_SRC);
    
    std::string mainUri = "file:///main.as";
    Document mainDoc(mainUri, MAIN_SRC);

    analysis::SymbolTable table;
    analysis::SymbolCollector::CollectGlobals(predefinedDoc, table);
    analysis::SymbolCollector::CollectGlobals(mainDoc, table);
    analysis::SymbolCollector::TraverseLocals(mainDoc.RootNode(), mainDoc, table, nullptr);

    // 1. Go to Definition on `msg.length()` from main.as -> length() in file:///as.predefined
    size_t offset = std::string(MAIN_SRC).find("length()");
    uint32_t line = 4, col = (uint32_t)(offset - std::string(MAIN_SRC).rfind('\n', offset) - 1);

    lsp::requests::TextDocument_Definition::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    req.position.line = line;
    req.position.character = col;

    auto result = features::ProcessDefinition(req, mainDoc, table);
    REQUIRE(!result.isNull());
    const auto &def = std::get<lsp::Definition>(*result);
    const auto &loc = std::get<lsp::Location>(def);
    CHECK(loc.uri.toString() == predefinedUri);
    CHECK(loc.range.start.line == 3);

    // 2. Go to Type Definition on `msg` -> class string in file:///as.predefined
    lsp::requests::TextDocument_TypeDefinition::Params typeReq;
    typeReq.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    typeReq.position.line = 3;
    typeReq.position.character = 11; // `msg`

    auto typeResult = features::ProcessTypeDefinition(typeReq, mainDoc, table);
    REQUIRE(!typeResult.isNull());
    const auto &typeDef = std::get<lsp::Definition>(*typeResult);
    const auto &typeLoc = std::get<lsp::Location>(typeDef);
    CHECK(typeLoc.uri.toString() == predefinedUri);
    CHECK(typeLoc.range.start.line == 1);
}

TEST_CASE("DefinitionHandler - #include Directives & Cross-File Symbol Resolution")
{
    const char *HEADER_SRC = R"script(
namespace Engine
{
    class RenderDevice
    {
        void Present() {}
    }
}
)script";

    const char *MAIN_SRC = R"script(
#include "engine/render.as"

void AppMain()
{
    Engine::RenderDevice device;
    device.Present();
}
)script";

    std::string headerUri = "file:///project/engine/render.as";
    Document headerDoc(headerUri, HEADER_SRC);

    std::string mainUri = "file:///project/main.as";
    Document mainDoc(mainUri, MAIN_SRC);

    analysis::SymbolTable table;
    analysis::SymbolCollector::CollectGlobals(headerDoc, table);
    analysis::SymbolCollector::CollectGlobals(mainDoc, table);
    analysis::SymbolCollector::TraverseLocals(mainDoc.RootNode(), mainDoc, table, nullptr);

    // 1. Go to Definition on `#include "engine/render.as"` line in main.as -> file:///project/engine/render.as
    lsp::requests::TextDocument_Definition::Params incReq;
    incReq.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    incReq.position.line = 1;
    incReq.position.character = 5; // On `#include`

    auto incResult = features::ProcessDefinition(incReq, mainDoc, table);
    REQUIRE(!incResult.isNull());
    const auto &incDef = std::get<lsp::Definition>(*incResult);
    const auto &incLoc = std::get<lsp::Location>(incDef);
    CHECK(incLoc.uri.toString() == lsp::DocumentUri::parse(headerUri).toString());

    // 2. Go to Definition on `RenderDevice` in main.as -> class RenderDevice in engine/render.as
    lsp::requests::TextDocument_Definition::Params classReq;
    classReq.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    classReq.position.line = 5;
    classReq.position.character = 14;

    auto classResult = features::ProcessDefinition(classReq, mainDoc, table);
    REQUIRE(!classResult.isNull());
    const auto &classDef = std::get<lsp::Definition>(*classResult);
    const auto &classLoc = std::get<lsp::Location>(classDef);
    CHECK(classLoc.uri.toString() == lsp::DocumentUri::parse(headerUri).toString());
    CHECK(classLoc.range.start.line == 3);

    // 3. Go to Definition on `device.Present()` in main.as -> void Present() in engine/render.as
    lsp::requests::TextDocument_Definition::Params methodReq;
    methodReq.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    methodReq.position.line = 6;
    methodReq.position.character = 12; // `Present`

    auto methodResult = features::ProcessDefinition(methodReq, mainDoc, table);
    REQUIRE(!methodResult.isNull());
    const auto &methodDef = std::get<lsp::Definition>(*methodResult);
    const auto &methodLoc = std::get<lsp::Location>(methodDef);
    CHECK(methodLoc.uri.toString() == lsp::DocumentUri::parse(headerUri).toString());
    CHECK(methodLoc.range.start.line == 5);
}

TEST_CASE("DefinitionHandler - Class member field method resolution (m_device.Present())")
{
    const char *HEADER_SRC = R"script(
namespace Engine
{
    class RenderDevice
    {
        void Present() {}
    }
}
)script";

    const char *RENDERER_SRC = R"script(
#include "engine/render.as"
namespace Engine
{
    class Renderer
    {
        private Engine::RenderDevice m_device;
        void RenderFrame()
        {
            m_device.Present();
        }
    }
}
)script";

    std::string headerUri = "file:///project/engine/render.as";
    Document headerDoc(headerUri, HEADER_SRC);

    std::string rendererUri = "file:///project/renderer.as";
    Document rendererDoc(rendererUri, RENDERER_SRC);

    analysis::SymbolTable table;
    analysis::SymbolCollector::CollectGlobals(headerDoc, table);
    analysis::SymbolCollector::CollectGlobals(rendererDoc, table);
    analysis::SymbolCollector::TraverseLocals(rendererDoc.RootNode(), rendererDoc, table);

    // Go to Definition on `Present` in `m_device.Present()`
    lsp::requests::TextDocument_Definition::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse(rendererUri);
    req.position.line = 9;
    req.position.character = 21; // `Present`

    auto res = features::ProcessDefinition(req, rendererDoc, table);
    REQUIRE(!res.isNull());
    const auto &def = std::get<lsp::Definition>(*res);
    const auto &loc = std::get<lsp::Location>(def);
    CHECK(loc.uri.toString() == lsp::DocumentUri::parse(headerUri).toString());
}

TEST_CASE("DefinitionHandler - Advanced #include Formats (no .as, <angle brackets>) & Preprocessor Directives (#if, #else, #define)")
{
    const char *SRC = R"script(
#include <engine/math>
#define WITH_DEBUG 1

#if WITH_DEBUG
class NetworkManager
{
    void Sync() {}
}
#else
class NetworkManager
{
    void Sync() {}
}
#endif

void MainApp()
{
    #if WITH_DEBUG
    NetworkManager net;
    net.Sync();
    #endif
}
)script";

    std::string uriStr = "file:///project/app.as";
    Document doc(uriStr, SRC);
    analysis::SymbolTable table;
    analysis::SymbolCollector::SetDefinedWords({"WITH_DEBUG"});
    analysis::SymbolCollector::CollectGlobals(doc, table);
    analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table);

    // 1. Go to Definition on `#include <engine/math>` -> resolves auto-appended .as -> file:///project/engine/math.as
    lsp::requests::TextDocument_Definition::Params incReq;
    incReq.textDocument.uri = lsp::DocumentUri::parse(uriStr);
    incReq.position.line = 1;
    incReq.position.character = 5;

    auto incResult = features::ProcessDefinition(incReq, doc, table);
    REQUIRE(!incResult.isNull());
    const auto &incDef = std::get<lsp::Definition>(*incResult);
    const auto &incLoc = std::get<lsp::Location>(incDef);
    CHECK(incLoc.uri.toString() == lsp::DocumentUri::parse("file:///project/engine/math.as").toString());

    // 2. Go to Definition on `#define WITH_DEBUG 1` line -> returns valid location
    lsp::requests::TextDocument_Definition::Params defReq;
    defReq.textDocument.uri = lsp::DocumentUri::parse(uriStr);
    defReq.position.line = 2;
    defReq.position.character = 3;

    auto defResult = features::ProcessDefinition(defReq, doc, table);
    REQUIRE(!defResult.isNull());

    // 3. Go to Definition on `NetworkManager` inside MainApp -> returns valid location
    lsp::requests::TextDocument_Definition::Params netReq;
    netReq.textDocument.uri = lsp::DocumentUri::parse(uriStr);
    netReq.position.line = 20;
    netReq.position.character = 8; // `NetworkManager`

    auto netResult = features::ProcessDefinition(netReq, doc, table);
    REQUIRE(!netResult.isNull());
}

TEST_CASE("DefinitionHandler - Open In-Memory Included File Resolution & Symbol Deduplication")
{
    const char *INCLUDED_OPEN_SRC = R"script(
class OpenInEditorClass
{
    void LiveMethod() {}
}
)script";

    const char *MAIN_SRC = R"script(
#include "editor/open_file.as"

void Main()
{
    OpenInEditorClass obj;
    obj.LiveMethod();
}
)script";

    std::string incUri = "file:///project/editor/open_file.as";
    Document incDoc(incUri, INCLUDED_OPEN_SRC);

    std::string mainUri = "file:///project/main.as";
    Document mainDoc(mainUri, MAIN_SRC);

    analysis::SymbolTable table;

    auto resolver = [&](const std::string &uri) -> const Document * {
        if (uri == incUri) return &incDoc;
        return nullptr;
    };

    analysis::SymbolCollector::CollectGlobals(incDoc, table);
    analysis::SymbolCollector::CollectGlobals(mainDoc, table, resolver);
    analysis::SymbolCollector::TraverseLocals(mainDoc.RootNode(), mainDoc, table);

    auto overloads = table.FindAllGlobalsByName("OpenInEditorClass");
    CHECK(overloads.size() == 1);

    lsp::requests::TextDocument_Definition::Params classReq;
    classReq.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    classReq.position.line = 5;
    classReq.position.character = 4;

    auto classResult = features::ProcessDefinition(classReq, mainDoc, table);
    REQUIRE(!classResult.isNull());
    const auto &classDef = std::get<lsp::Definition>(*classResult);
    const auto &classLoc = std::get<lsp::Location>(classDef);
    CHECK(classLoc.uri.toString() == lsp::DocumentUri::parse(incUri).toString());
    CHECK(classLoc.range.start.line == 1);
}

TEST_CASE("DefinitionHandler - Multi-Level Nested Includes (main -> render -> vector)")
{
    const char *VECTOR_SRC = R"script(
class Vector2D { float x; float y; }
)script";

    const char *RENDER_SRC = R"script(
#include "vector.as"
class Canvas { Vector2D pos; }
)script";

    const char *MAIN_SRC = R"script(
#include "render.as"
void App() { Canvas c; Vector2D v = c.pos; }
)script";

    std::string vecUri = "file:///project/vector.as";
    Document vecDoc(vecUri, VECTOR_SRC);

    std::string renUri = "file:///project/render.as";
    Document renDoc(renUri, RENDER_SRC);

    std::string mainUri = "file:///project/main.as";
    Document mainDoc(mainUri, MAIN_SRC);

    analysis::SymbolTable table;

    auto resolver = [&](const std::string &uri) -> const Document * {
        if (uri == vecUri) return &vecDoc;
        if (uri == renUri) return &renDoc;
        return nullptr;
    };

    analysis::SymbolCollector::CollectGlobals(mainDoc, table, resolver);
    analysis::SymbolCollector::TraverseLocals(mainDoc.RootNode(), mainDoc, table);

    lsp::requests::TextDocument_Definition::Params vecReq;
    vecReq.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    vecReq.position.line = 2;
    vecReq.position.character = 23;

    auto vecResult = features::ProcessDefinition(vecReq, mainDoc, table);
    REQUIRE(!vecResult.isNull());
    const auto &vecDef = std::get<lsp::Definition>(*vecResult);
    const auto &vecLoc = std::get<lsp::Location>(vecDef);
    CHECK(vecLoc.uri.toString() == lsp::DocumentUri::parse(vecUri).toString());
}

TEST_CASE("Go To Definition on extensionless #include directive")
{
    const char *MAIN_SRC = R"script(
#include "engine/render"
void Main() {}
)script";

    const char *RENDER_SRC = R"script(
void Render() {}
)script";

    std::string renUri = "file:///project/engine/render.as";
    Document renDoc(renUri, RENDER_SRC);

    std::string mainUri = "file:///project/main.as";
    Document mainDoc(mainUri, MAIN_SRC);

    analysis::SymbolTable table;
    auto resolver = [&](const std::string &uri) -> const Document * {
        if (uri == renUri) return &renDoc;
        return nullptr;
    };

    analysis::SymbolCollector::CollectGlobals(mainDoc, table, resolver);

    lsp::requests::TextDocument_Definition::Params req;
    req.textDocument.uri = lsp::DocumentUri::parse(mainUri);
    req.position.line = 1;
    req.position.character = 12;

    auto result = features::ProcessDefinition(req, mainDoc, table);
    REQUIRE(!result.isNull());
    const auto &def = std::get<lsp::Definition>(*result);
    const auto &loc = std::get<lsp::Location>(def);
    CHECK(loc.uri.toString() == lsp::DocumentUri::parse(renUri).toString());
}
