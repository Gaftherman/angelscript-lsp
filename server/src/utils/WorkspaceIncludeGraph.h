#pragma once

#include "utils/IncludeResolver.h"

#include <ankerl/unordered_dense.h>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::utils
{
/**
 * @brief Workspace-wide `#include` graph, used to decide which files have to be indexed
 *        together with the one the user opened.
 */
class WorkspaceIncludeGraph
{
  public:
    /**
     * @brief Reads a file's contents. Defaults to std::ifstream when not supplied; tests inject
     *        an in-memory map instead of touching disk.
     */
    using FileReader = std::function<std::string(const std::string&)>;

    /**
     * @brief Parameters for building the workspace include graph from directories.
     */
    struct BuildRequest
    {
        std::vector<std::string> workspaceRoots;
        std::vector<std::string> searchDirectories;
        std::string scriptExtension;
        std::function<bool()> shouldStop = {};
        FileReader fileReader = {};
        std::vector<std::string> excludeGlobs = {};
        std::string implicitExtension = {};
        std::vector<std::string> allowedRoots = {};
    };

    /**
     * @brief Parameters for building the workspace include graph from a known file list.
     */
    struct BuildFromFilesRequest
    {
        std::vector<std::string> scriptFiles;
        std::vector<std::string> searchDirectories;
        std::vector<std::string> workspaceRoots;
        std::function<bool()> shouldStop = {};
        FileReader fileReader = {};
        std::string implicitExtension = {};
        std::vector<std::string> allowedRoots = {};
    };

    /**
     * @brief Parameters for updating a single file in the include graph.
     */
    struct UpdateFileRequest
    {
        std::string filePath;
        std::string_view sourceCode;
        std::vector<std::string> searchDirectories;
        std::vector<std::string> allowedRoots = {};
        std::string implicitExtension = {};
    };

    /**
     * @brief Scans the workspace and builds the graph from scratch, discarding any previous one.
     * @param request Bundled build parameters.
     */
    void Build(const BuildRequest& request);

    /**
     * @brief Scans the workspace with default options.
     * @param workspaceRoots Directories to walk recursively.
     * @param searchDirectories Extra include search paths.
     * @param scriptExtension Script file extension (e.g. ".as").
     */
    void Build(const std::vector<std::string>& workspaceRoots, const std::vector<std::string>& searchDirectories,
               std::string_view scriptExtension);

    /**
     * @brief Builds the graph from a pre-discovered list of script file paths.
     * @param request Bundled parameters.
     */
    void BuildFromFiles(const BuildFromFilesRequest& request);

    /**
     * @brief Builds the graph from a pre-discovered list of script file paths with default options.
     * @param scriptFiles Normalized paths of every script file to include in the graph.
     * @param searchDirectories Extra include search paths.
     * @param workspaceRoots Directories that define the allowed-roots boundary.
     */
    void BuildFromFiles(const std::vector<std::string>& scriptFiles, const std::vector<std::string>& searchDirectories,
                        const std::vector<std::string>& workspaceRoots);

    /**
     * @brief Re-reads one file's directives and patches just its edges.
     * @param request Bundled parameters.
     */
    void UpdateFile(const UpdateFileRequest& request);

    /**
     * @brief Re-reads one file's directives with default options.
     * @param filePath Any file in the module.
     * @param sourceCode Source code of the file.
     * @param searchDirectories Extra include search paths.
     */
    void UpdateFile(const std::string& filePath, std::string_view sourceCode,
                    const std::vector<std::string>& searchDirectories);

    /**
     * @brief All files that make up the module the given file participates in.
     *        Walks reverse edges up to the module's roots (files nothing includes), then forward edges
     * @param filePath Any file in the module; need not be normalized.
     * @return Normalized paths of every file in the module, including filePath itself.
     */
    std::vector<std::string> GetModuleClosure(const std::string& filePath) const;

    /**
     * @brief Computes strictly forward dependency closure originating from an entry point.
     *        Traverses forward include edges (`m_includes`) and sorts the reachable subgraph
     * @param[in] entryFilePath Entry-point script path; need not be normalized.
     * @return Deduplicated list of included files including entryFilePath in topological order.
     */
    std::vector<std::string> GetForwardClosure(const std::string& entryFilePath) const;

    /**
     * @brief The files whose `#include` lines name this one, directly.
     *        The graph has always held this edge - it is what GetModuleClosure ascends - but only ever
     */
    std::vector<std::string> GetFilesIncluding(const std::string& filePath) const;

    /**
     * @brief Collects all files transitively depending on filePath in topological order.
     *        Traverses reverse dependency edges (`m_includedBy`) and sorts the reachable subgraph
     * @param[in] filePath Path of the modified or invalidated dependency root.
     * @return Ordered, deduplicated list of dependent file paths in topological order.
     */
    std::vector<std::string> GetReverseDependenciesTopological(const std::string& filePath) const;

    /**
     * @brief Drops one file from the graph, detaching it from both edge directions.
     *        Called when a file is deleted on disk. Distinct from UpdateFile(path, "", ...), which
     * @param filePath Path of the removed file; need not be normalized.
     * @return True if the graph had an entry for it.
     */
    bool RemoveFile(const std::string& filePath);

    /**
     * @brief Whether the graph has an entry for the given file.
     */
    bool Contains(const std::string& filePath) const;

    /**
     * @brief Number of files currently in the graph.
     */
    size_t FileCount() const;

    /**
     * @brief Every file the graph knows, in no particular order.
     *        The set an `#include` may name: the walk that built the graph already visited exactly
     */
    std::vector<std::string> AllFiles() const;

    /**
     * @brief Drops every node and edge.
     */
    void Clear();

  private:
    /**
     * @brief Replaces one file's forward edges and the matching reverse edges. Caller holds the
     *        write lock.
     */
    void SetIncludesLocked(const std::string& normalizedPath, std::vector<std::string> includes);

    mutable std::shared_mutex m_mutex;

    /// Forward edges: file -> the files it includes.
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_includes;

    /// Reverse edges: file -> the files that include it.
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_includedBy;
};

/**
 * @brief Context holding active source path and search directories for include directive resolution.
 */
struct IncludeContext
{
    const std::vector<std::string>& searchDirectories;
    const std::vector<std::string>& allowedRoots;
    std::string_view implicitExtension;
};

/**
 * @brief Environment holding search roots and file system caches for workspace include graph building.
 */
struct ResolvedIncludeEnv
{
    std::vector<std::string> allowedRoots;
    WorkspaceIncludeGraph::FileReader fileReader;

    [[nodiscard]] IncludeContext MakeContext(const std::vector<std::string>& searchDirectories,
                                             std::string_view implicitExtension) const noexcept
    {
        return IncludeContext{searchDirectories, allowedRoots, implicitExtension};
    }
};

/**
 * @brief List of include directives and conditional compilation markers discovered within a source file.
 */
struct FileDirectives
{
    std::string path;
    std::vector<std::string> targets;
};

/**
 * @brief Parallel worker context for scanning preprocessor directives across workspace source files.
 */
struct DirectivesWorkerContext
{
    const std::vector<std::string>& scriptFiles;
    const IncludeContext& includeCtx;
    const WorkspaceIncludeGraph::FileReader& read;
    const std::function<bool()>& shouldStop;
};
} // namespace angel_lsp::utils
