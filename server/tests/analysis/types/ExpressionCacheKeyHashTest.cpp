#include "analysis/types/ExpressionTypeCache.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <unordered_set>

namespace angel_lsp::analysis
{
TEST_SUITE("analysis/types/ExpressionCacheKeyHash")
{
    TEST_CASE("ExpressionCacheKeyHash - spreads document-sized keys over the high bits used for bucketing")
    {
        constexpr uint64_t seed = 20261115ULL;
        MESSAGE("Seed: " << seed);
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<uint32_t> start(0, 1u << 21);
        std::uniform_int_distribution<uint32_t> length(1, 200);
        std::uniform_int_distribution<uint16_t> symbol(1, 400);

        constexpr size_t keyCount = 100000;
        constexpr unsigned bucketBits = 16;
        const ExpressionCacheKeyHash hasher;
        std::unordered_set<uint64_t> buckets;
        for (size_t i = 0; i < keyCount; ++i)
        {
            const uint32_t s = start(rng);
            const ExpressionCacheKey key{s, s + length(rng), symbol(rng)};
            buckets.insert(hasher(key) >> (64 - bucketBits));
        }

        // 100k uniform keys over 65536 buckets occupy about 78% of them; degenerate hashes occupy a few dozen.
        CHECK(buckets.size() > (size_t{1} << bucketBits) / 2);
    }
}
} // namespace angel_lsp::analysis
