#include "utils/IncludeResolver.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace angel_lsp::utils
{
namespace
{
/**
 * @brief Canonical form of a directory, remembered so a workspace walk pays for it once.
 *
 * `weakly_canonical` queries the filesystem for every component of the path it is handed.
 * The workspace walk hands it one path per file, and measured against a generated workspace
 * that is nearly the whole of startup:
 *
 *     files   include-graph phase   of a total scan of
 *     1       1 ms                  16 ms
 *     50      305 ms                316 ms
 *     200     1016 ms               1025 ms
 *
 * Which is where a report of "2942 ms to load a small project" comes from - it is linear in
 * the file count and about 5 ms a file. Replacing the call with a purely lexical normalise
 * took the 200-file case to 146 ms, so ~85% of it was this one function.
 *
 * The files in a directory all share that directory, so canonicalising the directory once
 * turns a per-file cost into a per-directory one, without giving up what the call is for:
 * `..` resolved against the real filesystem, symlinks followed, and on Windows the on-disk
 * case of every directory component.
 *
 * Process-wide and never pruned. It holds one entry per directory a session touches, which
 * is the shape of the workspace rather than of its history. A directory renamed underneath
 * a running server leaves a stale entry - the cost of the cache, and a bounded one: the
 * answer stays the path that directory had when it was first seen, which is also what every
 * document already indexed is keyed by.
 */
std::mutex& CanonicalDirectoryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, std::string>& CanonicalDirectoryCache()
{
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}

std::string CanonicalDirectory(const std::filesystem::path& directory)
{
    std::mutex& mutex = CanonicalDirectoryMutex();
    std::unordered_map<std::string, std::string>& cache = CanonicalDirectoryCache();

    std::string key = directory.string();

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto found = cache.find(key); found != cache.end())
        {
            return found->second;
        }
    }

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(directory, ec);
    if (ec)
    {
        canonical = directory.lexically_normal();
    }

    std::string value = canonical.string();

    std::lock_guard<std::mutex> lock(mutex);
    cache.emplace(std::move(key), value);
    return value;
}

/**
 * @brief Normalizes a filesystem path to use standard forward slashes and strips Windows long-path prefixes.
 * @param p The path to normalize.
 * @return Normalized path string with forward slashes.
 */
