#include "helpers/McpTestSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

/**
 * Replays the differential corpus (tests/parity/differential) against the MCP bridge. Each case header
 * was confirmed by the real compiler through scripts/differential-validate.py, so this test needs no
 * oracle: it only checks that the server still agrees with the recorded verdicts.
 */
TEST_SUITE("oracle/DifferentialCorpus")
{
    /** @brief Recorded server error in a case-relative file. */
    struct ExpectedDiagnostic
    {
        std::string file = "main.as";
        /** @brief Required code, empty for legacy range-only tags. */
        std::string code;
        /** @brief Zero-based start and end positions with UTF-16 columns. */
        std::array<int, 4> range{};
    };

    using DiagnosticReports = std::map<std::string, lsp::json::Value>;

    static std::array<int, 4> DiagnosticRange(const lsp::json::Value& diagnostic)
    {
        const auto& range = diagnostic.object().get("range").object();
        const auto& start = range.get("start").object();
        const auto& end = range.get("end").object();
        return {static_cast<int>(start.get("line").integer()), static_cast<int>(start.get("character").integer()),
                static_cast<int>(end.get("line").integer()), static_cast<int>(end.get("character").integer())};
    }

    static bool IsError(const lsp::json::Value& diagnostic)
    {
        const auto* severity = diagnostic.object().find("severity");
        return severity && severity->isInteger() && severity->integer() == 1;
    }

    static bool HasExpectedError(const DiagnosticReports& reports, const ExpectedDiagnostic& expected)
    {
        const auto report = reports.find(expected.file);
        if (report == reports.end())
        {
            return false;
        }
        const auto& diagnostics = report->second.array();
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&](const auto& diagnostic)
                           {
                               const auto* code = diagnostic.object().find("code");
                               const bool sameCode = expected.code.empty() ||
                                                     (code && code->isString() && code->string() == expected.code);
                               return IsError(diagnostic) && sameCode && DiagnosticRange(diagnostic) == expected.range;
                           });
    }

    TEST_CASE("DifferentialCorpus - diagnostic expectations check file, code and both endpoints")
    {
        const auto diagnostic = lsp::json::parse(
            R"([{"severity":1,"code":"as-warn-undeclared-identifier","range":{"start":{"line":4,"character":22},)"
            R"("end":{"line":4,"character":33}}}])");
        DiagnosticReports reports{{"main.as", diagnostic}, {"peer.as", diagnostic}};
        const ExpectedDiagnostic primary{"main.as", "as-warn-undeclared-identifier", {4, 22, 4, 33}};
        const ExpectedDiagnostic sibling{"peer.as", primary.code, primary.range};
        REQUIRE(HasExpectedError(reports, primary));
        REQUIRE(HasExpectedError(reports, sibling));
        CHECK_FALSE(HasExpectedError(reports, {"main.as", "as-err-undefined-identifier", primary.range}));
        CHECK_FALSE(HasExpectedError(reports, {"main.as", primary.code, {4, 21, 4, 33}}));
        CHECK_FALSE(HasExpectedError(reports, {"main.as", primary.code, {4, 22, 4, 34}}));
        reports.at("main.as") = lsp::json::Array{};
        CHECK_FALSE(HasExpectedError(reports, primary));
        CHECK(HasExpectedError(reports, sibling));
    }

    /** @brief One corpus case: the main script plus any sibling files reached through #include. */
    struct CorpusCase
    {
        std::string id;
        std::string mainText;
        std::vector<std::pair<std::string, std::string>> siblings;
        std::string expect;
        std::vector<std::string> required;
        std::vector<std::string> forbidden;
        std::vector<ExpectedDiagnostic> diagnostics;
        /// Fixture served to the server as workspace stub, from `// @predefined: <fixture>`; empty when none.
        std::string stub;
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

    /** @brief Reads the `// @expect`, `@predefined`, `@complete` and `@error` tags of a case's main text. */
    static void ParseTags(CorpusCase & result)
    {
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
            else if (const std::string stub = take("// @predefined:"); !stub.empty())
            {
                result.stub = stub;
            }
            else if (const std::string plus = take("// @complete+"); !plus.empty())
            {
                result.required.push_back(plus);
            }
            else if (const std::string minus = take("// @complete-"); !minus.empty())
            {
                result.forbidden.push_back(minus);
            }
            else if (const std::string error = take("// @error:"); !error.empty())
            {
                ExpectedDiagnostic expectation;
                auto& range = expectation.range;
                std::istringstream fields(error);
                std::string coordinates, code, extra;
                REQUIRE(static_cast<bool>(fields >> coordinates));
                if (fields >> code)
                {
                    expectation.file = std::move(coordinates);
                    expectation.code = std::move(code);
                    REQUIRE(static_cast<bool>(fields >> coordinates));
                    REQUIRE_FALSE(static_cast<bool>(fields >> extra));
                }
                char trailing = '\0';
                REQUIRE(std::sscanf(coordinates.c_str(), "%d:%d-%d:%d%c", &range[0], &range[1], &range[2], &range[3],
                                    &trailing) == 4);
                REQUIRE(std::all_of(range.begin(), range.end(), [](int coordinate) { return coordinate >= 0; }));
                result.diagnostics.push_back(std::move(expectation));
            }
        }
    }

    /** @brief Marker line that starts a section of a multi-case file. */
    static constexpr std::string_view k_caseMarker = "// ==== case:";

    /**
     * @brief Loads every case an entry holds.
     * @param[in] entry A `.as` file (one case, or many split by `// ==== case: id ====`) or a directory with `main.as`.
     * @return One case per section; sections are independent scripts so their names never collide.
     */
    static std::vector<CorpusCase> LoadCases(const std::filesystem::path& entry)
    {
        const std::string category = entry.parent_path().filename().string();
        std::vector<CorpusCase> result;
        if (std::filesystem::is_directory(entry))
        {
            CorpusCase item;
            item.id = category + "/" + entry.filename().string();
            item.mainText = ReadFile(entry / "main.as");
            for (const auto& sibling : std::filesystem::directory_iterator(entry))
            {
                if (sibling.path().extension() == ".as" && sibling.path().filename() != "main.as")
                {
                    item.siblings.emplace_back(sibling.path().filename().string(), ReadFile(sibling.path()));
                }
            }
            ParseTags(item);
            result.push_back(std::move(item));
            return result;
        }
        std::istringstream lines(ReadFile(entry));
        std::string line;
        CorpusCase* current = nullptr;
        std::set<std::string> seen;
        while (std::getline(lines, line))
        {
            const std::string trimmed = Trim(line);
            if (trimmed.rfind(k_caseMarker, 0) == 0)
            {
                std::string name = Trim(trimmed.substr(k_caseMarker.size()));
                name = Trim(name.substr(0, name.rfind("====")));
                REQUIRE_MESSAGE(seen.insert(name).second, "duplicate case id " << name << " in " << entry.filename());
                current = &result.emplace_back();
                current->id = category + "/" + entry.stem().string() + "::" + name;
                continue;
            }
            if (current == nullptr)
            {
                if (Trim(line).empty())
                {
                    continue;
                }
                current = &result.emplace_back();
                current->id = category + "/" + entry.stem().string();
            }
            current->mainText += line + "\n";
        }
        for (CorpusCase& item : result)
        {
            ParseTags(item);
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
            for (CorpusCase& item : LoadCases(entry))
            {
                cases.push_back(std::move(item));
            }
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
            if (!item.stub.empty())
            {
                // The stub and its sibling rules file land in the workspace root, where the server discovers them.
                const std::filesystem::path fixtures(ANGELSCRIPT_FIXTURE_DIR);
                std::filesystem::copy_file(fixtures / (item.stub + ".as.predefined"), session.root / "as.predefined",
                                           std::filesystem::copy_options::overwrite_existing);
                const std::filesystem::path rules = fixtures / (item.stub + ".angelscript.rules.json");
                if (std::filesystem::exists(rules))
                {
                    std::filesystem::copy_file(rules, session.root / (item.stub + ".angelscript.rules.json"),
                                               std::filesystem::copy_options::overwrite_existing);
                }
            }
            for (const auto& [name, text] : files)
            {
                session.Call("open_document", angel_lsp::test::McpArgs(name, text));
            }
            // A real client shows every open file's diagnostics, so an error the compiler reports inside an
            // included file counts when the server reports it in that file.
            std::map<std::string, int> diagnosticCalls;
            for (const auto& file : files)
            {
                diagnosticCalls.emplace(file.first,
                                        session.Call("diagnostics", angel_lsp::test::McpPathArgs(file.first)));
            }
            lsp::json::Value completionArgs = angel_lsp::test::McpPositionArgs("main.as", line, character);
            completionArgs.object()["limit"] = 500;
            const int completion = session.Call("completion", std::move(completionArgs));
            const auto replies = session.Run();

            if (markerAt == std::string::npos)
            {
                bool hasErrors = false;
                DiagnosticReports reports;
                for (const auto& [file, call] : diagnosticCalls)
                {
                    const auto& fileReport =
                        reports.emplace(file, angel_lsp::test::McpPayload(replies.at(call))).first->second.array();
                    hasErrors |= std::any_of(fileReport.begin(), fileReport.end(), IsError);
                }
                CHECK(hasErrors == (item.expect != "accept"));
                for (const auto& expectation : item.diagnostics)
                {
                    REQUIRE_MESSAGE(reports.contains(expectation.file), "unknown diagnostic file " << expectation.file);
                    const bool found = HasExpectedError(reports, expectation);
                    const auto& want = expectation.range;
                    CHECK_MESSAGE(found, "no matching error in " << expectation.file << " (" << expectation.code
                                                                 << ") at " << want[0] << ":" << want[1] << "-"
                                                                 << want[2] << ":" << want[3]
                                                                 << "; the server reported "
                                                                 << lsp::json::stringify(reports.at(expectation.file)));
                }
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
