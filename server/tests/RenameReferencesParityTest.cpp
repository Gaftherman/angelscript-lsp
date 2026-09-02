#include <doctest/doctest.h>

#include "features/references/ReferencesHandler.h"
#include "features/rename/RenameHandler.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Rename and find-references have to agree, at every position, on every file.
//
// They are the same algorithm written twice - ~780 lines whose only non-mechanical difference is
// the guard on the token under the cursor. Everything else that differs between the two files is
// whitespace, a comment, or `sourceCode` against `request.sourceCode`.
//
// That duplication is not itself a defect. What it risks is: a resolution fix applied to one and
// not the other, after which rename edits a different set of occurrences than find-references
// shows. The user is then told what will change, and something else changes. There is no diagnostic
// for that and no test would have caught it, because each file's own tests would still pass.
//
// So this sweeps every identifier position in each sample and requires the two to return the same
// set of ranges. It is cheap, it is the guard that makes extracting the shared core safe, and it
// stays afterwards - a shared core can still be given a caller-specific branch by mistake.
// =====================================================================================

namespace
{
    /** @brief One occurrence, in the only form the two APIs can be compared through. */
    using Occurrence = std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t>;

    struct ParityEnv
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::unordered_set<std::string> predefinedUris;
        std::unordered_map<std::string, std::string> sources;
        std::unordered_map<std::string, TSTree *> trees;

        void AddFile(const std::string &uri, const std::string &code)
        {
            sources[uri] = code;
            trees[uri] = parser.Parse(code);
            symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
            if (auto rootScope = scopeCollector.CollectScopes(code, parser))
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~ParityEnv()
        {
            for (auto &[uri, tree] : trees)
            {
                if (tree)
                    ts_tree_delete(tree);
            }
        }

        std::set<Occurrence> ReferencesAt(const std::string &uri, lsp::Position position)
        {
            ReferencesRequest request{ uri, sources[uri], trees[uri], position,
                                       /*includeDeclaration=*/true, symbolTable, scopeIndex };

            std::set<Occurrence> found;
            if (auto refs = GetReferences(request))
            {
                for (const auto &location : *refs)
                {
                    found.emplace(location.uri.toString(), location.range.start.line,
                                  location.range.start.character, location.range.end.line,
                                  location.range.end.character);
                }
            }
            return found;
        }

        std::set<Occurrence> RenameAt(const std::string &uri, lsp::Position position)
        {
            RenameRequest request{ uri,          sources[uri], trees[uri], position, "renamed_",
                                   symbolTable,  scopeIndex,   predefinedUris };

            std::set<Occurrence> found;
            const auto edit = Rename(request);
            if (!edit.has_value() || !edit->changes.has_value())
                return found;

            for (const auto &[documentUri, edits] : *edit->changes)
            {
                for (const auto &textEdit : edits)
                {
                    found.emplace(documentUri.toString(), textEdit.range.start.line,
                                  textEdit.range.start.character, textEdit.range.end.line,
                                  textEdit.range.end.character);
                }
            }
            return found;
        }
    };

    /** @brief Every position that sits on an identifier character, one per character. */
    std::vector<lsp::Position> IdentifierPositions(const std::string &source)
    {
        std::vector<lsp::Position> positions;
        uint32_t line = 0;
        uint32_t character = 0;

        for (const char c : source)
        {
            if (c == '\n')
            {
                ++line;
                character = 0;
                continue;
            }
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_')
                positions.push_back(lsp::Position{ line, character });
            ++character;
        }

        return positions;
    }

    std::string Describe(const std::set<Occurrence> &occurrences)
    {
        std::string text;
        for (const auto &[uri, sl, sc, el, ec] : occurrences)
        {
            text += uri + ":" + std::to_string(sl) + ":" + std::to_string(sc) + "-" +
                    std::to_string(el) + ":" + std::to_string(ec) + " ";
        }
        return text.empty() ? "<none>" : text;
    }

