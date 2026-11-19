#include "utils/HeapTrim.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <memory>
#include <numeric>
#include <vector>

TEST_SUITE("HeapTrim")
{
    TEST_CASE("Releasing free heap keeps live allocations intact")
    {
        std::vector<std::unique_ptr<std::vector<uint32_t>>> live;
        for (uint32_t i = 0; i < 64; ++i)
        {
            auto block = std::make_unique<std::vector<uint32_t>>(4096, i);
            if (i % 2 == 0)
            {
                live.push_back(std::move(block));
            }
        }

        angel_lsp::utils::ReleaseFreeHeapMemory();
        angel_lsp::utils::ReleaseFreeHeapMemory();

        uint32_t expected = 0;
        for (const auto& block : live)
        {
            CHECK(std::accumulate(block->begin(), block->end(), uint64_t{0}) == uint64_t{expected} * 4096);
            expected += 2;
        }
    }
}