std::string TrimAndSlash(std::string s)
{
#if defined(_WIN32)
    if (s.rfind("\\\\?\\", 0) == 0)
    {
        s = s.substr(4);
    }
#endif
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string NormalizePathString(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(p, ec);
    if (ec)
    {
        canonicalPath = p.lexically_normal();
    }

    std::string s = canonicalPath.string();
#if defined(_WIN32)
    if (s.rfind("\\\\?\\", 0) == 0)
    {
        s = s.substr(4);
    }
#endif
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

bool IsSpaceOrTab(char c)
{
    return c == ' ' || c == '\t';
}

bool IsIncludeQuote(char c)
{
    return c == '"' || c == '\'' || c == '<';
}

struct IncludeScanState
{
    size_t index{0};
    size_t currentLine{0};
    bool atLineStart{true};
};

bool HandleIncludeNewline(std::string_view sourceCode, IncludeScanState& state)
{
    const char c = sourceCode[state.index];
    if (c == '\r')
    {
        if (state.index + 1 < sourceCode.size() && sourceCode[state.index + 1] == '\n')
        {
            ++state.index;
        }
        ++state.currentLine;
        state.atLineStart = true;
        ++state.index;
        return true;
    }
    if (c == '\n')
    {
        ++state.currentLine;
        state.atLineStart = true;
        ++state.index;
        return true;
    }
    return false;
}

void SkipLineComment(std::string_view sourceCode, IncludeScanState& state)
{
    state.index += 2;
    const size_t n = sourceCode.size();
    while (state.index < n && sourceCode[state.index] != '\n' && sourceCode[state.index] != '\r')
    {
        ++state.index;
    }
}

void SkipBlockComment(std::string_view sourceCode, IncludeScanState& state)
{
    state.index += 2;
    const size_t n = sourceCode.size();
    while (state.index < n)
    {
        if (sourceCode[state.index] == '\r')
        {
            if (state.index + 1 < n && sourceCode[state.index + 1] == '\n')
            {
                ++state.index;
            }
            ++state.currentLine;
            state.atLineStart = true;
        }
        else if (sourceCode[state.index] == '\n')
        {
            ++state.currentLine;
            state.atLineStart = true;
        }
        else if (sourceCode[state.index] == '*' && state.index + 1 < n && sourceCode[state.index + 1] == '/')
        {
            state.index += 2;
            break;
        }
        ++state.index;
    }
}

bool TrySkipComment(std::string_view sourceCode, IncludeScanState& state)
{
    if (sourceCode[state.index] != '/' || state.index + 1 >= sourceCode.size())
    {
        return false;
    }
    const char next = sourceCode[state.index + 1];
    if (next == '/')
    {
        SkipLineComment(sourceCode, state);
        return true;
    }
    if (next == '*')
    {
        SkipBlockComment(sourceCode, state);
        return true;
    }
    return false;
}

void SkipVerbatimString(std::string_view sourceCode, IncludeScanState& state)
{
    state.atLineStart = false;
    state.index += 2;
    const size_t n = sourceCode.size();
    while (state.index < n)
    {
        if (sourceCode[state.index] == '\r')
        {
            if (state.index + 1 < n && sourceCode[state.index + 1] == '\n')
            {
                ++state.index;
            }
            ++state.currentLine;
        }
        else if (sourceCode[state.index] == '\n')
        {
            ++state.currentLine;
        }
        else if (sourceCode[state.index] == '"')
        {
            if (state.index + 1 < n && sourceCode[state.index + 1] == '"')
            {
                state.index += 2;
                continue;
            }
            ++state.index;
            break;
        }
        ++state.index;
    }
}

void SkipMultilineString(std::string_view sourceCode, IncludeScanState& state)
{
    state.index += 3;
    const size_t n = sourceCode.size();
    while (state.index < n)
    {
        if (sourceCode[state.index] == '\r')
        {
            if (state.index + 1 < n && sourceCode[state.index + 1] == '\n')
            {
                ++state.index;
            }
            ++state.currentLine;
        }
        else if (sourceCode[state.index] == '\n')
        {
            ++state.currentLine;
        }
        else if (sourceCode[state.index] == '"' && state.index + 2 < n && sourceCode[state.index + 1] == '"' &&
                 sourceCode[state.index + 2] == '"')
        {
            state.index += 3;
            break;
        }
        ++state.index;
    }
}

void SkipEscapedString(std::string_view sourceCode, IncludeScanState& state, char quote)
{
    ++state.index;
    const size_t n = sourceCode.size();
    while (state.index < n)
    {
        if (sourceCode[state.index] == '\\')
        {
            state.index += 2;
            continue;
        }
        if (sourceCode[state.index] == quote)
        {
            ++state.index;
            break;
        }
        if (sourceCode[state.index] == '\n' || sourceCode[state.index] == '\r')
        {
            break;
        }
        ++state.index;
    }
}

bool TrySkipString(std::string_view sourceCode, IncludeScanState& state)
{
    const char c = sourceCode[state.index];
    const size_t n = sourceCode.size();
    if (c == '@' && state.index + 1 < n && sourceCode[state.index + 1] == '"')
    {
        SkipVerbatimString(sourceCode, state);
        return true;
    }
    if (c == '"')
    {
        state.atLineStart = false;
        if (state.index + 2 < n && sourceCode[state.index + 1] == '"' && sourceCode[state.index + 2] == '"')
        {
            SkipMultilineString(sourceCode, state);
        }
        else
        {
            SkipEscapedString(sourceCode, state, '"');
        }
        return true;
    }
    if (c == '\'')
    {
        state.atLineStart = false;
        SkipEscapedString(sourceCode, state, '\'');
        return true;
    }
    return false;
}

void SkipSpacesAndTabs(std::string_view sourceCode, IncludeScanState& state)
{
    const size_t n = sourceCode.size();
    while (state.index < n && IsSpaceOrTab(sourceCode[state.index]))
    {
        ++state.index;
    }
}

void SkipDirectiveRemainder(std::string_view sourceCode, IncludeScanState& state)
{
    const size_t n = sourceCode.size();
    while (state.index < n && sourceCode[state.index] != '\n' && sourceCode[state.index] != '\r')
    {
        ++state.index;
    }
}

std::optional<IncludeDirective> TryExtractDirective(std::string_view sourceCode, IncludeScanState& state)
{
    const size_t directiveLine = state.currentLine;
    const size_t n = sourceCode.size();
    ++state.index; // skip '#'

    SkipSpacesAndTabs(sourceCode, state);

    if (!sourceCode.substr(state.index).starts_with("include"))
    {
        SkipDirectiveRemainder(sourceCode, state);
        return std::nullopt;
    }

    const size_t afterInclude = state.index + 7;
    if (afterInclude >= n || !IsSpaceOrTab(sourceCode[afterInclude]))
    {
        SkipDirectiveRemainder(sourceCode, state);
        return std::nullopt;
    }

    state.index = afterInclude;
    SkipSpacesAndTabs(sourceCode, state);

    if (state.index >= n)
    {
        return std::nullopt;
    }

    const char quoteChar = sourceCode[state.index];
    if (!IsIncludeQuote(quoteChar))
    {
        SkipDirectiveRemainder(sourceCode, state);
        return std::nullopt;
    }

    const char closingChar = (quoteChar == '<') ? '>' : quoteChar;
    const bool isAngled = (quoteChar == '<');
    ++state.index;

    const size_t pathStart = state.index;
    while (state.index < n && sourceCode[state.index] != closingChar && sourceCode[state.index] != '\n' &&
           sourceCode[state.index] != '\r')
    {
        ++state.index;
    }

    std::optional<IncludeDirective> result;
    if (state.index < n && sourceCode[state.index] == closingChar)
    {
        result = IncludeDirective{
            .rawPath = std::string(sourceCode.substr(pathStart, state.index - pathStart)),
            .line = directiveLine,
            .resolvedPath = "",
            .isAngled = isAngled,
        };
        ++state.index;
    }

    SkipDirectiveRemainder(sourceCode, state);
    return result;
}

std::filesystem::path FirstExisting(const std::filesystem::path& directory, const std::filesystem::path& name,
                                    std::string_view implicitExtension)
{
    std::error_code inner;
    std::filesystem::path exact = directory / name;
    if (std::filesystem::exists(exact, inner) && !std::filesystem::is_directory(exact, inner))
    {
        return exact;
    }

    if (implicitExtension.empty())
    {
        return {};
    }

    std::filesystem::path extended = directory / (name.string() + std::string(implicitExtension));
    if (std::filesystem::exists(extended, inner) && !std::filesystem::is_directory(extended, inner))
    {
        return extended;
    }

    return {};
}

std::filesystem::path GetParentDirectory(std::string_view currentFilePath)
{
    std::error_code ec;
    std::filesystem::path currentPath(currentFilePath);
    if (std::filesystem::is_directory(currentPath, ec))
    {
        return currentPath;
    }
    if (currentPath.has_parent_path())
    {
        return currentPath.parent_path();
    }
    return std::filesystem::current_path(ec);
}

std::filesystem::path SearchInDirectories(const std::filesystem::path& inc,
                                          std::span<const std::string> searchDirectories,
                                          std::string_view implicitExtension)
{
    for (const auto& dir : searchDirectories)
    {
        if (dir.empty())
        {
            continue;
        }

        if (const std::filesystem::path candidate = FirstExisting(std::filesystem::path(dir), inc, implicitExtension);
            !candidate.empty())
        {
            return candidate;
        }
    }
    return {};
}
} // namespace

std::string IncludeResolver::NormalizePath(const std::filesystem::path& path)
{
    return NormalizePathString(path);
}

void IncludeResolver::ForgetCanonicalDirectories()
{
    std::lock_guard<std::mutex> lock(CanonicalDirectoryMutex());
    CanonicalDirectoryCache().clear();
}

std::string IncludeResolver::NormalizeWalkedPath(const std::filesystem::path& path)
{
    if (!path.has_parent_path() || !path.has_filename())
    {
        return NormalizePathString(path);
    }

    return TrimAndSlash((std::filesystem::path(CanonicalDirectory(path.parent_path())) / path.filename()).string());
}

bool IncludeResolver::IsWithinRoots(const std::string& normalizedPath, std::span<const std::string> allowedRoots)
{
    // No roots configured means no confinement. Unit tests and any library caller with no
    // workspace context rely on this; the server always supplies roots.
    if (allowedRoots.empty())
    {
        return true;
    }

    if (normalizedPath.empty())
    {
        return false;
    }

#if defined(_WIN32)
    // The same file has many spellings on Windows, differing only in case. Comparing them
    // byte-for-byte would let "C:/Work/../Windows/win.ini" back in under a different case.
    const auto equal = [](char a, char b)
    { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
#else
    const auto equal = [](char a, char b) { return a == b; };
#endif

    for (const auto& root : allowedRoots)
    {
        if (root.empty())
        {
            continue;
        }

        std::string normalizedRoot = NormalizePathString(std::filesystem::path(root));
        while (normalizedRoot.size() > 1 && normalizedRoot.back() == '/')
        {
            normalizedRoot.pop_back();
        }

        if (normalizedRoot.empty() || normalizedPath.size() < normalizedRoot.size())
        {
            continue;
        }

        if (!std::equal(normalizedRoot.begin(), normalizedRoot.end(), normalizedPath.begin(), equal))
        {
            continue;
        }

        // Component boundary: "/w/lib" must not be treated as containing "/w/library". Equal
        // length is the root itself, which counts as inside.
        if (normalizedPath.size() == normalizedRoot.size() || normalizedPath[normalizedRoot.size()] == '/')
        {
            return true;
        }
    }

    return false;
}

std::vector<IncludeDirective> IncludeResolver::ExtractIncludes(std::string_view sourceCode,
                                                               const std::vector<ExcludedLineRange>& excludedLines)
{
    std::vector<IncludeDirective> kept;
    for (auto& directive : ExtractIncludes(sourceCode))
    {
        if (!IsLineExcluded(excludedLines, static_cast<uint32_t>(directive.line)))
            kept.push_back(std::move(directive));
    }
    return kept;
}

std::vector<IncludeDirective> IncludeResolver::ExtractIncludes(std::string_view sourceCode)
{
    std::vector<IncludeDirective> directives;
    const size_t n = sourceCode.size();
    IncludeScanState state;

    while (state.index < n)
    {
        const char c = sourceCode[state.index];
        if (HandleIncludeNewline(sourceCode, state))
        {
            continue;
        }
        if (c == ' ' || c == '\t')
        {
            ++state.index;
            continue;
        }
        if (TrySkipComment(sourceCode, state))
        {
            continue;
        }
        if (TrySkipString(sourceCode, state))
        {
            continue;
        }
        if (c == '#' && state.atLineStart)
        {
            if (auto dir = TryExtractDirective(sourceCode, state))
            {
                directives.push_back(std::move(*dir));
            }
            continue;
        }
        state.atLineStart = false;
        ++state.index;
    }

    return directives;
}

std::string IncludeResolver::ResolveIncludePath(const IncludeResolveRequest& request)
{
    if (request.includePath.empty())
    {
        return "";
    }

    const auto permit = [&request](std::string resolved) -> std::string
    { return IsWithinRoots(resolved, request.allowedRoots) ? resolved : std::string(); };

    std::error_code ec;
    const std::filesystem::path inc(request.includePath);

    if (inc.is_absolute())
    {
        if (std::filesystem::exists(inc, ec) && !std::filesystem::is_directory(inc, ec))
        {
            return permit(NormalizePathString(inc));
        }
        return "";
    }

    if (!request.currentFilePath.empty())
    {
        const std::filesystem::path parentDir = GetParentDirectory(request.currentFilePath);
        if (const std::filesystem::path candidate = FirstExisting(parentDir, inc, request.implicitExtension);
            !candidate.empty())
        {
            return permit(NormalizePathString(candidate));
        }
    }

    if (const std::filesystem::path candidate =
            SearchInDirectories(inc, request.searchDirectories, request.implicitExtension);
        !candidate.empty())
    {
        return permit(NormalizePathString(candidate));
    }

    return "";
}

std::string IncludeResolver::ResolveIncludePath(std::string_view includePath, std::string_view currentFilePath,
                                                const std::vector<std::string>& searchDirectories,
                                                const std::vector<std::string>& allowedRoots)
{
    return ResolveIncludePath(IncludeResolveRequest{
        .includePath = includePath,
        .currentFilePath = currentFilePath,
        .searchDirectories = searchDirectories,
        .allowedRoots = allowedRoots,
        .implicitExtension = {},
    });
}

std::vector<std::string> IncludeResolver::ResolveAllIncludes(std::string_view rootFilePath,
                                                             const std::vector<std::string>& searchDirectories,
                                                             std::function<std::string(const std::string&)> fileReader,
                                                             const std::vector<std::string>& allowedRoots)
{
    std::vector<std::string> resolvedFiles;
    if (rootFilePath.empty())
    {
        return resolvedFiles;
    }

    if (!fileReader)
    {
        fileReader = [](const std::string& path) -> std::string
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                return "";
            }
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        };
    }

    std::filesystem::path rootP(rootFilePath);
    std::string rootNorm = NormalizePathString(rootP);

    std::unordered_set<std::string> visited;
    visited.insert(rootNorm);

    std::vector<std::string> queue;
    queue.push_back(rootNorm);

    size_t queueIndex = 0;
    while (queueIndex < queue.size())
    {
        std::string currentPath = queue[queueIndex++];
        std::string content = fileReader(currentPath);
        if (content.empty())
        {
            continue;
        }

        std::vector<IncludeDirective> includes = ExtractIncludes(content);
        for (const auto& inc : includes)
        {
            std::string resolved = ResolveIncludePath(inc.rawPath, currentPath, searchDirectories, allowedRoots);
            if (resolved.empty())
            {
                continue;
            }

            if (visited.find(resolved) == visited.end())
            {
                visited.insert(resolved);
                resolvedFiles.push_back(resolved);
                queue.push_back(resolved);
            }
        }
    }

    return resolvedFiles;
}

