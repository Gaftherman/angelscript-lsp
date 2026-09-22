#include <doctest/doctest.h>

#include "config/ServerConfig.h"
#include "helpers/ScriptedStream.h"
#include "helpers/TestUtils.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace
{
using namespace angel_lsp;
using namespace angel_lsp::utils;

constexpr size_t k_maxLspPayloadSize = 16 * 1024 * 1024; // 16 MB

struct SandboxGuard
{
    std::filesystem::path root;

    SandboxGuard()
    {
        const std::string name = test::GenerateRandomSymbolName("sec_sandbox");
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(root);
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(root, ec);
        if (!ec)
            root = std::move(canon);
    }

    ~SandboxGuard()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::string WriteFile(const std::string& relPath, const std::string& content) const
    {
        const auto full = root / relPath;
        if (full.has_parent_path())
            std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::binary);
        out << content;
        return IncludeResolver::NormalizePath(full);
    }
};

/**
 * @brief Generates a randomized path traversal attack vector string.
 */
std::string GenerateFuzzedPath(std::mt19937_64& rng, const std::string& validSubdir, const std::string& validFile)
{
    std::uniform_int_distribution<int> depthDist(1, 25);
    std::uniform_int_distribution<int> sepChoice(0, 3);
    std::uniform_int_distribution<int> modeChoice(0, 5);

    const int depth = depthDist(rng);
    std::string sep = "/";
    switch (sepChoice(rng))
    {
    case 1:
        sep = "\\";
        break;
    case 2:
        sep = "/\\//";
        break;
    case 3:
        sep = "//";
        break;
    default:
        break;
    }

    std::string attack;
    switch (modeChoice(rng))
    {
    case 0: // Classic dot-dot
        for (int i = 0; i < depth; ++i)
            attack += ".." + sep;
        attack += "Windows" + sep + "System32" + sep + "cmd.exe";
        break;
    case 1: // Interleaved subfolder
        attack = validSubdir + sep;
        for (int i = 0; i < depth; ++i)
            attack += ".." + sep;
        attack += "etc" + sep + "passwd";
        break;
    case 2: // Device namespace prefix
        attack = "\\\\?\\" + validSubdir + sep;
        for (int i = 0; i < depth; ++i)
            attack += ".." + sep;
        attack += validFile;
        break;
    case 3: // Non-canonical dot segments
        attack = "." + sep + "." + sep + ".." + sep;
        for (int i = 0; i < depth; ++i)
            attack += ".." + sep;
        attack += validFile;
        break;
    default: // Root escape
        attack = "/" + validFile;
        break;
    }
    return attack;
}

/**
 * @brief Fuzzes transport headers with malformed and boundary Content-Length fields.
 */
void FuzzHeaderPayload(std::mt19937_64& rng, test::ScriptedStream& stream)
{
    std::uniform_int_distribution<int> fuzzKind(0, 4);
    std::uniform_int_distribution<uint64_t> hugeLen(k_maxLspPayloadSize + 1, UINT64_MAX);

    switch (fuzzKind(rng))
    {
    case 0: // Giant length
        stream.PushRaw("Content-Length: " + std::to_string(hugeLen(rng)) + "\r\n\r\n");
        break;
    case 1: // Negative length
        stream.PushRaw("Content-Length: -" + std::to_string(hugeLen(rng) % 1000000) + "\r\n\r\n");
        break;
    case 2: // Non-numeric noise
        stream.PushRaw("Content-Length: BAD_LEN_" + test::GenerateRandomSymbolName() + "\r\n\r\n");
        break;
    case 3: // Zero-padded boundary
        stream.PushRaw("Content-Length: 00000000000000000000\r\n\r\n");
        break;
    default: // Malformed header delimiter
        stream.PushRaw("Content-Length: 42\n\n");
        break;
    }
}
} // namespace

TEST_CASE("Security - SEC-02 Randomized Path Traversal Fuzzing Invariant")
{
    SandboxGuard sandbox;
    const std::string subDir = test::GenerateRandomSymbolName("sub");
    const std::string scriptName = test::GenerateRandomSymbolName("script") + ".as";
    const std::string entryFile = sandbox.WriteFile(subDir + "/" + scriptName, "void main() {}\n");

    const std::string rootNorm = IncludeResolver::NormalizePath(sandbox.root);
    const std::vector<std::string> allowedRoots = {rootNorm};

    std::mt19937_64 rng(std::random_device{}());

    for (int iter = 0; iter < 120; ++iter)
    {
        const std::string fuzzed = GenerateFuzzedPath(rng, subDir, scriptName);
        const std::string resolved = IncludeResolver::ResolveIncludePath(fuzzed, entryFile, {}, allowedRoots);

        if (!resolved.empty())
        {
            CHECK(IncludeResolver::IsWithinRoots(resolved, allowedRoots));
            std::error_code ec;
            CHECK(std::filesystem::exists(resolved, ec));
        }
    }
}

TEST_CASE("Security - SEC-03 JSON-RPC Transport Header Fuzzing Invariant")
{
    std::mt19937_64 rng(std::random_device{}());
    config::ServerConfig config;

    for (int iter = 0; iter < 100; ++iter)
    {
        test::ScriptedStream stream;
        FuzzHeaderPayload(rng, stream);

        // Server must not throw uncaught exceptions (e.g. bad_alloc, out_of_range)
        CHECK_NOTHROW({
            Server server(config, stream);
            server.Run();
        });
    }
}

