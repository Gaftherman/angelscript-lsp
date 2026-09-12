#include <doctest/doctest.h>
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include <vector>

namespace
{
    /**
     * @brief Helper to create a single 5-tuple token.
     */
    std::vector<lsp::uint> MakeToken(lsp::uint deltaLine, lsp::uint deltaChar, lsp::uint length, lsp::uint tokenType, lsp::uint tokenMod)
    {
        return { deltaLine, deltaChar, length, tokenType, tokenMod };
    }

    /**
     * @brief Helper to concatenate multiple token streams into a single flat vector.
     */
    std::vector<lsp::uint> Concat(const std::vector<std::vector<lsp::uint>> &tokens)
    {
        std::vector<lsp::uint> out;
        for (const auto &t : tokens)
        {
            out.insert(out.end(), t.begin(), t.end());
        }
        return out;
    }

    /**
     * @brief Reconstructs the target stream by applying LSP SemanticTokensEdit sequence to previous.
     */
    std::vector<lsp::uint> ApplyEdits(std::vector<lsp::uint> previous, const std::vector<lsp::SemanticTokensEdit> &edits)
    {
        if (edits.empty())
        {
            return previous;
        }

        for (const auto &edit : edits)
        {
            CHECK(edit.start % 5 == 0);
            CHECK(edit.deleteCount % 5 == 0);
            if (edit.data.has_value())
            {
                CHECK(edit.data->size() % 5 == 0);
            }

            const size_t start = static_cast<size_t>(edit.start);
            const size_t del = static_cast<size_t>(edit.deleteCount);
            REQUIRE(start <= previous.size());
            REQUIRE(start + del <= previous.size());

            auto it = previous.begin() + start;
            it = previous.erase(it, it + del);
            if (edit.data.has_value() && !edit.data->empty())
            {
                previous.insert(it, edit.data->begin(), edit.data->end());
            }
        }
        return previous;
    }
}

TEST_CASE("SemanticTokensDelta - Empty streams")
{
    std::vector<lsp::uint> prev = {};
    std::vector<lsp::uint> curr = {};

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    CHECK(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Identical streams")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(0, 5, 3, 2, 1);
    auto prev = Concat({ t0, t1 });
    auto curr = Concat({ t0, t1 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    CHECK(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Prepend token at beginning")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto t2 = MakeToken(1, 8, 3, 3, 0);

    auto prev = Concat({ t1, t2 });
    auto curr = Concat({ t0, t1, t2 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Append token at end")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto t2 = MakeToken(1, 8, 3, 3, 0);

    auto prev = Concat({ t0, t1 });
    auto curr = Concat({ t0, t1, t2 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Insert token in middle")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto t2 = MakeToken(1, 8, 3, 3, 0);

    auto prev = Concat({ t0, t2 });
    auto curr = Concat({ t0, t1, t2 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Delete token from middle")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto t2 = MakeToken(1, 8, 3, 3, 0);

    auto prev = Concat({ t0, t1, t2 });
    auto curr = Concat({ t0, t2 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Modify token in middle")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto t1_mod = MakeToken(1, 2, 8, 2, 1);
    auto t2 = MakeToken(1, 8, 3, 3, 0);

    auto prev = Concat({ t0, t1, t2 });
    auto curr = Concat({ t0, t1_mod, t2 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Replace all tokens")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto t2 = MakeToken(2, 0, 6, 3, 1);
    auto t3 = MakeToken(3, 4, 2, 4, 0);

    auto prev = Concat({ t0, t1 });
    auto curr = Concat({ t2, t3 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Clear all tokens (previous to empty)")
{
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);

    auto prev = Concat({ t0, t1 });
    std::vector<lsp::uint> curr = {};

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Populate from empty (empty to current)")
{
    std::vector<lsp::uint> prev = {};
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto curr = Concat({ t0, t1 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE_FALSE(edits.empty());
    CHECK(ApplyEdits(prev, edits) == curr);
}

TEST_CASE("SemanticTokensDelta - Strict 5-integer alignment invariant")
{
    // Test that partial changes inside a token (e.g. only tokenModifier changes)
    // always expand edit bounds to a multiple of 5.
    auto t0 = MakeToken(0, 0, 4, 1, 0);
    auto t1 = MakeToken(1, 2, 5, 2, 0);
    auto t2 = MakeToken(2, 4, 6, 3, 0);

    // t1 only differs in tokenModifier (last element of 5-tuple)
    auto t1_changed_mod = MakeToken(1, 2, 5, 2, 4);

    auto prev = Concat({ t0, t1, t2 });
    auto curr = Concat({ t0, t1_changed_mod, t2 });

    auto edits = angel_lsp::features::ComputeSemanticTokensDelta(prev, curr);
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].start % 5 == 0);
    CHECK(edits[0].deleteCount % 5 == 0);
    REQUIRE(edits[0].data.has_value());
    CHECK(edits[0].data->size() % 5 == 0);

    // Specifically, only t1 should be edited (start = 5, deleteCount = 5, data size = 5)
    CHECK(edits[0].start == 5);
    CHECK(edits[0].deleteCount == 5);
    CHECK(edits[0].data->size() == 5);

    CHECK(ApplyEdits(prev, edits) == curr);
}
