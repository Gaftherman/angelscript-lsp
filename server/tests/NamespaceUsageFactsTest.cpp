#include "analysis/index/NamespaceUsageFacts.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("NamespaceUsageFacts - Scoped identifier references")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("fn");
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("NS");
    const std::string subNs = angel_lsp::test::GenerateRandomSymbolName("Sub");
    const std::string targetFunc = angel_lsp::test::GenerateRandomSymbolName("target");
    const std::string globalFunc = angel_lsp::test::GenerateRandomSymbolName("globalTarget");

    const std::string code = "void " + fnName + "()\n{\n" + "    " + nsName + "::" + targetFunc + "();\n" +
                             "    ::" + nsName + "::" + targetFunc + "();\n" + "    " + nsName + "::" + subNs +
                             "::" + targetFunc + "();\n" + "    ::" + globalFunc + "();\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildNamespaceUsageFacts(index, code);

    REQUIRE(facts.scopedIdentifiers.size() >= 3);

    // 1. Simple scoped identifier: NS::target
    const auto* fact0 = facts.FindScopedIdentifier(facts.scopedIdentifiers[0].bytes);
    REQUIRE(fact0 != nullptr);
    CHECK(fact0->prefix == nsName);
    CHECK(fact0->memberName == targetFunc);
    CHECK_FALSE(fact0->hasLeadingGlobal);
    CHECK(fact0->qualifiedText == nsName + "::" + targetFunc);

    // 2. Global scoped identifier: ::NS::target
    const auto* fact1 = facts.FindScopedIdentifier(facts.scopedIdentifiers[1].bytes);
    REQUIRE(fact1 != nullptr);
    CHECK(fact1->prefix == nsName);
    CHECK(fact1->memberName == targetFunc);
    CHECK(fact1->hasLeadingGlobal);

    // 3. Multi-segment scoped identifier: NS::Sub::target
    const auto* fact2 = facts.FindScopedIdentifier(facts.scopedIdentifiers[2].bytes);
    REQUIRE(fact2 != nullptr);
    CHECK(fact2->prefix == nsName);
    CHECK(fact2->memberName == targetFunc);

    ts_tree_delete(tree);
}

TEST_CASE("NamespaceUsageFacts - Scoped type references")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("TypeNS");
    const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("CustomType");
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("myVar");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("process");
    const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("param");

    const std::string code = nsName + "::" + typeName + " " + varName + ";\n" + "void " + fnName + "(::" + nsName +
                             "::" + typeName + " " + paramName + ") {}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildNamespaceUsageFacts(index, code);

    REQUIRE(facts.scopedTypes.size() == 2);

    // 1. Scoped type: TypeNS::CustomType
    const auto& type0 = facts.scopedTypes[0];
    CHECK(type0.prefix == nsName);
    CHECK(type0.memberName == typeName);
    CHECK_FALSE(type0.hasLeadingGlobal);
    CHECK(facts.FindScopedType(type0.bytes) == &type0);

    // 2. Global scoped type: ::TypeNS::CustomType
    const auto& type1 = facts.scopedTypes[1];
    CHECK(type1.prefix == nsName);
    CHECK(type1.memberName == typeName);
    CHECK(type1.hasLeadingGlobal);
    CHECK(facts.FindScopedType(type1.bytes) == &type1);

    ts_tree_delete(tree);
}

TEST_CASE("NamespaceUsageFacts - Namespace and using declarations")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("OuterNS");
    const std::string usingNs = angel_lsp::test::GenerateRandomSymbolName("ImportedNS");
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("val");

    const std::string code = "using namespace " + usingNs + ";\n" + "namespace " + nsName + "\n{\n" + "    int " +
                             varName + " = 42;\n" + "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildNamespaceUsageFacts(index, code);

    // Using declarations
    REQUIRE(facts.usingDeclarations.size() == 1);
    const auto& usingDecl = facts.usingDeclarations.front();
    CHECK(usingDecl.namespaceName == usingNs);
    CHECK(facts.FindUsingDeclaration(usingDecl.bytes) == &usingDecl);

    // Namespace declarations
    REQUIRE(facts.namespaceDeclarations.size() == 1);
    const auto& nsDecl = facts.namespaceDeclarations.front();
    CHECK(nsDecl.name == nsName);
    CHECK(facts.FindNamespaceDeclaration(nsDecl.bytes) == &nsDecl);
    CHECK(nsDecl.bodyBytes.startByte < nsDecl.bodyBytes.endByte);

    ts_tree_delete(tree);
}

