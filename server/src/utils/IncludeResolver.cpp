#include "utils/IncludeResolver.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <mutex>
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
        std::mutex &CanonicalDirectoryMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::unordered_map<std::string, std::string> &CanonicalDirectoryCache()
        {
            static std::unordered_map<std::string, std::string> cache;
            return cache;
        }

        std::string CanonicalDirectory(const std::filesystem::path &directory)
        {
            std::mutex &mutex = CanonicalDirectoryMutex();
            std::unordered_map<std::string, std::string> &cache = CanonicalDirectoryCache();

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

        std::string NormalizePathString(const std::filesystem::path &p)
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
    }

    std::string IncludeResolver::NormalizePath(const std::filesystem::path &path)
    {
        return NormalizePathString(path);
    }

    void IncludeResolver::ForgetCanonicalDirectories()
    {
        std::lock_guard<std::mutex> lock(CanonicalDirectoryMutex());
        CanonicalDirectoryCache().clear();
    }

    std::string IncludeResolver::NormalizeWalkedPath(const std::filesystem::path &path)
    {
        if (!path.has_parent_path() || !path.has_filename())
        {
            return NormalizePathString(path);
        }

        return TrimAndSlash((std::filesystem::path(CanonicalDirectory(path.parent_path())) /
                             path.filename())
                                .string());
    }

    bool IncludeResolver::IsWithinRoots(const std::string &normalizedPath,
                                        const std::vector<std::string> &allowedRoots)
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
        {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        };
#else
        const auto equal = [](char a, char b) { return a == b; };
