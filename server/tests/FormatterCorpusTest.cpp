#include <doctest/doctest.h>
#include "helpers/CorpusDirectory.h"
#include "features/formatting/FormattingHandler.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;

namespace fs = std::filesystem;

/**
 * @brief The formatter, run over every script whose compiler verdict this project already knows.
 *
 * A formatter is the one feature that writes the user's file back, so "it produced something" is
 * not a result - the question is whether what it produced still says the same thing. Two
 * properties answer that without a compiler in the loop:
 *
 *   - **The token stream is unchanged.** Formatting moves whitespace and nothing else. If the
 *     sequence of non-whitespace tokens differs, a character was invented, dropped or merged, and
 *     the file means something different now. This is what caught nothing on the first run and is
 *     kept because it is what would catch the next brace rule that goes wrong.
 *   - **Formatting is idempotent.** Running it twice must give what running it once gave. A
 *     formatter that keeps moving is one that will fight the editor's format-on-save forever.
 *
 * The stronger check - compile the original and the formatted file and compare the compiler's own
 * verdict - lives outside the suite, because it needs the oracle binary. Dump the formatted corpus
 * with ANGELLSP_FORMAT_DUMP_DIR set and run angelscript_oracle over both trees:
 *
 *     ANGELLSP_FORMAT_DUMP_DIR=/tmp/fmt angel_lsp_tests.exe --test-case="*formatter corpus*"
 */
namespace
{
    std::string ReadWholeFile(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    /**
     * @brief The file's non-whitespace tokens, as coarsely as it takes to notice a lost character.
     *
     * Deliberately not the formatter's own tokenizer: a bug shared by the formatter and the checker
     * would cancel out and the check would pass on a corrupted file. This one only has to agree on
     * where a token ends, and it keeps comments and string literals whole so a brace or a semicolon
     * inside one is never mistaken for code.
     */
    std::vector<std::string> CoarseTokens(const std::string &text)
    {
        std::vector<std::string> tokens;
        size_t i = 0;

        auto isWordChar = [](char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        };

        while (i < text.size())
        {
            const char c = text[i];

            if (std::isspace(static_cast<unsigned char>(c)))
            {
                ++i;
                continue;
            }

            const size_t start = i;

            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
            {
                while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
            }
            else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
                i = std::min(text.size(), i + 2);
            }
            else if (c == '"' && i + 2 < text.size() && text[i + 1] == '"' && text[i + 2] == '"')
            {
                i += 3;
                while (i + 2 < text.size() &&
                       !(text[i] == '"' && text[i + 1] == '"' && text[i + 2] == '"')) ++i;
                i = std::min(text.size(), i + 3);
            }
            else if (c == '"' || c == '\'')
            {
                const char quote = c;
                ++i;
                while (i < text.size() && text[i] != quote && text[i] != '\n')
                {
                    if (text[i] == '\\' && i + 1 < text.size()) ++i;
                    ++i;
                }
                if (i < text.size() && text[i] == quote) ++i;
            }
            else if (c == '#')
            {
                while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
            }
            else if (isWordChar(c))
            {
                while (i < text.size() && isWordChar(text[i])) ++i;
            }
            else
            {
                ++i;
            }

            tokens.push_back(text.substr(start, i - start));
        }

        return tokens;
    }

    /** @brief A line comment's text is whitespace-trimmed at the end, which is not a lost token. */
    std::string NormalizeToken(std::string token)
    {
        while (!token.empty() &&
               std::isspace(static_cast<unsigned char>(token.back())) != 0)
        {
            token.pop_back();
        }
        return token;
    }
}

