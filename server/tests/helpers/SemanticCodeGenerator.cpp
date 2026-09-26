#include "SemanticCodeGenerator.h"
#include "TestUtils.h"

namespace angel_lsp::test
{

PrimitiveScriptInfo GeneratePrimitiveFunctionsScript(std::mt19937_64& rng)
{
    PrimitiveScriptInfo info;
    info.typeName = GenerateRandomPrimitiveType(rng);
    info.funcName = GenerateIdentifier(rng, "compute");
    info.varName = GenerateIdentifier(rng, "result");
    const std::string paramName = GenerateIdentifier(rng, "input");

    info.script = info.typeName + " " + info.funcName + "(" + info.typeName + " " + paramName + ")\n"
                + "{\n"
                + "    " + info.typeName + " " + info.varName + " = " + paramName + ";\n"
                + "    return " + info.varName + ";\n"
                + "}\n";

    info.varLine = 2;
    info.varCol = 4 + static_cast<uint32_t>(info.typeName.size()) + 1;
    return info;
}

InheritanceScriptInfo GenerateInheritanceHierarchyScript(std::mt19937_64& rng)
{
    InheritanceScriptInfo info;
    info.baseName = GenerateIdentifier(rng, "BaseCls");
    info.derivedName = GenerateIdentifier(rng, "DerivedCls");
    info.leafName = GenerateIdentifier(rng, "LeafCls");
    info.baseField = GenerateIdentifier(rng, "baseProp");
    info.baseMethod = GenerateIdentifier(rng, "BaseMethod");
    info.derivedField = GenerateIdentifier(rng, "derivedProp");
    info.derivedMethod = GenerateIdentifier(rng, "DerivedMethod");
    info.leafField = GenerateIdentifier(rng, "leafProp");
    info.leafMethod = GenerateIdentifier(rng, "LeafMethod");
    info.objVarName = GenerateIdentifier(rng, "instance");

    std::string s;
    s += "class " + info.baseName + "\n{\n";
    s += "    int " + info.baseField + ";\n";
    s += "    void " + info.baseMethod + "() {}\n}\n\n";
    s += "class " + info.derivedName + " : " + info.baseName + "\n{\n";
    s += "    float " + info.derivedField + ";\n";
    s += "    void " + info.derivedMethod + "() {}\n}\n\n";
    s += "class " + info.leafName + " : " + info.derivedName + "\n{\n";
    s += "    string " + info.leafField + ";\n";
    s += "    void " + info.leafMethod + "() {}\n}\n\n";
    s += "void ExecuteTest()\n{\n";
    s += "    " + info.leafName + " " + info.objVarName + ";\n";
    s += "    " + info.objVarName + ".\n}\n";

    info.script = s;
    info.baseFieldHoverLine = 2;
    info.baseFieldHoverCol = 8;
    info.dotLine = 21;
    info.dotCol = 4 + static_cast<uint32_t>(info.objVarName.size()) + 1;
    return info;
}

EngineScriptInfo GenerateEngineHookScript(std::mt19937_64& rng)
{
    EngineScriptInfo info;
    info.funcName = GenerateIdentifier(rng, "PlayerHook");
    info.paramName = GenerateIdentifier(rng, "pPlayer");

    std::string s;
    s += "void " + info.funcName + "(CBasePlayer@ " + info.paramName + ")\n";
    s += "{\n";
    s += "    if (" + info.paramName + " !is null)\n";
    s += "    {\n";
    s += "        " + info.paramName + ".\n";
    s += "    }\n";
    s += "}\n";

    info.script = s;
    info.playerHoverLine = 0;
    info.playerHoverCol = 5 + static_cast<uint32_t>(info.funcName.size()) + 14;
    info.playerDotLine = 4;
    info.playerDotCol = 8 + static_cast<uint32_t>(info.paramName.size()) + 1;
    return info;
}

std::string GenerateFuzzBombardmentScript(std::mt19937_64& rng)
{
    const std::string modeEnum = GenerateIdentifier(rng, "GameMode");
    const std::string baseClass = GenerateIdentifier(rng, "EntityTracker");
    const std::string derivedClass = GenerateIdentifier(rng, "PlayerTracker");
    const std::string funcName = GenerateIdentifier(rng, "ProcessGame");

    std::string s;
    s += "enum " + modeEnum + "\n{\n    Deathmatch = 1,\n    Coop = 2,\n    Survival = 3\n}\n\n";
    s += "class " + baseClass + "\n{\n";
    s += "    int entityCount;\n";
    s += "    float refreshRate;\n";
    s += "    void Update(float dt) { entityCount += int(dt); }\n";
    s += "    bool IsActive() const { return entityCount > 0; }\n";
    s += "}\n\n";
    s += "class " + derivedClass + " : " + baseClass + "\n{\n";
    s += "    string playerName;\n";
    s += "    void SetName(const string& in name) { playerName = name; }\n";
    s += "}\n\n";
    s += "void " + funcName + "(" + modeEnum + " mode, float delta)\n{\n";
    s += "    " + derivedClass + " tracker;\n";
    s += "    tracker.entityCount = 10;\n";
    s += "    tracker.Update(delta);\n";
    s += "    if (mode == " + modeEnum + "::Coop && tracker.IsActive())\n";
    s += "    {\n";
    s += "        tracker.SetName(\"Gordon\");\n";
    s += "    }\n";
    s += "}\n";
    return s;
}

} // namespace angel_lsp::test
