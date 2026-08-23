#include "utils/WorkspaceIncludeGraph.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace angel_lsp::utils
{
    namespace
    {
        std::string ReadFileFromDisk(const std::string &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
                return "";

            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        /**
         * @brief Resolves every `#include` in one file to a normalized path, dropping directives
         *        that point nowhere.
         */
        std::vector<std::string> ResolveDirectives(const std::string &normalizedPath,
                                                   std::string_view sourceCode,
                                                   const std::vector<std::string> &searchDirectories)
        {
            std::vector<std::string> resolved;

            for (const auto &directive : IncludeResolver::ExtractIncludes(sourceCode))
            {
                std::string target = IncludeResolver::ResolveIncludePath(directive.rawPath, normalizedPath, searchDirectories);
                if (target.empty())
                    continue; // Unresolvable include - reported as a diagnostic elsewhere, not an edge.

                if (target == normalizedPath)
                    continue; // A file including itself would be a self-loop with no meaning here.

                if (std::find(resolved.begin(), resolved.end(), target) == resolved.end())
                    resolved.push_back(std::move(target));
            }

            return resolved;
        }
    }

    void WorkspaceIncludeGraph::SetIncludesLocked(const std::string &normalizedPath, std::vector<std::string> includes)
    {
        // Detach the previous forward edges from their reverse counterparts before overwriting, or
        // a removed #include would leave a dangling includedBy entry that keeps the two files in
        // the same module forever.
        if (const auto previous = m_includes.find(normalizedPath); previous != m_includes.end())
        {
            for (const auto &target : previous->second)
            {
                if (auto reverse = m_includedBy.find(target); reverse != m_includedBy.end())
                {
                    auto &includers = reverse->second;
                    includers.erase(std::remove(includers.begin(), includers.end(), normalizedPath), includers.end());
                }
            }
        }

        for (const auto &target : includes)
        {
            auto &includers = m_includedBy[target];
            if (std::find(includers.begin(), includers.end(), normalizedPath) == includers.end())
                includers.push_back(normalizedPath);
        }

        m_includes[normalizedPath] = std::move(includes);
    }

    void WorkspaceIncludeGraph::Build(const std::vector<std::string> &workspaceRoots,
                                      const std::vector<std::string> &searchDirectories,
                                      std::string_view scriptExtension,
                                      const std::function<bool()> &shouldStop,
                                      const FileReader &fileReader)
    {
        const FileReader read = fileReader ? fileReader : FileReader(ReadFileFromDisk);

        // Collect first, then swap under the lock, so a long filesystem walk never blocks the
        // message loop's reads against a half-built graph.
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> includes;
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> includedBy;

        for (const auto &root : workspaceRoots)
        {
            if (shouldStop && shouldStop())
                return;

            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
            if (ec)
                continue;

            for (const auto &entry : it)
            {
                if (shouldStop && shouldStop())
                    return;

                if (!entry.is_regular_file(ec) || ec)
                    continue;

                const std::string path = IncludeResolver::NormalizePath(entry.path());
                if (!scriptExtension.empty() && !std::string_view(path).ends_with(scriptExtension))
                    continue;

                std::vector<std::string> targets = ResolveDirectives(path, read(path), searchDirectories);

                for (const auto &target : targets)
                    includedBy[target].push_back(path);

                includes[path] = std::move(targets);
            }
        }

        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_includes = std::move(includes);
        m_includedBy = std::move(includedBy);
    }

    void WorkspaceIncludeGraph::UpdateFile(const std::string &filePath,
                                           std::string_view sourceCode,
                                           const std::vector<std::string> &searchDirectories)
    {
        const std::string normalized = IncludeResolver::NormalizePath(filePath);
        std::vector<std::string> targets = ResolveDirectives(normalized, sourceCode, searchDirectories);

        std::unique_lock<std::shared_mutex> lock(m_mutex);
        SetIncludesLocked(normalized, std::move(targets));
    }

    bool WorkspaceIncludeGraph::RemoveFile(const std::string &filePath)
    {
        const std::string normalized = IncludeResolver::NormalizePath(filePath);

        std::unique_lock<std::shared_mutex> lock(m_mutex);

        // Tested before clearing, because SetIncludesLocked inserts the node it is given - so
        // asking afterwards would report every unknown path as having been removed.
        const bool existed = m_includes.contains(normalized) || m_includedBy.contains(normalized);
        if (!existed)
        {
            return false;
        }

        // Clearing the forward edges first reuses the reverse-edge detaching SetIncludesLocked
        // already does correctly, so only the node's own two entries are left to erase.
        SetIncludesLocked(normalized, {});
        m_includes.erase(normalized);
        m_includedBy.erase(normalized);

        // Deliberately left alone: the forward edges of the files that still include this one.
        // Their directives really do still name a file that is gone, and reporting that is
        // AppendIncludeDiagnostics' job - rewriting their edges here would hide it instead.
        return true;
    }

    std::vector<std::string> WorkspaceIncludeGraph::GetModuleClosure(const std::string &filePath) const
    {
        const std::string normalized = IncludeResolver::NormalizePath(filePath);

        std::shared_lock<std::shared_mutex> lock(m_mutex);

        // Pass 1 - ascend. Everything that (transitively) includes this file belongs to its module,
        // and the ones nothing includes are the module's entry points.
        ankerl::unordered_dense::set<std::string> ascended;
        std::vector<std::string> roots;
        std::vector<std::string> queue{normalized};
        ascended.insert(normalized);

        for (size_t i = 0; i < queue.size(); ++i)
        {
            const std::string current = queue[i];
            const auto includers = m_includedBy.find(current);

            if (includers == m_includedBy.end() || includers->second.empty())
            {
                roots.push_back(current);
                continue;
            }

            for (const auto &includer : includers->second)
            {
                if (ascended.insert(includer).second)
                    queue.push_back(includer);
            }
        }

        // A cycle can leave every node with an includer and therefore no root at all. Treating the
        // whole ascended set as roots still yields the right closure - it is only the starting
        // points for the descent that are ambiguous, not the membership.
        if (roots.empty())
            roots.assign(ascended.begin(), ascended.end());

        // Pass 2 - descend. From each root, everything reachable through forward edges is part of
        // the same module.
        ankerl::unordered_dense::set<std::string> closure;
        std::vector<std::string> descendQueue;

        for (const auto &root : roots)
        {
            if (closure.insert(root).second)
                descendQueue.push_back(root);
        }

        for (size_t i = 0; i < descendQueue.size(); ++i)
        {
            const auto targets = m_includes.find(descendQueue[i]);
            if (targets == m_includes.end())
                continue;

            for (const auto &target : targets->second)
            {
                if (closure.insert(target).second)
                    descendQueue.push_back(target);
            }
        }

        // The file itself is in the closure by construction whenever the graph knows it; add it
        // explicitly so an unknown file (never scanned, just opened) still gets indexed alone
        // rather than coming back empty.
        closure.insert(normalized);

        return std::vector<std::string>(closure.begin(), closure.end());
    }

    bool WorkspaceIncludeGraph::Contains(const std::string &filePath) const
    {
        const std::string normalized = IncludeResolver::NormalizePath(filePath);

        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_includes.contains(normalized) || m_includedBy.contains(normalized);
    }

    size_t WorkspaceIncludeGraph::FileCount() const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_includes.size();
    }

    void WorkspaceIncludeGraph::Clear()
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_includes.clear();
        m_includedBy.clear();
    }
}
