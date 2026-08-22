#include "utils/IncludeResolver.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace angel_lsp::utils
{
    namespace
    {
        /**
         * @brief Normalizes a filesystem path to use standard forward slashes and strips Windows long-path prefixes.
         * @param p The path to normalize.
         * @return Normalized path string with forward slashes.
         */
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
                            if (quoteChar == '"' || quoteChar == '<')
                            {
                                char closingChar = (quoteChar == '"') ? '"' : '>';
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
        const std::vector<std::string> &searchDirectories)
    {
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
                return NormalizePathString(inc);
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

            std::filesystem::path candidate = parentDir / inc;
            if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
            {
                return NormalizePathString(candidate);
            }
        }

        // 2. Search configured searchDirectories in order
        for (const auto &dir : searchDirectories)
        {
            if (dir.empty())
            {
                continue;
            }

            std::filesystem::path searchDir(dir);
            std::filesystem::path candidate = searchDir / inc;
            if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec))
            {
                return NormalizePathString(candidate);
            }
        }

        return "";
    }

    std::vector<std::string> IncludeResolver::ResolveAllIncludes(
        std::string_view rootFilePath,
        const std::vector<std::string> &searchDirectories,
        std::function<std::string(const std::string &)> fileReader)
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
                std::string resolved = ResolveIncludePath(inc.rawPath, currentPath, searchDirectories);
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
