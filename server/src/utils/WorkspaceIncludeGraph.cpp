#include "utils/WorkspaceIncludeGraph.h"
#include "utils/Utils.h"
#include "utils/WorkspaceScan.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace angel_lsp::utils
{
namespace
{
std::string ReadFileFromDisk(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return "";

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/**
 * @brief Bundled parameters for include resolution.
 */
struct IncludeContext
{
    const std::vector<std::string>& searchDirectories;
    const std::vector<std::string>& allowedRoots;
    std::string_view implicitExtension;
};

/**
 * @brief Resolves every `#include` in one file to a normalized path, dropping directives
 *        that point nowhere.
 */
std::vector<std::string> ResolveDirectives(const std::string& normalizedPath, std::string_view sourceCode,
                                           const IncludeContext& ctx)
{
    std::vector<std::string> resolved;

    for (const auto& directive : IncludeResolver::ExtractIncludes(sourceCode))
    {
        std::string target = IncludeResolver::ResolveIncludePath(IncludeResolveRequest{
            .includePath = directive.rawPath,
            .currentFilePath = normalizedPath,
            .searchDirectories = ctx.searchDirectories,
            .allowedRoots = ctx.allowedRoots,
            .implicitExtension = ctx.implicitExtension,
        });
        if (target.empty())
            continue; // Unresolvable include - reported as a diagnostic elsewhere, not an edge.

        if (target == normalizedPath)
            continue; // A file including itself would be a self-loop with no meaning here.

        if (std::find(resolved.begin(), resolved.end(), target) == resolved.end())
            resolved.push_back(std::move(target));
    }

    return resolved;
}

struct FileDirectives
{
    std::string path;
    std::vector<std::string> targets;
};

struct DirectivesWorkerContext
{
    const std::vector<std::string>& scriptFiles;
    const IncludeContext& includeCtx;
    const WorkspaceIncludeGraph::FileReader& read;
    const std::function<bool()>& shouldStop;
};

void ProcessDirectivesRange(const DirectivesWorkerContext& dctx, size_t start, size_t end,
                            std::vector<FileDirectives>& results)
{
    for (size_t i = start; i < end; ++i)
    {
        if (dctx.shouldStop && dctx.shouldStop())
        {
            return;
        }
        const auto& path = dctx.scriptFiles[i];
        results[i] = FileDirectives{path, ResolveDirectives(path, dctx.read(path), dctx.includeCtx)};
    }
}

void CollectFileDirectives(const WorkspaceIncludeGraph::BuildFromFilesRequest& request,
                           const IncludeContext& includeCtx, const WorkspaceIncludeGraph::FileReader& read,
                           std::vector<FileDirectives>& results)
{
    const size_t totalFiles = request.scriptFiles.size();
    const unsigned int hwThreads = std::thread::hardware_concurrency();
    const unsigned int numThreads = (totalFiles >= 16 && hwThreads > 1) ? std::min(hwThreads, 8u) : 1u;

    const DirectivesWorkerContext dctx{request.scriptFiles, includeCtx, read, request.shouldStop};

    if (numThreads > 1)
    {
        std::vector<std::thread> workers;
        workers.reserve(numThreads);
        const size_t chunkSize = (totalFiles + numThreads - 1) / numThreads;

        for (unsigned int t = 0; t < numThreads; ++t)
        {
            const size_t start = t * chunkSize;
            const size_t end = std::min(start + chunkSize, totalFiles);
            if (start >= end)
            {
                break;
            }

            workers.emplace_back([&dctx, start, end, &results]()
                                 { ProcessDirectivesRange(dctx, start, end, results); });
        }

        for (auto& w : workers)
        {
            if (w.joinable())
            {
                w.join();
            }
        }
    }
    else
    {
        ProcessDirectivesRange(dctx, 0, totalFiles, results);
    }
}
} // namespace

void WorkspaceIncludeGraph::SetIncludesLocked(const std::string& normalizedPath, std::vector<std::string> includes)
{
    // Detach the previous forward edges from their reverse counterparts before overwriting, or
    // a removed #include would leave a dangling includedBy entry that keeps the two files in
    // the same module forever.
    if (const auto previous = m_includes.find(normalizedPath); previous != m_includes.end())
    {
        for (const auto& target : previous->second)
        {
            if (auto reverse = m_includedBy.find(target); reverse != m_includedBy.end())
            {
                auto& includers = reverse->second;
                includers.erase(std::remove(includers.begin(), includers.end(), normalizedPath), includers.end());
            }
        }
    }

    for (const auto& target : includes)
    {
        auto& includers = m_includedBy[target];
        if (std::find(includers.begin(), includers.end(), normalizedPath) == includers.end())
            includers.push_back(normalizedPath);
    }

    m_includes[normalizedPath] = std::move(includes);
}

void WorkspaceIncludeGraph::Build(const BuildRequest& request)
{
    // An edge may only point at a file inside the workspace or one of the configured search
    // directories. Those are exactly the two places a script is legitimately allowed to include
    // from, and confining the graph here is what stops a hostile `#include "/etc/passwd"` from
    // pulling an arbitrary file into the index in the first place.
    std::vector<std::string> allowedRoots = request.workspaceRoots;
    allowedRoots.insert(allowedRoots.end(), request.searchDirectories.begin(), request.searchDirectories.end());

    const FileReader read = request.fileReader ? request.fileReader : FileReader(ReadFileFromDisk);
    const IncludeContext includeCtx{request.searchDirectories, allowedRoots, request.implicitExtension};

    // Collect first, then swap under the lock, so a long filesystem walk never blocks the
    // message loop's reads against a half-built graph.
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> includes;
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> includedBy;

    const bool completed = ForEachWorkspaceFile(
        request.workspaceRoots, request.excludeGlobs, request.shouldStop,
        [&](const std::filesystem::directory_entry& entry)
        {
            // The walk produced this path, so its own spelling is already the filesystem's and
            // only the directory needs canonicalising - see IncludeResolver::NormalizeWalkedPath.
            // This one line was most of the server's startup time.
            const std::string path = IncludeResolver::NormalizeWalkedPath(entry.path());
            if (!request.scriptExtension.empty() && !std::string_view(path).ends_with(request.scriptExtension))
                return;

            std::vector<std::string> targets = ResolveDirectives(path, read(path), includeCtx);

            for (const auto& target : targets)
                includedBy[target].push_back(path);

            includes[path] = std::move(targets);
        });

    // A cancelled walk leaves the existing graph alone rather than swapping in whatever half
    // of it was reached. Publishing a partial graph would make every file the walk had not got
    // to yet look as though it included nothing.
    if (!completed)
        return;

    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_includes = std::move(includes);
    m_includedBy = std::move(includedBy);
}

void WorkspaceIncludeGraph::Build(const std::vector<std::string>& workspaceRoots,
                                  const std::vector<std::string>& searchDirectories, std::string_view scriptExtension)
{
    Build(BuildRequest{workspaceRoots, searchDirectories, std::string(scriptExtension), {}, {}, {}, {}});
}

void WorkspaceIncludeGraph::BuildFromFiles(const BuildFromFilesRequest& request)
{
    std::vector<std::string> allowedRoots = request.workspaceRoots;
    allowedRoots.insert(allowedRoots.end(), request.searchDirectories.begin(), request.searchDirectories.end());

    const FileReader read = request.fileReader ? request.fileReader : FileReader(ReadFileFromDisk);
    const IncludeContext includeCtx{request.searchDirectories, allowedRoots, request.implicitExtension};

    std::vector<FileDirectives> results(request.scriptFiles.size());
    CollectFileDirectives(request, includeCtx, read, results);

    if (request.shouldStop && request.shouldStop())
    {
        return;
    }

    ankerl::unordered_dense::map<std::string, std::vector<std::string>> includes;
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> includedBy;

    for (auto& entry : results)
    {
        if (entry.path.empty())
        {
            continue;
        }
        for (const auto& target : entry.targets)
        {
            includedBy[target].push_back(entry.path);
        }
        includes[std::move(entry.path)] = std::move(entry.targets);
    }

    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_includes = std::move(includes);
    m_includedBy = std::move(includedBy);
}

void WorkspaceIncludeGraph::BuildFromFiles(const std::vector<std::string>& scriptFiles,
                                           const std::vector<std::string>& searchDirectories,
                                           const std::vector<std::string>& workspaceRoots)
{
    BuildFromFiles(BuildFromFilesRequest{scriptFiles, searchDirectories, workspaceRoots, {}, {}, {}});
}

void WorkspaceIncludeGraph::UpdateFile(const UpdateFileRequest& request)
{
    const std::string normalized = IncludeResolver::NormalizePath(request.filePath);
    const IncludeContext includeCtx{request.searchDirectories, request.allowedRoots, request.implicitExtension};
    std::vector<std::string> targets = ResolveDirectives(normalized, request.sourceCode, includeCtx);

    std::unique_lock<std::shared_mutex> lock(m_mutex);
    SetIncludesLocked(normalized, std::move(targets));
}

void WorkspaceIncludeGraph::UpdateFile(const std::string& filePath, std::string_view sourceCode,
                                       const std::vector<std::string>& searchDirectories)
{
    UpdateFile(UpdateFileRequest{filePath, sourceCode, searchDirectories, {}, {}});
}

bool WorkspaceIncludeGraph::RemoveFile(const std::string& filePath)
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

std::vector<std::string> WorkspaceIncludeGraph::GetFilesIncluding(const std::string& filePath) const
{
    const std::string normalized = IncludeResolver::NormalizePath(filePath);

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto found = m_includedBy.find(normalized);
    return found == m_includedBy.end() ? std::vector<std::string>{} : found->second;
}

namespace
{
/**
 * @brief Collects all reachable reverse dependencies starting from direct includers.
 */
ankerl::unordered_dense::set<std::string>
CollectReachableDependents(const std::string& startNode,
                           const ankerl::unordered_dense::map<std::string, std::vector<std::string>>& includedBy)
{
    const auto direct = includedBy.find(startNode);
    if (direct == includedBy.end() || direct->second.empty())
        return {};

    ankerl::unordered_dense::set<std::string> reachable;
    std::vector<std::string> queue;
    for (const auto& inc : direct->second)
    {
        if (reachable.insert(inc).second)
            queue.push_back(inc);
    }
    for (size_t i = 0; i < queue.size(); ++i)
    {
        const auto it = includedBy.find(queue[i]);
        if (it != includedBy.end())
        {
            for (const auto& next : it->second)
            {
                if (reachable.insert(next).second)
                    queue.push_back(next);
            }
        }
    }
    return reachable;
}

ankerl::unordered_dense::set<std::string>
CollectReachableIncludes(const std::string& root,
                         const ankerl::unordered_dense::map<std::string, std::vector<std::string>>& includes)
{
    ankerl::unordered_dense::set<std::string> reachable;
    std::vector<std::string> queue;
    reachable.insert(root);
    queue.push_back(root);

    for (size_t i = 0; i < queue.size(); ++i)
    {
        const auto it = includes.find(queue[i]);
        if (it != includes.end())
        {
            for (const auto& next : it->second)
            {
                if (reachable.insert(next).second)
                    queue.push_back(next);
            }
        }
    }
    return reachable;
}

/**
 * @brief Performs topological sorting via Kahn's algorithm over a reachable dependency subgraph.
 */
std::vector<std::string>
SortReachableTopological(const ankerl::unordered_dense::set<std::string>& reachable,
                         const ankerl::unordered_dense::map<std::string, std::vector<std::string>>& includes,
                         const ankerl::unordered_dense::map<std::string, std::vector<std::string>>& includedBy)
{
    ankerl::unordered_dense::map<std::string, size_t> inDegree;
    for (const auto& node : reachable)
        inDegree[node] = 0;

    for (const auto& u : reachable)
    {
        const auto it = includes.find(u);
        if (it != includes.end())
        {
            for (const auto& v : it->second)
            {
                if (reachable.contains(v))
                    inDegree[u]++;
            }
        }
    }

    std::vector<std::string> readyQueue;
    for (const auto& node : reachable)
    {
        if (inDegree[node] == 0)
            readyQueue.push_back(node);
    }

    std::vector<std::string> result;
    result.reserve(reachable.size());
    for (size_t i = 0; i < readyQueue.size(); ++i)
    {
        const std::string curr = readyQueue[i];
        result.push_back(curr);

        const auto it = includedBy.find(curr);
        if (it != includedBy.end())
        {
            for (const auto& dependent : it->second)
            {
                if (reachable.contains(dependent) && --inDegree[dependent] == 0)
                    readyQueue.push_back(dependent);
            }
        }
    }

    for (const auto& node : reachable)
    {
        if (inDegree[node] > 0)
            result.push_back(node);
    }
    return result;
}
} // namespace

std::vector<std::string> WorkspaceIncludeGraph::GetForwardClosure(const std::string& entryFilePath) const
{
    const std::string normalized = IncludeResolver::NormalizePath(entryFilePath);
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto reachable = CollectReachableIncludes(normalized, m_includes);
    if (reachable.empty())
        return {};

    return SortReachableTopological(reachable, m_includes, m_includedBy);
}

std::vector<std::string> WorkspaceIncludeGraph::GetReverseDependenciesTopological(const std::string& filePath) const
{
    const std::string normalized = IncludeResolver::NormalizePath(filePath);
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto reachable = CollectReachableDependents(normalized, m_includedBy);
    if (reachable.empty())
        return {};

    return SortReachableTopological(reachable, m_includes, m_includedBy);
}

std::vector<std::string> WorkspaceIncludeGraph::GetModuleClosure(const std::string& filePath) const
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

        for (const auto& includer : includers->second)
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

    for (const auto& root : roots)
    {
        if (closure.insert(root).second)
            descendQueue.push_back(root);
    }

    for (size_t i = 0; i < descendQueue.size(); ++i)
    {
        const auto targets = m_includes.find(descendQueue[i]);
        if (targets == m_includes.end())
            continue;

        for (const auto& target : targets->second)
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

bool WorkspaceIncludeGraph::Contains(const std::string& filePath) const
{
    const std::string normalized = IncludeResolver::NormalizePath(filePath);

    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_includes.contains(normalized) || m_includedBy.contains(normalized);
}

std::vector<std::string> WorkspaceIncludeGraph::AllFiles() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    std::vector<std::string> files;
    files.reserve(m_includes.size());
    for (const auto& [path, _] : m_includes)
        files.push_back(path);

    return files;
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
} // namespace angel_lsp::utils