TEST_CASE("Security - SEC-01 Canonical Workspace Containment Invariant")
{
    SandboxGuard sandbox;
    const std::string validDir = test::GenerateRandomSymbolName("vdir");
    const std::string validFile = test::GenerateRandomSymbolName("vscript") + ".as";
    const std::string fullPath = sandbox.WriteFile(validDir + "/" + validFile, "void run() {}\n");

    std::mt19937_64 rng(std::random_device{}());

    // Valid file within workspace
    auto resValid = IncludeResolver::resolveInclude(sandbox.root, fullPath, validFile);
    CHECK(!resValid.empty());
    CHECK(utils::IsWithinDirectory(sandbox.root, resValid));

    for (int iter = 0; iter < 100; ++iter)
    {
        const std::string attack = GenerateFuzzedPath(rng, validDir, validFile);
        auto resolved = IncludeResolver::resolveInclude(sandbox.root, fullPath, attack);
        if (!resolved.empty())
        {
            CHECK(utils::IsWithinDirectory(sandbox.root, resolved));
            std::error_code ec;
            CHECK(std::filesystem::exists(resolved, ec));
            CHECK(std::filesystem::is_regular_file(resolved, ec));
        }
    }
}

TEST_CASE("Security - SEC-01 Single-File Sandbox Mode Confinement")
{
    struct IsolatedModeGuard
    {
        bool prev;
        explicit IsolatedModeGuard(bool mode) : prev(IncludeResolver::IsIsolatedHarnessMode())
        {
            IncludeResolver::SetIsolatedHarnessMode(mode);
        }
        ~IsolatedModeGuard()
        {
            IncludeResolver::SetIsolatedHarnessMode(prev);
        }
    } modeGuard(false);

    SandboxGuard sandbox;
    const std::string docDir = test::GenerateRandomSymbolName("doc_dir");
    const std::string outsideDir = test::GenerateRandomSymbolName("outside_dir");
    const std::string docName = test::GenerateRandomSymbolName("main") + ".as";
    const std::string siblingName = test::GenerateRandomSymbolName("sibling") + ".as";
    const std::string outsideName = test::GenerateRandomSymbolName("outside") + ".as";

    const std::string docPath = sandbox.WriteFile(docDir + "/" + docName, "void main() {}\n");
    const std::string siblingPath = sandbox.WriteFile(docDir + "/" + siblingName, "void sibling() {}\n");
    sandbox.WriteFile(outsideDir + "/" + outsideName, "void secret() {}\n");

    // In single-file mode without workspace roots, allowedRoots is empty
    const std::vector<std::string> emptyAllowedRoots;

    // Sibling in the same directory must resolve
    const std::string resolvedSibling =
        IncludeResolver::ResolveIncludePath(siblingName, docPath, {}, emptyAllowedRoots);
    CHECK_FALSE(resolvedSibling.empty());
    CHECK(IncludeResolver::NormalizePath(resolvedSibling) == IncludeResolver::NormalizePath(siblingPath));

    std::mt19937_64 rng(std::random_device{}());
    for (int iter = 0; iter < 50; ++iter)
    {
        const std::string attack = "../" + outsideDir + "/" + outsideName;
        const std::string resolvedOutside = IncludeResolver::ResolveIncludePath(attack, docPath, {}, emptyAllowedRoots);
        CHECK(resolvedOutside.empty());

        const std::string sysAttack = GenerateFuzzedPath(rng, docDir, docName);
        const std::string resolvedSys = IncludeResolver::ResolveIncludePath(sysAttack, docPath, {}, emptyAllowedRoots);
        if (!resolvedSys.empty())
        {
            // If resolved, must be within the parent directory of docPath
            const auto parentDir = std::filesystem::path(docPath).parent_path().string();
            const std::vector<std::string> parentRoots = {parentDir};
            CHECK(IncludeResolver::IsWithinRoots(resolvedSys, parentRoots));
        }
    }
}

TEST_CASE("Security - SEC-02 Windows UNC / NTLM Hash Leak Invariant")
{
    SandboxGuard sandbox;
    const std::string docName = test::GenerateRandomSymbolName("entry") + ".as";
    const std::string docPath = sandbox.WriteFile(docName, "void start() {}\n");

    const std::vector<std::string> allowedRoots = {sandbox.root.string()};

    // Randomized hostnames and shares
    for (int iter = 0; iter < 50; ++iter)
    {
        const std::string host = test::GenerateRandomSymbolName("host");
        const std::string share = test::GenerateRandomSymbolName("share");
        const std::string file = test::GenerateRandomSymbolName("payload") + ".as";

        // Backslash UNC
        const std::string uncBackslash = "\\\\" + host + "\\" + share + "\\" + file;
        CHECK(IncludeResolver::ResolveIncludePath(uncBackslash, docPath, {}, allowedRoots).empty());

        // Forward slash UNC
        const std::string uncForwardSlash = "//" + host + "/" + share + "/" + file;
        CHECK(IncludeResolver::ResolveIncludePath(uncForwardSlash, docPath, {}, allowedRoots).empty());
    }
}

TEST_CASE("Security - ROBUST-01 JSON-RPC Header Line Buffer Limit")
{
    config::ServerConfig config;
    test::ScriptedStream stream;

    // Header line exceeding 8 KB limit
    const std::string hugeHeaderKey = "X-Fuzzed-Header-" + test::GenerateRandomSymbolName();
    const std::string hugeHeaderValue(9000, 'A');
    stream.PushRaw(hugeHeaderKey + ": " + hugeHeaderValue + "\r\n\r\n");

    CHECK_NOTHROW({
        Server server(config, stream);
        server.Run();
    });
}