TEST_SUITE("Formatting")
{
    TEST_CASE("The formatter corpus keeps every token and settles in one pass")
    {
        const fs::path parityDir{ ANGELSCRIPT_PARITY_DIR };
        REQUIRE_MESSAGE(fs::exists(parityDir), "parity corpus directory is missing");

        std::vector<fs::path> scripts;
        for (const auto &entry : fs::directory_iterator(parityDir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".as")
            {
                scripts.push_back(entry.path());
            }
        }
        REQUIRE_MESSAGE(!scripts.empty(), "parity corpus is empty");

        fs::path dumpDir;
        if (const char *dump = std::getenv("ANGELLSP_FORMAT_DUMP_DIR"); dump && *dump)
        {
            dumpDir = fs::path(dump);
            std::error_code ec;
            fs::create_directories(dumpDir, ec);
        }

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        size_t checked = 0;
        for (const auto &script : scripts)
        {
            const std::string original = ReadWholeFile(script);
            if (original.empty())
            {
                continue;
            }

            for (const BraceStyle style : { BraceStyle::Allman, BraceStyle::KAndR })
            {
                const std::string formatted = FormatSourceCode(original, options, style);

                std::vector<std::string> before = CoarseTokens(original);
                std::vector<std::string> after = CoarseTokens(formatted);
                for (auto &t : before) t = NormalizeToken(std::move(t));
                for (auto &t : after) t = NormalizeToken(std::move(t));

                INFO("script: " << script.filename().string()
                                << " style: " << (style == BraceStyle::Allman ? "allman" : "kr"));
                CHECK(before == after);

                const std::string twice = FormatSourceCode(formatted, options, style);
                CHECK(twice == formatted);

                if (!dumpDir.empty() && style == BraceStyle::Allman)
                {
                    std::ofstream out(dumpDir / script.filename(), std::ios::binary);
                    out << formatted;
                }
            }

            ++checked;
        }

        MESSAGE("formatter corpus: " << checked << " scripts, both brace styles");
    }

    /**
     * @brief The same two properties over the full 1061-file corpus of real scripts.
     *
     * skip()-decorated for the same reason the parity audit is: it walks a tree that need not be
     * checked out, and a plain `ctest` should not depend on it. Run it on demand:
     *
     *     Debug/angel_lsp_tests.exe --no-skip --test-case="*full corpus*"
     */
    TEST_CASE("The formatter keeps every token across the full corpus" * doctest::skip())
    {
        if (!angel_lsp::test::CorpusIsAvailable())
        {
            MESSAGE(angel_lsp::test::CorpusMissingMessage());
            return;
        }
        const fs::path corpusDir = angel_lsp::test::CorpusDirectory();

        fs::path dumpDir;
        if (const char *dump = std::getenv("ANGELLSP_FORMAT_DUMP_DIR"); dump && *dump)
        {
            dumpDir = fs::path(dump);
            std::error_code mkEc;
            fs::create_directories(dumpDir, mkEc);
        }

        lsp::FormattingOptions options;
        options.tabSize = 4;
        options.insertSpaces = true;

        size_t checked = 0;
        size_t tokenMismatches = 0;
        size_t unstable = 0;

        std::error_code ec;
        for (fs::recursive_directory_iterator it(corpusDir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec || !it->is_regular_file() || it->path().extension() != ".as")
            {
                continue;
            }

            const std::string original = ReadWholeFile(it->path());
            if (original.empty())
            {
                continue;
            }

            const std::string formatted = FormatSourceCode(original, options);

            std::vector<std::string> before = CoarseTokens(original);
            std::vector<std::string> after = CoarseTokens(formatted);
            for (auto &t : before) t = NormalizeToken(std::move(t));
            for (auto &t : after) t = NormalizeToken(std::move(t));

            if (before != after)
            {
                ++tokenMismatches;
                size_t d = 0;
                while (d < before.size() && d < after.size() && before[d] == after[d]) ++d;
                MESSAGE("token stream changed: " << it->path().filename().string()
                        << " at #" << d
                        << " before=[" << (d < before.size() ? before[d] : std::string("<end>"))
                        << "] after=[" << (d < after.size() ? after[d] : std::string("<end>")) << "]");

                if (!dumpDir.empty())
                {
                    std::ofstream out(dumpDir / it->path().filename(), std::ios::binary);
                    out << formatted;
                }
            }
            if (FormatSourceCode(formatted, options) != formatted)
            {
                ++unstable;
                MESSAGE("not idempotent: " << it->path().string());
            }

            ++checked;
        }

        MESSAGE("full corpus: " << checked << " scripts, " << tokenMismatches
                                << " token mismatches, " << unstable << " unstable");
        CHECK(tokenMismatches == 0);
        CHECK(unstable == 0);
    }
}
