#include "helpers/ProcessMemory.h"

#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <string>

namespace angel_lsp::test
{

/**
 * @brief Doctest listener that appends one line per test case with the memory it used.
 *
 * Active only when ANGELLSP_TEST_RSS_LOG names a writable file, so normal runs are unaffected.
 * Each line is `<peak KB>\t<resident growth KB>\t<resident after KB>\t<test name>`: the peak
 * exposes transient spikes, the growth exposes memory a test leaves behind (caches, statics).
 * Where the platform cannot reset the peak, the peak column is the lifetime peak.
 */
struct MemoryListener : doctest::IReporter
{
    std::ofstream log;
    std::string currentName;
    size_t residentBefore = 0;

    explicit MemoryListener([[maybe_unused]] const doctest::ContextOptions& options)
    {
        if (const char* path = std::getenv("ANGELLSP_TEST_RSS_LOG"))
        {
            log.open(path, std::ios::app);
        }
    }

    void report_query([[maybe_unused]] const doctest::QueryData& data) override
    {
    }
    void test_run_start() override
    {
    }
    void test_run_end([[maybe_unused]] const doctest::TestRunStats& stats) override
    {
    }
    void test_case_reenter([[maybe_unused]] const doctest::TestCaseData& data) override
    {
    }
    void test_case_exception([[maybe_unused]] const doctest::TestCaseException& exception) override
    {
    }
    void subcase_start([[maybe_unused]] const doctest::SubcaseSignature& signature) override
    {
    }
    void subcase_end() override
    {
    }
    void log_assert([[maybe_unused]] const doctest::AssertData& data) override
    {
    }
    void log_message([[maybe_unused]] const doctest::MessageData& data) override
    {
    }
    void test_case_skipped([[maybe_unused]] const doctest::TestCaseData& data) override
    {
    }

    void test_case_start(const doctest::TestCaseData& data) override
    {
        if (!log.is_open())
        {
            return;
        }
        currentName = data.m_name;
        ResetPeakMemory();
        residentBefore = CurrentResidentBytes();
    }

    void test_case_end([[maybe_unused]] const doctest::CurrentTestCaseStats& stats) override
    {
        if (!log.is_open())
        {
            return;
        }
        const size_t after = CurrentResidentBytes();
        const long long growthKb = (static_cast<long long>(after) - static_cast<long long>(residentBefore)) / 1024;
        log << PeakResidentBytes() / 1024 << '\t' << growthKb << '\t' << after / 1024 << '\t' << currentName << '\n';
        log.flush();
    }
};

REGISTER_LISTENER("angel_lsp_memory", 1, MemoryListener);

} // namespace angel_lsp::test
