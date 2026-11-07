#include "analysis/resolution/ReferenceCounting.h"

#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/MixinInclusion.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

TEST_CASE("ReferenceCounting - Member calls via base class")
{
    angel_lsp::parser::AngelScriptParser parser;
    angel_lsp::analysis::SymbolCollector symbolCollector(nullptr);
    angel_lsp::analysis::LocalScopeCollector scopeCollector(nullptr);
    angel_lsp::analysis::SymbolTable symbolTable;
    angel_lsp::analysis::ScopeIndex scopeIndex;

    const std::string baseName = angel_lsp::test::GenerateRandomSymbolName("Base");
    const std::string derivedName = angel_lsp::test::GenerateRandomSymbolName("Derived");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("Compute");

    const std::string uri = "file:///hierarchy_test.as";
    const std::string code = "class " + baseName +
                             " {\n"
                             "    void " +
                             methodName +
                             "() {}\n"
                             "}\n"
                             "class " +
                             derivedName + " : " + baseName +
                             " {\n"
                             "    void Run() {\n"
                             "        " +
                             methodName +
                             "();\n"
                             "    }\n"
                             "}\n";

    TSTree* tree = parser.Parse(code);
    symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
    auto scopeRoot = scopeCollector.CollectScopes(code, parser);
    if (scopeRoot)
    {
        scopeIndex.SetScopeTree(uri, std::move(scopeRoot));
    }
    if (tree)
    {
        ts_tree_delete(tree);
    }

    auto methodSyms = symbolTable.FindMemberSymbols(baseName, methodName);
    REQUIRE(!methodSyms.empty());

    auto mixinMap = angel_lsp::analysis::BuildMixinInclusionMap(symbolTable);
    angel_lsp::analysis::MixinClassContext mixinCtx{symbolTable, mixinMap};
    auto compatibleClasses = angel_lsp::analysis::CollectCompatibleClasses(
        methodSyms, methodSyms.front(), methodSyms.front().GetFunction().modifiers.access, mixinCtx);
    auto allDeclRanges =
        angel_lsp::analysis::CollectAllDeclRanges(methodName, methodSyms, compatibleClasses, symbolTable);

    angel_lsp::analysis::ReferenceCountTarget target{
        .key = {methodSyms.front().startLine, methodSyms.front().startCharacter, methodSyms.front().endLine,
                methodSyms.front().endCharacter},
        .targetName = methodName,
        .group = methodSyms,
        .compatibleClasses = std::move(compatibleClasses),
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = {},
        .targetEnumContainer = {},
        .isEnumConstant = false,
        .targetAccess = methodSyms.front().GetFunction().modifiers.access,
        .isFunction = true,
        .minArgs = 0,
        .maxArgs = 0,
        .seenRefs = {},
    };

    std::vector<angel_lsp::analysis::ReferenceCountTarget> targets;
    targets.push_back(std::move(target));
    angel_lsp::analysis::TargetIndexMap targetsByName;
    targetsByName[methodName].push_back(0);

    angel_lsp::analysis::ReferenceCountRequest request{
        .uri = uri,
        .sourceCode = code,
        .symbolTable = symbolTable,
        .scopeIndex = scopeIndex,
    };

    angel_lsp::analysis::CountReferences(targets, targetsByName, request);
    CHECK(targets[0].seenRefs.size() == 1);
}

