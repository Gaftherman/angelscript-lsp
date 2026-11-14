#include "parser/DoxygenSyntax.h"

#include "helpers/TestUtils.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE("analysis/documentation/DoxygenSyntax")
{
    /**
     * @brief Finds the first syntax node view matching a requested kind using breadth-first traversal.
     * @param[in] root The starting node view for the search.
     * @param[in] targetKind The grammatical node kind to locate.
     * @return The first matching DoxygenNodeView, or a null view if not found.
     */
    static angel_lsp::parser::DoxygenNodeView FindFirstKind(angel_lsp::parser::DoxygenNodeView root,
                                                            angel_lsp::parser::DoxygenNodeKind targetKind)
    {
        if (root.IsNull())
        {
            return angel_lsp::parser::DoxygenNodeView();
        }
        std::vector<angel_lsp::parser::DoxygenNodeView> worklist{root};
        for (size_t i = 0; i < worklist.size(); ++i)
        {
            const angel_lsp::parser::DoxygenNodeView current = worklist[i];
            if (current.Kind() == targetKind)
            {
                return current;
            }
            for (uint32_t c = 0, count = current.ChildCount(); c < count; ++c)
            {
                const angel_lsp::parser::DoxygenNodeView child = current.Child(c);
                if (!child.IsNull())
                {
                    worklist.push_back(child);
                }
            }
        }
        return angel_lsp::parser::DoxygenNodeView();
    }

    TEST_CASE("DoxygenSyntax - Param tag")
    {
        const std::string id = angel_lsp::test::GenerateRandomSymbolName();
        const std::string input = "/**\n * @param[in] " + id + " the value\n */";
        const angel_lsp::parser::DoxygenSyntaxFacts facts = angel_lsp::parser::ExtractDoxygenSyntax(input);
        const angel_lsp::parser::DoxygenNodeView root = angel_lsp::parser::DoxygenNodeView::Root(facts, input);

        const angel_lsp::parser::DoxygenNodeView tagNode = FindFirstKind(root, angel_lsp::parser::DoxygenNodeKind::Tag);
        CHECK(!tagNode.IsNull());

        const angel_lsp::parser::DoxygenNodeView tagName =
            tagNode.FindChild(angel_lsp::parser::DoxygenNodeKind::TagName);
        CHECK(!tagName.IsNull());
        CHECK(tagName.Text().find("param") != std::string_view::npos);

        const angel_lsp::parser::DoxygenNodeView storageClass =
            tagNode.FindChild(angel_lsp::parser::DoxygenNodeKind::StorageClass);
        CHECK(!storageClass.IsNull());

        const angel_lsp::parser::DoxygenNodeView identifier =
            tagNode.FindChild(angel_lsp::parser::DoxygenNodeKind::Identifier);
        CHECK(!identifier.IsNull());
        CHECK(identifier.Text() == id);

        const angel_lsp::parser::DoxygenNodeView description =
            tagNode.FindChild(angel_lsp::parser::DoxygenNodeKind::Description);
        CHECK(!description.IsNull());
    }

    TEST_CASE("DoxygenSyntax - Code block")
    {
        const std::string id = angel_lsp::test::GenerateRandomSymbolName();
        const std::string input = "/**\n * @code{.cpp}\n * int " + id + " = 1;\n * @endcode\n */";
        const angel_lsp::parser::DoxygenSyntaxFacts facts = angel_lsp::parser::ExtractDoxygenSyntax(input);
        const angel_lsp::parser::DoxygenNodeView root = angel_lsp::parser::DoxygenNodeView::Root(facts, input);

        const angel_lsp::parser::DoxygenNodeView codeBlock =
            FindFirstKind(root, angel_lsp::parser::DoxygenNodeKind::CodeBlock);
        CHECK(!codeBlock.IsNull());

        const angel_lsp::parser::DoxygenNodeView content =
            codeBlock.FindChild(angel_lsp::parser::DoxygenNodeKind::CodeBlockContent);
        CHECK(!content.IsNull());
        CHECK(content.Text().find(id) != std::string_view::npos);

        const angel_lsp::parser::DoxygenNodeView lang =
            codeBlock.FindChild(angel_lsp::parser::DoxygenNodeKind::CodeBlockLanguage);
        if (!lang.IsNull())
        {
            CHECK(lang.Text().find("cpp") != std::string_view::npos);
        }
    }

    TEST_CASE("DoxygenSyntax - Inline nodes")
    {
        const std::string id = angel_lsp::test::GenerateRandomSymbolName();
        const std::string input = "/**\n * @brief Uses \\c " + id + " here.\n */";
        const angel_lsp::parser::DoxygenSyntaxFacts facts = angel_lsp::parser::ExtractDoxygenSyntax(input);
        const angel_lsp::parser::DoxygenNodeView root = angel_lsp::parser::DoxygenNodeView::Root(facts, input);

        const angel_lsp::parser::DoxygenNodeView codeWord =
            FindFirstKind(root, angel_lsp::parser::DoxygenNodeKind::CodeWord);
        CHECK(!codeWord.IsNull());
        CHECK(codeWord.Text().find(id) != std::string_view::npos);
    }

    TEST_CASE("DoxygenSyntax - Structure invariant")
    {
        const std::string id = angel_lsp::test::GenerateRandomSymbolName();
        const std::string input = "/**\n * @param[in] " + id + " the value\n */";
        const angel_lsp::parser::DoxygenSyntaxFacts facts = angel_lsp::parser::ExtractDoxygenSyntax(input);
        REQUIRE(!facts.nodes.empty());

        std::vector<uint32_t> parentCount(facts.nodes.size(), 0);
        for (size_t p = 0; p < facts.nodes.size(); ++p)
        {
            const angel_lsp::parser::DoxygenNodeFact& parent = facts.nodes[p];
            const uint32_t childEnd = parent.firstChild + parent.childCount;
            CHECK(childEnd <= facts.nodes.size());
            for (uint32_t c = parent.firstChild; c < childEnd; ++c)
            {
                if (c < facts.nodes.size())
                {
                    ++parentCount[c];
                    CHECK(facts.nodes[c].startByte >= parent.startByte);
                    CHECK(facts.nodes[c].endByte <= parent.endByte);
                }
            }
        }

        for (size_t i = 1; i < facts.nodes.size(); ++i)
        {
            CHECK(parentCount[i] == 1);
        }
    }

    TEST_CASE("DoxygenSyntax - Empty and whitespace input")
    {
        const std::string_view emptyInput = "";
        const angel_lsp::parser::DoxygenSyntaxFacts emptyFacts = angel_lsp::parser::ExtractDoxygenSyntax(emptyInput);
        CHECK(emptyFacts.nodes.empty());
        CHECK(angel_lsp::parser::DoxygenNodeView::Root(emptyFacts, emptyInput).IsNull());

        const std::string_view wsInput = "   \n \t \r\n  ";
        const angel_lsp::parser::DoxygenSyntaxFacts wsFacts = angel_lsp::parser::ExtractDoxygenSyntax(wsInput);
        CHECK(wsFacts.nodes.empty());
        CHECK(angel_lsp::parser::DoxygenNodeView::Root(wsFacts, wsInput).IsNull());
    }

    TEST_CASE("DoxygenSyntax - Null view accessors")
    {
        const angel_lsp::parser::DoxygenNodeView nullView;
        CHECK(nullView.IsNull());
        CHECK(nullView.ChildCount() == 0);
        CHECK(nullView.Text().empty());
        CHECK(nullView.FindChild(angel_lsp::parser::DoxygenNodeKind::Tag).IsNull());
    }
}
