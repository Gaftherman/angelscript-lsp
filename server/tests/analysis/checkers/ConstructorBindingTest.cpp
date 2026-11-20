#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/index/CallBindings.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <memory>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
/** @brief Analyzes one document the way the server does and keeps the call bindings the analysis published. */
struct BindingFixture
{
    AngelScriptParser parser;
    SymbolCollector collector{nullptr};
    LocalScopeCollector scopes{nullptr};
    SymbolTable table;
    std::shared_ptr<Scope> root;
    std::string code;
    TSTree* tree = nullptr;

    explicit BindingFixture(std::string source) : code(std::move(source))
    {
        static angel_lsp::i18n::I18n i18n;
        const std::string uri = "file:///bindings.as";
        collector.CollectSymbols(uri, code, parser, table);
        table.EnsureRuleIndex();
        root = scopes.CollectScopes(code, parser);

        SemanticAnalysisRequest request{table, uri, ".as.predefined", &i18n};
        request.scopeRoot = root;
        request.mutableScopeRoot = root.get();
        request.sourceCode = code;
        tree = parser.Parse(code);
        NodeIndex nodeIndex(ts_tree_root_node(tree));
        request.nodeIndex = &nodeIndex;
        SemanticAnalyzer analyzer(nullptr);
        analyzer.Analyze(request);
    }

    ~BindingFixture()
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }

    BindingFixture(const BindingFixture&) = delete;
    BindingFixture& operator=(const BindingFixture&) = delete;

    const CallBindings* Bindings() const
    {
        return root ? root->GetCallBindings() : nullptr;
    }

    /** @brief The declaration published for the use that starts at `marker`, or nullptr. */
    const CallDeclaration* DeclarationAt(const std::string& marker) const
    {
        const size_t at = code.find(marker);
        const CallBindings* bindings = Bindings();
        return at == std::string::npos || !bindings ? nullptr : bindings->Find(static_cast<uint32_t>(at));
    }

    /** @brief 0-based line on which `text` first appears. */
    uint32_t LineOf(const std::string& text) const
    {
        const size_t at = code.find(text);
        return static_cast<uint32_t>(std::count(code.begin(), code.begin() + static_cast<long>(at), '\n'));
    }
};
} // namespace

TEST_CASE("ConstructorBinding - Same-arity constructors keep separate declaration identities")
{
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("Widget");
    const std::string b = angel_lsp::test::GenerateRandomSymbolName("Left");
    const std::string c = angel_lsp::test::GenerateRandomSymbolName("Right");
    const std::string source = "class " + b + " { }\nclass " + c + " { }\nclass " + a + "\n{\n    " + a + "(" + b +
                               "@ l) { }\n    " + a + "(" + c + "@ r) { }\n}\nvoid main()\n{\n    " + b +
                               "@ hb;\n    " + c + "@ hc;\n    " + a + " first(hb);\n    " + a + " second(hc);\n}\n";
    const BindingFixture fixture(source);

    const CallDeclaration* first = fixture.DeclarationAt("first(");
    const CallDeclaration* second = fixture.DeclarationAt("second(");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->range.startLine == fixture.LineOf(a + "(" + b + "@ l)"));
    CHECK(second->range.startLine == fixture.LineOf(a + "(" + c + "@ r)"));
    CHECK_FALSE(*first == *second);
}

TEST_CASE("ConstructorBinding - Qualified homonyms resolve to their own namespace's constructor")
{
    const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Shared");
    const std::string one = angel_lsp::test::GenerateRandomSymbolName("NsOne");
    const std::string two = angel_lsp::test::GenerateRandomSymbolName("NsTwo");
    const std::string source = "namespace " + one + "\n{\n    class " + cls + " { " + cls +
                               "(int x) { } }\n}\nnamespace " + two + "\n{\n    class " + cls + " { " + cls +
                               "(int x) { } }\n}\nvoid main()\n{\n    " + one + "::" + cls + " first(1);\n    " + two +
                               "::" + cls + " second(1);\n}\n";
    const BindingFixture fixture(source);

    const CallDeclaration* first = fixture.DeclarationAt("first(");
    const CallDeclaration* second = fixture.DeclarationAt("second(");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->range.startLine == fixture.LineOf("    class " + cls));
    CHECK(second->range.startLine > first->range.startLine);
    CHECK_FALSE(*first == *second);
    CHECK(first->qualifiedName.find(one) != std::string::npos);
    CHECK(second->qualifiedName.find(two) != std::string::npos);
}

