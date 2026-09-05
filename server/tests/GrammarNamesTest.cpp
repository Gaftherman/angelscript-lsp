#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

#include "parser/GrammarNames.h"

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace
{
    using namespace angel_lsp::parser;
}

// =====================================================================================
// The grammar is the authority on what a node type and a field name are called, and until this
// test existed nothing asked it. Twenty node types and four field names that the grammar has never
// defined were being compared against and looked up: `function_definition`, `subscript_expression`,
// `update_expression`, `compound_statement`, `initializer`, `argument` and the rest - names from
// the C, C++ and JavaScript grammars, copied in from another project's handler.
//
// Nineteen were dead weight, sitting in an `||` beside the correct name. One was a live bug.
//
// The forward-looking half matters more than the cleanup: the grammar is pinned by commit in
// cmake/TreeSitter.cmake and gets bumped. When a bump renames or removes a node, every rule that
// reads it stops matching, silently, and no existing check notices - PARITY-BACKLOG.md records
// that the parity audit structurally cannot, because an unparseable construct costs a symbol from
// the index rather than producing a diagnostic, and silence is what the audit reads as agreement.
// =====================================================================================

TEST_CASE("GrammarNames - every node type constant is a node type the grammar defines")
{
    const TSLanguage *language = tree_sitter_angelscript();
    REQUIRE(language != nullptr);

    std::vector<std::string> missing;
    for (const std::string_view name : k_allNodeTypes)
    {
        // `true` asks for a named node. An anonymous token of the same spelling does not count:
        // "class" the keyword is not "class_declaration" the node, and a rule that walks the tree
        // wants the second.
        const TSSymbol symbol = ts_language_symbol_for_name(
            language, name.data(), static_cast<uint32_t>(name.length()), true);

        if (symbol == 0)
            missing.emplace_back(name);
    }

    INFO("node types the grammar no longer defines: " << [&]
    {
        std::string joined;
        for (const std::string &name : missing)
        {
            if (!joined.empty())
                joined += ", ";
            joined += name;
        }
        return joined;
    }());

    CHECK(missing.empty());
}

TEST_CASE("GrammarNames - every field constant is a field the grammar defines")
{
    const TSLanguage *language = tree_sitter_angelscript();
    REQUIRE(language != nullptr);

    std::vector<std::string> missing;
    for (const std::string_view name : k_allFieldNames)
    {
        const TSFieldId field = ts_language_field_id_for_name(
            language, name.data(), static_cast<uint32_t>(name.length()));

        if (field == 0)
            missing.emplace_back(name);
    }

    INFO("fields the grammar no longer defines: " << [&]
    {
        std::string joined;
        for (const std::string &name : missing)
        {
            if (!joined.empty())
                joined += ", ";
            joined += name;
        }
        return joined;
    }());

    CHECK(missing.empty());
}

// A guard that can only pass is not a guard. GrammarNames.h is generated from the grammar's own
// node-types.json, so the two tests above will always pass on the day they are written - what has
// to be shown is that they would fail on the day the grammar changes underneath them.
//
// These are four of the twenty names that were really in this codebase, taken from the C, C++ and
// JavaScript grammars. If any of them ever starts resolving, this grammar has grown a node by that
// name and the checks above have a new meaning.
TEST_CASE("GrammarNames - the check can fail: names from other grammars do not resolve here")
{
    const TSLanguage *language = tree_sitter_angelscript();
    REQUIRE(language != nullptr);

    for (const std::string_view foreign : { "function_definition", "subscript_expression",
                                            "update_expression", "compound_statement" })
    {
        CAPTURE(foreign);
        CHECK(ts_language_symbol_for_name(language, foreign.data(),
                                          static_cast<uint32_t>(foreign.length()), true) == 0);
    }

    // Same for the field lookup, using the one that motivated a hand-written '=' scan in four
    // separate files: `value` is real, `initializer` never was.
    CHECK(ts_language_field_id_for_name(language, "value", 5) != 0);
    CHECK(ts_language_field_id_for_name(language, "initializer", 11) == 0);
}

// The other direction, and deliberately not a failure. A field the grammar defines and nothing
// reads is either a feature nobody wired up yet or a name that changed meaning; both are worth
// seeing after a pin bump, neither is wrong. Reported, not asserted - a test that fails because
// someone added a field upstream would only teach people to delete the test.
TEST_CASE("GrammarNames - what the grammar offers that this server never reads")
{
    const TSLanguage *language = tree_sitter_angelscript();
    REQUIRE(language != nullptr);

    const uint32_t fieldCount = ts_language_field_count(language);
    std::vector<std::string> unread;

    for (TSFieldId id = 1; id <= static_cast<TSFieldId>(fieldCount); ++id)
    {
        const char *name = ts_language_field_name_for_id(language, id);
        if (name == nullptr)
            continue;

        bool declared = false;
        for (const std::string_view known : k_allFieldNames)
        {
            if (known == name)
            {
                declared = true;
                break;
            }
        }

        if (!declared)
            unread.emplace_back(name);
    }

    MESSAGE("grammar fields: " << fieldCount << ", declared here: " << std::size(k_allFieldNames));
    for (const std::string &name : unread)
        MESSAGE("  not in GrammarNames.h: " << name);

    CHECK(true);
}
