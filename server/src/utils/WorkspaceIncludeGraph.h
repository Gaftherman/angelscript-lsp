#pragma once

#include "utils/IncludeResolver.h"

#include <ankerl/unordered_dense.h>

#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::utils
{
    /**
     * @brief Workspace-wide `#include` graph, used to decide which files have to be indexed
     *        together with the one the user opened.
     *
     * AngelScript `#include` behaves like textual module composition rather than C++ header
     * inclusion: every file pulled into a module shares one namespace, so a symbol declared in the
     * *including* file is visible in the *included* one. Indexing only the opened file, or only its
     * forward includes, therefore misses declarations the file legitimately depends on - opening
     * `weapon_ak47.as` has to bring in `base.as` even though it is `base.as` that includes it, not
     * the other way round.
     *
     * The graph stores both directions so a closure can be computed by walking up to the module's
     * roots and back down from them; see GetModuleClosure.
     *
     * Thread-safe: built on the workspace background thread while the message loop queries it,
     * guarded the same way SymbolTable and ScopeIndex are.
     */
    class WorkspaceIncludeGraph
    {
    public:
        /**
         * @brief Reads a file's contents. Defaults to std::ifstream when not supplied; tests inject
         *        an in-memory map instead of touching disk.
         */
        using FileReader = std::function<std::string(const std::string &)>;

        /**
         * @brief Scans the workspace and builds the graph from scratch, discarding any previous one.
         *
         * Only `#include` directives are parsed (via IncludeResolver::ExtractIncludes), never the
         * AST, which is what makes a full-workspace scan cheap enough to run at startup.
         *
         * @param workspaceRoots Directories to walk recursively.
         * @param searchDirectories Extra include search paths, in priority order.
         * @param scriptExtension Only files ending with this are considered (e.g. ".as").
         * @param shouldStop Polled between files so a shutdown can interrupt a long scan; may be empty.
         * @param fileReader Optional content source; reads from disk when not supplied.
         */
        void Build(const std::vector<std::string> &workspaceRoots,
                   const std::vector<std::string> &searchDirectories,
                   std::string_view scriptExtension,
                   const std::function<bool()> &shouldStop = {},
                   const FileReader &fileReader = {});

        /**
         * @brief Re-reads one file's directives and patches just its edges.
         *
         * Called on save: an edited `#include` line changes which module a file belongs to, and a
         * full rebuild for one keystroke-sized change would be wasteful.
         */
        void UpdateFile(const std::string &filePath,
                        std::string_view sourceCode,
                        const std::vector<std::string> &searchDirectories);

        /**
         * @brief All files that make up the module the given file participates in.
         *
         * Walks reverse edges up to the module's roots (files nothing includes), then forward edges
         * back down from every root found. A file that neither includes nor is included by anything
         * is its own module and comes back alone. Cycles and diamonds are tolerated: both walks keep
         * a visited set, and a cycle with no reachable root falls back to treating every file
         * reached on the way up as a root.
         *
         * @param filePath Any file in the module; need not be normalized.
         * @return Normalized paths of every file in the module, including filePath itself.
         */
        std::vector<std::string> GetModuleClosure(const std::string &filePath) const;

        /**
         * @brief Drops one file from the graph, detaching it from both edge directions.
         *
         * Called when a file is deleted on disk. Distinct from UpdateFile(path, "", ...), which
         * only clears what the file includes and would leave it listed as a target of everything
         * that still includes it - so the deleted file would keep dragging its former module
         * together long after it stopped existing.
         *
         * @param filePath Path of the removed file; need not be normalized.
         * @return True if the graph had an entry for it.
         */
        bool RemoveFile(const std::string &filePath);

        /**
         * @brief Whether the graph has an entry for the given file.
         */
        bool Contains(const std::string &filePath) const;

        /**
         * @brief Number of files currently in the graph.
         */
        size_t FileCount() const;

        /**
         * @brief Drops every node and edge.
         */
        void Clear();

    private:
        /**
         * @brief Replaces one file's forward edges and the matching reverse edges. Caller holds the
         *        write lock.
         */
        void SetIncludesLocked(const std::string &normalizedPath, std::vector<std::string> includes);

        mutable std::shared_mutex m_mutex;

        /// Forward edges: file -> the files it includes.
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_includes;

        /// Reverse edges: file -> the files that include it.
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> m_includedBy;
    };
}