TEST_CASE("ConstructorBinding - A handle mention constructs nothing and publishes nothing")
{
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("Widget");
    const std::string source = "class " + a + " { " + a + "(int x) { } }\nvoid main()\n{\n    " + a + "@ handle;\n}\n";
    const BindingFixture fixture(source);

    CHECK(fixture.DeclarationAt("handle") == nullptr);
    const CallBindings* bindings = fixture.Bindings();
    CHECK((bindings == nullptr || bindings->Empty()));
}

TEST_CASE("ConstructorBinding - The use range is the declarator identifier")
{
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("Widget");
    const std::string source = "class " + a + " { " + a + "(int x) { } }\nvoid main()\n{\n    " + a + " local(1);\n}\n";
    const BindingFixture fixture(source);

    const CallBindings* bindings = fixture.Bindings();
    REQUIRE(bindings != nullptr);
    std::vector<SourceRange> uses;
    bindings->ForEachDeclaration([&](const CallDeclaration&, std::span<const SourceRange> ranges)
                                 { uses.insert(uses.end(), ranges.begin(), ranges.end()); });
    REQUIRE(uses.size() == 1);
    CHECK(uses[0].startLine == 3);
    CHECK(uses[0].startCharacter == 4 + a.size() + 1);
    CHECK(uses[0].endLine == 3);
    CHECK(uses[0].endCharacter == 4 + a.size() + 1 + std::string("local").size());
}

TEST_CASE("ConstructorBinding - Declarators that share one written type each get their own binding")
{
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("Widget");
    const std::string b = angel_lsp::test::GenerateRandomSymbolName("Left");
    const std::string c = angel_lsp::test::GenerateRandomSymbolName("Right");
    const std::string source = "class " + b + " { }\nclass " + c + " { }\nclass " + a + "\n{\n    " + a + "(" + b +
                               "@ l) { }\n    " + a + "(" + c + "@ r) { }\n}\nvoid main()\n{\n    " + b +
                               "@ hb;\n    " + c + "@ hc;\n    " + a + " grouped(hb), other(hc);\n}\n";
    const BindingFixture fixture(source);

    const CallDeclaration* grouped = fixture.DeclarationAt("grouped(");
    const CallDeclaration* other = fixture.DeclarationAt("other(");
    REQUIRE(grouped != nullptr);
    REQUIRE(other != nullptr);
    CHECK(grouped->range.startLine == fixture.LineOf(a + "(" + b + "@ l)"));
    CHECK(other->range.startLine == fixture.LineOf(a + "(" + c + "@ r)"));
    CHECK_FALSE(*grouped == *other);
}

TEST_CASE("ConstructorBinding - A declarator on another line than its type is still published")
{
    const std::string a = angel_lsp::test::GenerateRandomSymbolName("Widget");
    const std::string source =
        "class " + a + " { " + a + "(int x) { } }\nvoid main()\n{\n    " + a + "\n        split(1);\n}\n";
    const BindingFixture fixture(source);

    const CallBindings* bindings = fixture.Bindings();
    REQUIRE(bindings != nullptr);
    std::vector<SourceRange> uses;
    bindings->ForEachDeclaration([&](const CallDeclaration&, std::span<const SourceRange> ranges)
                                 { uses.insert(uses.end(), ranges.begin(), ranges.end()); });
    REQUIRE(uses.size() == 1);
    CHECK(uses[0].startLine == 4);
    CHECK(uses[0].startCharacter == 8);
    CHECK(uses[0].endCharacter == 8 + std::string("split").size());
}