TEST_CASE("ReferenceCounting - Local variable shadows target function")
{
    angel_lsp::parser::AngelScriptParser parser;
    angel_lsp::analysis::SymbolCollector symbolCollector(nullptr);
    angel_lsp::analysis::LocalScopeCollector scopeCollector(nullptr);
    angel_lsp::analysis::SymbolTable symbolTable;
    angel_lsp::analysis::ScopeIndex scopeIndex;

    const std::string funcName = angel_lsp::test::GenerateRandomSymbolName("Helper");
    const std::string callerName = angel_lsp::test::GenerateRandomSymbolName("Caller");

    const std::string uri = "file:///shadow_test.as";
    const std::string code = "void " + funcName +
                             "() {}\n"
                             "void " +
                             callerName +
                             "() {\n"
                             "    int " +
                             funcName +
                             " = 42;\n"
                             "    " +
                             funcName +
                             "++;\n"
                             "}\n";

    TSTree* tree = parser.Parse(code);
    symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
    auto scopeRoot = scopeCollector.CollectScopes(code, parser);
    if (scopeRoot)
    {
        scopeIndex.SetScopeTree(uri, std::move(scopeRoot));
    }
    if (tree)
    {
        ts_tree_delete(tree);
    }

    auto funcSyms = symbolTable.FindSymbols(funcName);
    REQUIRE(!funcSyms.empty());

    auto allDeclRanges = angel_lsp::analysis::CollectAllDeclRanges(funcName, funcSyms, {}, symbolTable);

    angel_lsp::analysis::ReferenceCountTarget target{
        .key = {funcSyms.front().startLine, funcSyms.front().startCharacter, funcSyms.front().endLine,
                funcSyms.front().endCharacter},
        .targetName = funcName,
        .group = funcSyms,
        .compatibleClasses = {},
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = {},
        .targetEnumContainer = {},
        .isEnumConstant = false,
        .targetAccess = angel_lsp::analysis::AccessModifier::Public,
        .isFunction = true,
        .minArgs = 0,
        .maxArgs = 0,
        .seenRefs = {},
    };

    std::vector<angel_lsp::analysis::ReferenceCountTarget> targets;
    targets.push_back(std::move(target));
    angel_lsp::analysis::TargetIndexMap targetsByName;
    targetsByName[funcName].push_back(0);

    angel_lsp::analysis::ReferenceCountRequest request{
        .uri = uri,
        .sourceCode = code,
        .symbolTable = symbolTable,
        .scopeIndex = scopeIndex,
    };

    angel_lsp::analysis::CountReferences(targets, targetsByName, request);
    CHECK(targets[0].seenRefs.empty());
}

TEST_CASE("ReferenceCounting - Enum constants across namespaces")
{
    angel_lsp::parser::AngelScriptParser parser;
    angel_lsp::analysis::SymbolCollector symbolCollector(nullptr);
    angel_lsp::analysis::LocalScopeCollector scopeCollector(nullptr);
    angel_lsp::analysis::SymbolTable symbolTable;
    angel_lsp::analysis::ScopeIndex scopeIndex;

    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("Game");
    const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("State");
    const std::string valName = angel_lsp::test::GenerateRandomSymbolName("Active");

    const std::string uri = "file:///enum_ns_test.as";
    const std::string code = "namespace " + nsName +
                             " {\n"
                             "    enum " +
                             enumName +
                             " {\n"
                             "        " +
                             valName +
                             " = 1\n"
                             "    }\n"
                             "    void Test() {\n"
                             "        int x = " +
                             valName +
                             ";\n"
                             "    }\n"
                             "}\n";

    TSTree* tree = parser.Parse(code);
    symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
    auto scopeRoot = scopeCollector.CollectScopes(code, parser);
    if (scopeRoot)
    {
        scopeIndex.SetScopeTree(uri, std::move(scopeRoot));
    }
    if (tree)
    {
        ts_tree_delete(tree);
    }

    const std::string qualifiedValName = nsName + "::" + enumName + "::" + valName;
    auto valSyms = symbolTable.FindSymbols(qualifiedValName);
    if (valSyms.empty())
    {
        valSyms = symbolTable.FindSymbols(valName);
    }
    REQUIRE(!valSyms.empty());

    auto allDeclRanges = angel_lsp::analysis::CollectAllDeclRanges(valName, valSyms, {}, symbolTable);

    angel_lsp::analysis::ReferenceCountTarget target{
        .key = {valSyms.front().startLine, valSyms.front().startCharacter, valSyms.front().endLine,
                valSyms.front().endCharacter},
        .targetName = valName,
        .group = valSyms,
        .compatibleClasses = {},
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = nsName,
        .targetEnumContainer = nsName + "::" + enumName,
        .isEnumConstant = true,
        .targetAccess = angel_lsp::analysis::AccessModifier::Public,
        .isFunction = false,
        .minArgs = 0,
        .maxArgs = 0,
        .seenRefs = {},
    };

    std::vector<angel_lsp::analysis::ReferenceCountTarget> targets;
    targets.push_back(std::move(target));
    angel_lsp::analysis::TargetIndexMap targetsByName;
    targetsByName[valName].push_back(0);

    angel_lsp::analysis::ReferenceCountRequest request{
        .uri = uri,
        .sourceCode = code,
        .symbolTable = symbolTable,
        .scopeIndex = scopeIndex,
    };

    angel_lsp::analysis::CountReferences(targets, targetsByName, request);
    CHECK(targets[0].seenRefs.size() == 1);
}

