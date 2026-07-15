#include "analysis/PredefinedLoader.h"
#include <angelscript.h>
#include <fstream>
#include <cstdio>
#include <iostream>
#include <doctest/doctest.h>

TEST_SUITE("PredefinedLoader") {

    static std::string CreateTempJSON(const std::string& content) {
        std::string path = "temp_predefined.json";
        std::ofstream out(path);
        out << content;
        return path;
    }

    TEST_CASE("Loads minimal valid predefined file into script engine") {
        std::string jsonStr = R"JSON(
        {
            "enums": [
                { "name": "GameState" }
            ],
            "types": [
                {
                    "name": "Entity",
                    "flags": ["ref"],
                    "methods": [
                        { "decl": "void Update(float dt)" }
                    ],
                    "properties": [
                        { "decl": "int id" }
                    ]
                }
            ],
            "functions": [
                { "decl": "void Print(int)" }
            ]
        }
        )JSON";

        std::string path = CreateTempJSON(jsonStr);
        
        asIScriptEngine* asEngine = asCreateScriptEngine();
        
        bool success = analysis::PredefinedLoader::LoadAndRegister(path, asEngine);
        CHECK(success);

        int tId = asEngine->GetTypeIdByDecl("GameState");
        CHECK(tId > 0);

        tId = asEngine->GetTypeIdByDecl("Entity");
        CHECK(tId > 0);
        
        asITypeInfo* tInfo = asEngine->GetTypeInfoById(tId);
        CHECK(tInfo != nullptr);
        
        int mCount = tInfo->GetMethodCount();
        CHECK(mCount == 1);
        if (mCount > 0) {
            asIScriptFunction* func = tInfo->GetMethodByIndex(0);
            CHECK(std::string(func->GetDeclaration()) == "void Entity::Update(float)");
        }

        int pCount = tInfo->GetPropertyCount();
        CHECK(pCount == 1);
        if (pCount > 0) {
            const char* propName;
            tInfo->GetProperty(0, &propName);
            CHECK(std::string(propName) == "id");
        }

        int gCount = asEngine->GetGlobalFunctionCount();
        CHECK(gCount == 1);
        if (gCount > 0) {
            asIScriptFunction* func = asEngine->GetGlobalFunctionByIndex(0);
            CHECK(std::string(func->GetDeclaration()) == "void Print(int)");
        }

        asEngine->ShutDownAndRelease();
        std::remove(path.c_str());
    }
}
