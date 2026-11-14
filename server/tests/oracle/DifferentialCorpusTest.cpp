#include "helpers/McpTestSession.h"

#include <algorithm>
#include <cctype>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

/**
 * Replays the differential corpus (tests/parity/differential) against the MCP bridge. Each case header
 * was confirmed by the real compiler through scripts/differential-validate.py, so this test needs no
 * oracle: it only checks that the server still agrees with the recorded verdicts.
 */
TEST_SUITE("oracle/DifferentialCorpus")
{
    /** @brief One corpus case: the main script plus any sibling files reached through #include. */
    struct CorpusCase
    {
        std::string id;
        std::string mainText;
        std::vector<std::pair<std::string, std::string>> siblings;
        std::string expect;
        std::vector<std::string> required;
        std::vector<std::string> forbidden;
    };

    static std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    static std::string Trim(std::string text)
    {
        const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), isSpace));
        text.erase(std::find_if_not(text.rbegin(), text.rend(), isSpace).base(), text.end());
        return text;
    }

    static CorpusCase LoadCase(const std::filesystem::path& entry)
    {
        CorpusCase result;
        const bool isDirectory = std::filesystem::is_directory(entry);
        result.id =
            entry.parent_path().filename().string() + "/" + (isDirectory ? entry.filename() : entry.stem()).string();
        result.mainText = ReadFile(isDirectory ? entry / "main.as" : entry);
        if (isDirectory)
        {
            for (const auto& sibling : std::filesystem::directory_iterator(entry))
            {
                if (sibling.path().extension() == ".as" && sibling.path().filename() != "main.as")
                {
                    result.siblings.emplace_back(sibling.path().filename().string(), ReadFile(sibling.path()));
                }
            }
        }
        std::istringstream lines(result.mainText);
        std::string line;
        while (std::getline(lines, line))
        {
            line = Trim(line);
            const auto take = [&line](std::string_view tag) -> std::string
            { return line.rfind(tag, 0) == 0 ? Trim(line.substr(tag.size())) : std::string(); };
            if (const std::string value = take("// @expect:"); !value.empty())
            {
                result.expect = value;
            }
            else if (const std::string plus = take("// @complete+"); !plus.empty())
            {
                result.required.push_back(plus);
            }
            else if (const std::string minus = take("// @complete-"); !minus.empty())
            {
                result.forbidden.push_back(minus);
            }
        }
        return result;
    }

    static std::vector<CorpusCase> LoadCorpus()
    {
        std::vector<CorpusCase> cases;
        const std::filesystem::path root = std::filesystem::path(ANGELSCRIPT_PARITY_DIR) / "differential";
        if (!std::filesystem::exists(root))
        {
            return cases;
        }
        std::vector<std::filesystem::path> entries;
        for (const auto& category : std::filesystem::directory_iterator(root))
        {
            // Directories starting with '_' hold gaps already reported by the validator.
            if (!category.is_directory() || category.path().filename().string().rfind('_', 0) == 0)
            {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(category.path()))
            {
                if (entry.path().extension() == ".as" || std::filesystem::exists(entry.path() / "main.as"))
                {
                    entries.push_back(entry.path());
                }
            }
        }
        std::sort(entries.begin(), entries.end());
        for (const auto& entry : entries)
        {
            cases.push_back(LoadCase(entry));
        }
        return cases;
    }

    TEST_CASE("DifferentialCorpus - server agrees with the verdicts the compiler confirmed")
    {
        static constexpr std::string_view k_marker = "/*$complete*/";
        static constexpr int k_keywordKind = 14;
        static constexpr int k_snippetKind = 15;
        const std::vector<CorpusCase> cases = LoadCorpus();
        REQUIRE_FALSE(cases.empty());

        for (const CorpusCase& item : cases)
        {
            INFO("case: " << item.id);
            const size_t markerAt = item.mainText.find(k_marker);
            std::string mainText = item.mainText;
            int line = 0;
            int character = 0;
            if (markerAt != std::string::npos)
            {
                const std::string before = item.mainText.substr(0, markerAt);
                mainText = before + item.mainText.substr(markerAt + k_marker.size());
                line = static_cast<int>(std::count(before.begin(), before.end(), '\n'));
                character = static_cast<int>(before.size() -
                                             (before.rfind('\n') == std::string::npos ? 0 : before.rfind('\n') + 1));
            }

            angel_lsp::test::McpTestSession session;
            std::vector<std::pair<std::string, std::string>> files = item.siblings;
            files.emplace_back("main.as", mainText);
            for (const auto& [name, text] : files)
            {
                std::ofstream(session.root / name, std::ios::binary) << text;
            }
            for (const auto& [name, text] : files)
            {
                session.Call("open_document", angel_lsp::test::McpArgs(name, text));
            }
            const int diagnostics = session.Call("diagnostics", angel_lsp::test::McpPathArgs("main.as"));
            lsp::json::Value completionArgs = angel_lsp::test::McpPositionArgs("main.as", line, character);
            completionArgs.object()["limit"] = 500;
            const int completion = session.Call("completion", std::move(completionArgs));
            const auto replies = session.Run();

            if (markerAt == std::string::npos)
            {
                int errors = 0;
                const lsp::json::Value report = angel_lsp::test::McpPayload(replies.at(diagnostics));
                for (const auto& diag : report.array())
                {
                    const lsp::json::Value* severity = diag.object().find("severity");
                    errors += (severity != nullptr && severity->isInteger() && severity->integer() == 1) ? 1 : 0;
                }
                CHECK((errors == 0) == (item.expect == "accept"));
                continue;
            }

            std::set<std::string> labels;
            const lsp::json::Value payload = angel_lsp::test::McpPayload(replies.at(completion));
            for (const auto& entry : payload.object().get("items").array())
            {
                const lsp::json::Value* kind = entry.object().find("kind");
                const int kindValue = (kind != nullptr && kind->isInteger()) ? static_cast<int>(kind->integer()) : 0;
                if (kindValue != k_keywordKind && kindValue != k_snippetKind)
                {
                    labels.insert(entry.object().get("label").string());
                }
            }
            for (const std::string& label : item.required)
            {
                CHECK_MESSAGE(labels.count(label) == 1, "missing completion: " << label);
            }
            for (const std::string& label : item.forbidden)
            {
                CHECK_MESSAGE(labels.count(label) == 0, "leaked completion: " << label);
            }
        }
    }

    /** @brief Diagnostics a one-file script produces, as the severity-1 messages. */
    static std::vector<std::string> ErrorsFor(const std::string& script)
    {
        angel_lsp::test::McpTestSession session;
        std::ofstream(session.root / "main.as", std::ios::binary) << script;
        session.Call("open_document", angel_lsp::test::McpArgs("main.as", script));
        const int id = session.Call("diagnostics", angel_lsp::test::McpPathArgs("main.as"));
        const auto replies = session.Run();
        const lsp::json::Value report = angel_lsp::test::McpPayload(replies.at(id));
        std::vector<std::string> errors;
        for (const auto& diag : report.array())
        {
            const lsp::json::Value* severity = diag.object().find("severity");
            if (severity != nullptr && severity->isInteger() && severity->integer() == 1)
            {
                errors.push_back(diag.object().get("message").string());
            }
        }
        return errors;
    }

    TEST_CASE("DifferentialCorpus - namespaced members and qualified calls follow the compiler for any names")
    {
        for (int round = 0; round < 5; ++round)
        {
            const std::string ns = angel_lsp::test::GenerateRandomSymbolName("Ns");
            const std::string type = angel_lsp::test::GenerateRandomSymbolName("Ty");
            const std::string field = angel_lsp::test::GenerateRandomSymbolName("fld");
            const std::string func = angel_lsp::test::GenerateRandomSymbolName("fn");
            const std::string ghost = angel_lsp::test::GenerateRandomSymbolName("ghost");
            INFO("round " << round << " " << ns << " " << type);

            const std::string nl = "\n";
            const std::string classBody = "class " + type + " { private int " + field + "; int open; }" + nl;
            const std::string useField = "void use() { " + type + " v; int x = v." + field + "; } }" + nl;
            const std::string useOpen = "void use() { " + type + " v; int x = v.open; } }" + nl;
            const std::string declareFunc = "namespace " + ns + " { void " + func + "() {} }" + nl;
            CHECK(ErrorsFor("namespace " + ns + " { " + classBody + useField).size() == 1);
            CHECK(ErrorsFor("namespace " + ns + " { " + classBody + useOpen).empty());
            CHECK(ErrorsFor(declareFunc + "void main() { " + ns + "::" + func + "(); }" + nl).empty());
            CHECK(ErrorsFor(declareFunc + "void main() { " + ns + "::" + ghost + "(); }" + nl).size() == 1);
        }
    }
}