TEST_CASE("NamespaceUsageFacts - Import declarations with body detection")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string funcGood = angel_lsp::test::GenerateRandomSymbolName("funcGood");
    const std::string funcBad = angel_lsp::test::GenerateRandomSymbolName("funcBad");
    const std::string modName = angel_lsp::test::GenerateRandomSymbolName("MyMod");

    const std::string code = "import void " + funcGood + "() from \"" + modName + "\";\n" + "import void " + funcBad +
                             "() from \"" + modName + "\" { int x = 1; }\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildNamespaceUsageFacts(index, code);

    REQUIRE(facts.importDeclarations.size() >= 2);

    const auto* validFact = facts.FindImportDeclaration(facts.importDeclarations[0].bytes);
    REQUIRE(validFact != nullptr);
    CHECK_FALSE(validFact->hasBodyBlock);

    const auto* invalidFact = facts.FindImportDeclaration(facts.importDeclarations[1].bytes);
    REQUIRE(invalidFact != nullptr);
    CHECK(invalidFact->hasBodyBlock);

    ts_tree_delete(tree);
}

TEST_CASE("NamespaceUsageFacts - Identifier use sites and enclosing container hierarchy")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("ScopeNS");
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("ScopeClass");
    const std::string mixinName = angel_lsp::test::GenerateRandomSymbolName("ScopeMixin");
    const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("method");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("varA");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("varB");

    const std::string code = "namespace " + nsName + "\n{\n" + "    class " + clsName + "\n    {\n" + "        void " +
                             methodName + "()\n        {\n" + "            " + varA + " = 1;\n" +
                             "        }\n    }\n}\n" + "mixin class " + mixinName + "\n{\n" + "    void " + methodName +
                             "()\n    {\n" + "        " + varB + " = 2;\n" + "    }\n}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    angel_lsp::analysis::NodeIndex index(root, angel_lsp::analysis::NodeIndexOptions{.source = code});

    const auto facts = angel_lsp::analysis::BuildNamespaceUsageFacts(index, code);

    // Locate varA use site
    const auto itVarA = std::ranges::find_if(
        facts.identifierUseSites, [&](const angel_lsp::analysis::IdentifierUseSiteFact& s) { return s.name == varA; });
    REQUIRE(itVarA != facts.identifierUseSites.end());
    CHECK_FALSE(itVarA->isInsideMixin);
    CHECK_FALSE(itVarA->isInsideError);

    const auto containersA = facts.EnclosingContainersFor(*itVarA);
    REQUIRE(containersA.size() >= 2);
    // Innermost container is class Cls, followed by namespace NS
    CHECK(containersA[0].name == clsName);
    CHECK(containersA[0].kind == angel_lsp::analysis::ContainerKind::Class);
    CHECK_FALSE(containersA[0].isMixin);
    CHECK(containersA[1].name == nsName);
    CHECK(containersA[1].kind == angel_lsp::analysis::ContainerKind::Namespace);

    // Locate varB use site inside mixin class
    const auto itVarB = std::ranges::find_if(
        facts.identifierUseSites, [&](const angel_lsp::analysis::IdentifierUseSiteFact& s) { return s.name == varB; });
    REQUIRE(itVarB != facts.identifierUseSites.end());
    CHECK(itVarB->isInsideMixin);

    const auto containersB = facts.EnclosingContainersFor(*itVarB);
    REQUIRE(containersB.size() >= 1);
    CHECK(containersB[0].name == mixinName);
    CHECK(containersB[0].kind == angel_lsp::analysis::ContainerKind::Class);
    CHECK(containersB[0].isMixin);

    ts_tree_delete(tree);
}