    /**
     * @brief Sweeps one file and requires the two features to agree everywhere.
     *
     * A position where rename declines outright is not a disagreement: rename refuses a token that
     * is not a legal identifier, which find-references has no reason to. It is only a disagreement
     * when both produce edits and the sets differ, or when rename produces some and references
     * produces none.
     */
    void RequireAgreement(ParityEnv &env, const std::string &uri)
    {
        size_t comparedWithResults = 0;

        for (const lsp::Position position : IdentifierPositions(env.sources[uri]))
        {
            const auto references = env.ReferencesAt(uri, position);
            const auto renames = env.RenameAt(uri, position);

            if (renames.empty())
                continue;

            ++comparedWithResults;

            INFO("at " << uri << ":" << position.line << ":" << position.character
                       << "\n  references: " << Describe(references)
                       << "\n  rename:     " << Describe(renames));
            CHECK(references == renames);
        }

        // Without this the loop above passes on a file where rename never resolves anything, which
        // is exactly the failure this test exists to notice.
        CHECK(comparedWithResults > 0);
    }
}

TEST_CASE("Rename and references agree on a local variable and its uses")
{
    ParityEnv env;
    env.AddFile("file:///main.as",
                "void main()\n"
                "{\n"
                "    int counter = 0;\n"
                "    counter = counter + 1;\n"
                "    Print(counter);\n"
                "}\n"
                "void Print(int v) { }\n");

    RequireAgreement(env, "file:///main.as");
}

TEST_CASE("Rename and references agree on class members across files")
{
    ParityEnv env;
    env.AddFile("file:///entity.as",
                "class Entity\n"
                "{\n"
                "    int health;\n"
                "    void Damage(int amount) { health = health - amount; }\n"
                "    void Heal(int amount) { health = health + amount; }\n"
                "}\n");
    env.AddFile("file:///game.as",
                "void Update(Entity@ e)\n"
                "{\n"
                "    e.Damage(5);\n"
                "    e.Heal(2);\n"
                "}\n");

    RequireAgreement(env, "file:///entity.as");
    RequireAgreement(env, "file:///game.as");
}

TEST_CASE("Rename and references agree through inheritance")
{
    ParityEnv env;
    env.AddFile("file:///shapes.as",
                "class Shape\n"
                "{\n"
                "    void Draw() { }\n"
                "}\n"
                "class Circle : Shape\n"
                "{\n"
                "    void Draw() { }\n"
                "}\n"
                "void main()\n"
                "{\n"
                "    Circle c;\n"
                "    c.Draw();\n"
                "}\n");

    RequireAgreement(env, "file:///shapes.as");
}

TEST_CASE("Rename and references agree on namespaced symbols")
{
    ParityEnv env;
    env.AddFile("file:///ns.as",
                "namespace Game\n"
                "{\n"
                "    int score;\n"
                "    void Reset() { score = 0; }\n"
                "}\n"
                "void main()\n"
                "{\n"
                "    Game::Reset();\n"
                "    Game::score = 1;\n"
                "}\n");

    RequireAgreement(env, "file:///ns.as");
}

TEST_CASE("Rename and references agree on a global function called from several places")
{
    ParityEnv env;
    env.AddFile("file:///util.as",
                "int Clamp(int v, int lo, int hi)\n"
                "{\n"
                "    if (v < lo) return lo;\n"
                "    if (v > hi) return hi;\n"
                "    return v;\n"
                "}\n"
                "void main()\n"
                "{\n"
                "    int a = Clamp(1, 0, 2);\n"
                "    int b = Clamp(a, 0, 2);\n"
                "}\n");

    RequireAgreement(env, "file:///util.as");
}

TEST_CASE("Rename and references agree where a local shadows a member")
{
    // The case most likely to drift, because it is the one where the two files' scope walks have to
    // reach the same answer about which declaration the cursor is on.
    ParityEnv env;
    env.AddFile("file:///shadow.as",
                "class Holder\n"
                "{\n"
                "    int value;\n"
                "    void Use()\n"
                "    {\n"
                "        int value = 3;\n"
                "        value = value + 1;\n"
                "    }\n"
                "    void Other() { value = 9; }\n"
                "}\n");

    RequireAgreement(env, "file:///shadow.as");
}