#endif

        for (const auto &root : allowedRoots)
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

    std::vector<IncludeDirective> IncludeResolver::ExtractIncludes(
        std::string_view sourceCode,
        const std::vector<ExcludedLineRange> &excludedLines)
    {
        std::vector<IncludeDirective> kept;
        for (auto &directive : ExtractIncludes(sourceCode))
        {
            if (!IsLineExcluded(excludedLines, static_cast<uint32_t>(directive.line)))
                kept.push_back(std::move(directive));
        }
        return kept;
    }

    std::vector<IncludeDirective> IncludeResolver::ExtractIncludes(std::string_view sourceCode)
    {
        std::vector<IncludeDirective> directives;
        size_t n = sourceCode.size();
        size_t i = 0;
        size_t currentLine = 0;
        bool atLineStart = true;

        while (i < n)
        {
            char c = sourceCode[i];

            // Handle newline characters
            if (c == '\r')
            {
                if (i + 1 < n && sourceCode[i + 1] == '\n')
                {
                    ++i;
                }
                ++currentLine;
                atLineStart = true;
                ++i;
                continue;
            }
            if (c == '\n')
            {
                ++currentLine;
                atLineStart = true;
                ++i;
                continue;
            }

            // Horizontal whitespace preserves line start status
            if (c == ' ' || c == '\t')
            {
                ++i;
                continue;
            }

            // Single-line or multi-line comment
            if (c == '/')
            {
                if (i + 1 < n && sourceCode[i + 1] == '/')
                {
                    // Single-line comment: skip until newline or EOF
                    i += 2;
                    while (i < n && sourceCode[i] != '\n' && sourceCode[i] != '\r')
                    {
                        ++i;
                    }
                    continue;
                }
                if (i + 1 < n && sourceCode[i + 1] == '*')
                {
                    // Multi-line block comment: skip until */ or EOF
                    i += 2;
                    while (i < n)
                    {
                        if (sourceCode[i] == '\r')
                        {
                            if (i + 1 < n && sourceCode[i + 1] == '\n')
                            {
                                ++i;
                            }
                            ++currentLine;
                            atLineStart = true;
                        }
                        else if (sourceCode[i] == '\n')
                        {
                            ++currentLine;
                            atLineStart = true;
                        }
                        else if (sourceCode[i] == '*' && i + 1 < n && sourceCode[i + 1] == '/')
                        {
                            i += 2;
                            break;
                        }
                        ++i;
                    }
                    continue;
                }
            }

            // Verbatim string literal @"..."
            if (c == '@' && i + 1 < n && sourceCode[i + 1] == '"')
            {
                atLineStart = false;
                i += 2;
                while (i < n)
                {
                    if (sourceCode[i] == '\r')
                    {
                        if (i + 1 < n && sourceCode[i + 1] == '\n')
                        {
                            ++i;
                        }
                        ++currentLine;
                    }
                    else if (sourceCode[i] == '\n')
                    {
                        ++currentLine;
                    }
                    else if (sourceCode[i] == '"')
                    {
                        if (i + 1 < n && sourceCode[i + 1] == '"')
                        {
                            i += 2;
                            continue;
                        }
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }

            // Multiline string """...""" or standard string literal "..."
            if (c == '"')
            {
                atLineStart = false;
                if (i + 2 < n && sourceCode[i + 1] == '"' && sourceCode[i + 2] == '"')
                {
                    // Multiline string """ ... """
                    i += 3;
                    while (i < n)
                    {
                        if (sourceCode[i] == '\r')
                        {
                            if (i + 1 < n && sourceCode[i + 1] == '\n')
                            {
                                ++i;
                            }
                            ++currentLine;
                        }
                        else if (sourceCode[i] == '\n')
                        {
                            ++currentLine;
                        }
                        else if (sourceCode[i] == '"' && i + 2 < n && sourceCode[i + 1] == '"' && sourceCode[i + 2] == '"')
                        {
                            i += 3;
                            break;
                        }
                        ++i;
                    }
                    continue;
                }
                else
                {
                    // Standard double-quoted string "..."
                    ++i;
                    while (i < n)
                    {
                        if (sourceCode[i] == '\\')
                        {
                            i += 2;
                            continue;
                        }
                        if (sourceCode[i] == '"')
                        {
                            ++i;
                            break;
                        }
                        if (sourceCode[i] == '\n' || sourceCode[i] == '\r')
                        {
                            break;
                        }
                        ++i;
                    }
                    continue;
                }
            }

            // Character literal '...'
            if (c == '\'')
            {
                atLineStart = false;
                ++i;
                while (i < n)
                {
                    if (sourceCode[i] == '\\')
                    {
                        i += 2;
                        continue;
                    }
                    if (sourceCode[i] == '\'')
                    {
                        ++i;
                        break;
                    }
                    if (sourceCode[i] == '\n' || sourceCode[i] == '\r')
                    {
                        break;
                    }
                    ++i;
                }
                continue;
            }

            // Preprocessor directive at line start
            if (c == '#' && atLineStart)
            {
                size_t directiveLine = currentLine;
                ++i; // skip '#'

                // Skip spaces/tabs between '#' and directive name
                while (i < n && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
                {
                    ++i;
                }

                // Check for "include" keyword
                std::string_view remaining = sourceCode.substr(i);
                if (remaining.starts_with("include"))
                {
                    size_t afterInclude = i + 7;
                    if (afterInclude < n && (sourceCode[afterInclude] == ' ' || sourceCode[afterInclude] == '\t'))
                    {
                        i = afterInclude;
                        // Skip whitespace after "include"
                        while (i < n && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
                        {
                            ++i;
                        }

                        if (i < n)
                        {
                            char quoteChar = sourceCode[i];

                            // Single quotes too. AngelScript's string literal is `'...'` as well as
                            // `"..."` while asEP_USE_CHARACTER_LITERALS is off, which is the default,
                            // and CScriptBuilder reads whichever the file used - measured:
                            // `#include 'helper.as'` compiles. Sven Co-op's scripts are written that
                            // way throughout, and until this line not one of their includes was
                            // extracted: the directive was skipped, the file never entered the
                            // module, and every type it declared came back unresolved.
                            if (quoteChar == '"' || quoteChar == '\'' || quoteChar == '<')
                            {
                                char closingChar = (quoteChar == '<') ? '>' : quoteChar;
                                bool isAngled = (quoteChar == '<');
                                ++i; // skip opening quote/bracket

                                size_t pathStart = i;
                                while (i < n && sourceCode[i] != closingChar && sourceCode[i] != '\n' && sourceCode[i] != '\r')
                                {
                                    ++i;
                                }

                                if (i < n && sourceCode[i] == closingChar)
                                {
                                    std::string rawPath(sourceCode.substr(pathStart, i - pathStart));
                                    directives.push_back(IncludeDirective{
                                        .rawPath = std::move(rawPath),
                                        .line = directiveLine,
                                        .resolvedPath = "",
                                        .isAngled = isAngled
                                    });
                                    ++i; // skip closing quote/bracket
                                }

                                // Skip rest of directive line
                                while (i < n && sourceCode[i] != '\n' && sourceCode[i] != '\r')
                                {
                                    ++i;
                                }
                                continue;
                            }
                        }
                    }
                }

                // Skip remainder of unrecognized preprocessor directive line
                while (i < n && sourceCode[i] != '\n' && sourceCode[i] != '\r')
                {
                    ++i;
                }
                continue;
            }

            // Any other non-whitespace token marks the line as no longer at start
            atLineStart = false;
            ++i;
        }

        return directives;
    }

    std::string IncludeResolver::ResolveIncludePath(
        std::string_view includePath,
        std::string_view currentFilePath,
        const std::vector<std::string> &searchDirectories,
        const std::vector<std::string> &allowedRoots,
        std::string_view implicitExtension)
    {
        // Every successful return below goes through this. Checking at the single exit rather than
        // per branch is deliberate: a new resolution strategy added later is confined by default
        // instead of silently bypassing the check.
        const auto permit = [&allowedRoots](std::string resolved) -> std::string
        {
            return IsWithinRoots(resolved, allowedRoots) ? resolved : std::string();
        };

        // One directory, tried the way the host would: the name exactly as written first, and only
        // if nothing is there, the same name with the configured extension.
        //
        // That order is the whole of the rule. A workspace holding both `helper` and `helper.as`
        // resolves to `helper`, which is what the compiler does; appending first would quietly pick
        // the other file and nothing would say so.
        const auto firstExisting = [&implicitExtension](const std::filesystem::path &directory,
                                                        const std::filesystem::path &name) -> std::filesystem::path
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

            // No "does it already end in .as" test on purpose. If it does, the exact try above
            // found it or the file is not there at all, and `helper.as.as` simply does not exist.
            std::filesystem::path extended = directory / (name.string() + std::string(implicitExtension));
            if (std::filesystem::exists(extended, inner) && !std::filesystem::is_directory(extended, inner))
            {
                return extended;
            }

            return {};
        };

        if (includePath.empty())
        {
            return "";
        }

        std::error_code ec;
        std::filesystem::path inc(includePath);

        // If includePath is already absolute
        if (inc.is_absolute())
        {
            if (std::filesystem::exists(inc, ec) && !std::filesystem::is_directory(inc, ec))
            {
                return permit(NormalizePathString(inc));
            }
            return "";
        }

        // 1. Resolve relative to current file's directory
        if (!currentFilePath.empty())
        {
            std::filesystem::path currentPath(currentFilePath);
            std::filesystem::path parentDir;

            if (std::filesystem::is_directory(currentPath, ec))
            {
                parentDir = currentPath;
            }
            else if (currentPath.has_parent_path())
            {
                parentDir = currentPath.parent_path();
            }
            else
            {
                parentDir = std::filesystem::current_path(ec);
            }

            if (const std::filesystem::path candidate = firstExisting(parentDir, inc); !candidate.empty())
            {
                return permit(NormalizePathString(candidate));
            }
        }

        // 2. Search configured searchDirectories in order
        for (const auto &dir : searchDirectories)
        {
            if (dir.empty())
            {
                continue;
            }

            if (const std::filesystem::path candidate = firstExisting(std::filesystem::path(dir), inc);
                !candidate.empty())
            {
                return permit(NormalizePathString(candidate));
            }
        }

        return "";
    }

    std::vector<std::string> IncludeResolver::ResolveAllIncludes(
        std::string_view rootFilePath,
        const std::vector<std::string> &searchDirectories,
        std::function<std::string(const std::string &)> fileReader,
        const std::vector<std::string> &allowedRoots)
    {
        std::vector<std::string> resolvedFiles;
        if (rootFilePath.empty())
        {
            return resolvedFiles;
        }

        if (!fileReader)
        {
            fileReader = [](const std::string &path) -> std::string
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
            for (const auto &inc : includes)
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
}