TEST_CASE("ReferenceCounting - Private member access rejected from unrelated class")
{
    angel_lsp::parser::AngelScriptParser parser;
    angel_lsp::analysis::SymbolCollector symbolCollector(nullptr);
    angel_lsp::analysis::LocalScopeCollector scopeCollector(nullptr);
    angel_lsp::analysis::SymbolTable symbolTable;
    angel_lsp::analysis::ScopeIndex scopeIndex;

    const std::string cls1 = angel_lsp::test::GenerateRandomSymbolName("Owner");
    const std::string cls2 = angel_lsp::test::GenerateRandomSymbolName("Other");
    const std::string memberName = angel_lsp::test::GenerateRandomSymbolName("secret");

    const std::string uri = "file:///private_test.as";
    const std::string code = "class " + cls1 +
                             " {\n"
                             "    private int " +
                             memberName +
                             ";\n"
                             "}\n"
                             "class " +
                             cls2 +
                             " {\n"
                             "    void Work() {\n"
                             "        int " +
                             memberName +
                             " = 10;\n"
                             "    }\n"
                             "}\n";

    TSTree* tree = parser.Parse(code);
    symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
    auto scopeRoot = scopeCollector.CollectScopes(code, parser);
    if (scopeRoot)
    {
        scopeIndex.SetScopeTree(uri, std::move(scopeRoot));
    }
    if (tree)
    {
        ts_tree_delete(tree);
    }

    auto memberSyms = symbolTable.FindMemberSymbols(cls1, memberName);
    REQUIRE(!memberSyms.empty());

    auto mixinMap = angel_lsp::analysis::BuildMixinInclusionMap(symbolTable);
    angel_lsp::analysis::MixinClassContext mixinCtx{symbolTable, mixinMap};
    auto compatibleClasses = angel_lsp::analysis::CollectCompatibleClasses(
        memberSyms, memberSyms.front(), angel_lsp::analysis::AccessModifier::Private, mixinCtx);
    auto allDeclRanges =
        angel_lsp::analysis::CollectAllDeclRanges(memberName, memberSyms, compatibleClasses, symbolTable);

    angel_lsp::analysis::ReferenceCountTarget target{
        .key = {memberSyms.front().startLine, memberSyms.front().startCharacter, memberSyms.front().endLine,
                memberSyms.front().endCharacter},
        .targetName = memberName,
        .group = memberSyms,
        .compatibleClasses = std::move(compatibleClasses),
        .allDeclRanges = std::move(allDeclRanges),
        .targetNamespace = {},
        .targetEnumContainer = {},
        .isEnumConstant = false,
        .targetAccess = angel_lsp::analysis::AccessModifier::Private,
        .isFunction = false,
        .minArgs = 0,
        .maxArgs = 0,
        .seenRefs = {},
    };

    std::vector<angel_lsp::analysis::ReferenceCountTarget> targets;
    targets.push_back(std::move(target));
    angel_lsp::analysis::TargetIndexMap targetsByName;
    targetsByName[memberName].push_back(0);

    angel_lsp::analysis::ReferenceCountRequest request{
        .uri = uri,
        .sourceCode = code,
        .symbolTable = symbolTable,
        .scopeIndex = scopeIndex,
    };

    angel_lsp::analysis::CountReferences(targets, targetsByName, request);
    CHECK(targets[0].seenRefs.empty());
}

TEST_CASE("MixinInclusion - FindTypeReferenceRange locates base and mixin specifiers")
{
    angel_lsp::parser::AngelScriptParser parser;
    angel_lsp::analysis::LocalScopeCollector scopeCollector(nullptr);

    const std::string hostName = angel_lsp::test::GenerateRandomSymbolName("Host");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("Mixin");

    const std::string code = "class " + hostName +
                             " :\n"
                             "    " +
                             mixinName +
                             " {\n"
                             "    void Work() {}\n"
                             "}\n";

    auto rootScope = scopeCollector.CollectScopes(code, parser);
    REQUIRE(rootScope != nullptr);

    auto range = angel_lsp::analysis::FindTypeReferenceRange(*rootScope, mixinName, 0, 3);
    REQUIRE(range.has_value());
    CHECK(range->startLine == 1u);
}