std::filesystem::path IncludeResolver::resolveInclude(const std::filesystem::path& workspaceRoot,
                                                      const std::filesystem::path& currentFilePath,
                                                      std::string_view includePath)
{
    if (includePath.empty())
    {
        return {};
    }

    const std::filesystem::path inc(includePath);
    const std::filesystem::path target = inc.is_absolute() ? inc : (currentFilePath.parent_path() / inc);

    std::error_code ec;
    const std::filesystem::path canonicalTarget = std::filesystem::weakly_canonical(target, ec);
    if (ec)
    {
        return {};
    }

    std::filesystem::path canonicalRoot = std::filesystem::canonical(workspaceRoot, ec);
    if (ec)
    {
        canonicalRoot = std::filesystem::weakly_canonical(workspaceRoot, ec);
        if (ec)
        {
            return {};
        }
    }

    auto [rootMismatch, targetMismatch] = std::mismatch(
        canonicalRoot.begin(), canonicalRoot.end(),
        canonicalTarget.begin(), canonicalTarget.end());

    if (rootMismatch != canonicalRoot.end())
    {
        return {};
    }

    if (!std::filesystem::is_regular_file(canonicalTarget, ec) || ec)
    {
        return {};
    }

    return canonicalTarget;
}
} // namespace angel_lsp::utils
