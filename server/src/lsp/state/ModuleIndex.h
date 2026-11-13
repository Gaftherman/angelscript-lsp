#pragma once

#include "utils/TransparentHash.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp
{
/**
 * @brief One configured module, resolved against the include graph.
 */
struct ModuleView
{
    std::string name;
    std::string entryPath;
    std::string folderPath;
    TransparentStringSet memberPaths;
    TransparentStringSet closurePaths;
    bool isImplicitEntryPoint = false;
};

/**
 * @brief Which module owns a file, and which others also claimed it.
 */
struct ModuleClaim
{
    const ModuleView* owner = nullptr;
    std::vector<std::string> alsoClaimedBy;
};

/**
 * @brief Candidate module file path and workspace configuration state during workspace module indexing.
 */
struct ModuleCandidate
{
    const ModuleView* view = nullptr;
    int rank = 0;
    size_t depth = 0;
};

/**
 * @brief Thread-safe manager for workspace include graphs, module closures, and cross-file indexing.
 */
class ModuleIndex
{
  public:
    /**
     * @brief Constructs an empty ModuleIndex.
     */
    ModuleIndex() = default;

    /**
     * @brief Destructor.
     */
    ~ModuleIndex() = default;

    ModuleIndex(const ModuleIndex&) = delete;
    ModuleIndex& operator=(const ModuleIndex&) = delete;
    ModuleIndex(ModuleIndex&&) = delete;
    ModuleIndex& operator=(ModuleIndex&&) = delete;

    /**
     * @brief Parameters for updating a file in the module index.
     */
    struct UpdateFileRequest
    {
        std::string filePath;
        std::string_view content;
        std::vector<std::string> searchDirectories;
        std::vector<std::string> allowedRoots = {};
        std::string implicitExtension = {};
    };

    /**
     * @brief Updates include dependencies for a saved or scanned file.
     * @param request Bundled update parameters.
     */
    void UpdateFile(const UpdateFileRequest& request);

    /**
     * @brief Updates include dependencies for a saved or scanned file with default options.
     * @param filePath Path of the file.
     * @param content File contents.
     * @param searchDirectories Search directories for includes.
     */
    void UpdateFile(std::string_view filePath, std::string_view content,
                    std::span<const std::string> searchDirectories);

    /**
     * @brief Removes a file from the include graph.
     */
    void RemoveFile(std::string_view filePath);

    /**
     * @brief Computes transitive module closure of file paths for a root file.
     */
    [[nodiscard]] std::vector<std::string> GetModuleClosure(std::string_view filePath) const;

    /**
     * @brief Associates an open document with its required closure URIs.
     */
    void AssociateClosure(std::string_view openUri, std::vector<std::string> closureUris);

    /**
     * @brief Releases an open document's reference to its closure URIs.
     * @param openUri Canonical URI of open document closing.
     * @return Closure URIs that are no longer referenced by any open document.
     */
    std::vector<std::string> ReleaseClosure(std::string_view openUri);

    /**
     * @brief Stores text for a closure document.
     */
    void SetClosureDocument(std::string_view uri, std::string content);

    /**
     * @brief Checks if a closure document is currently cached.
     */
    [[nodiscard]] bool HasClosureDocument(std::string_view uri) const;

    /**
     * @brief Gets stored text for a closure document, if present.
     */
    [[nodiscard]] std::optional<std::string> GetClosureDocument(std::string_view uri) const;

    /**
     * @brief Purges a closure document from cache.
     */
    void PurgeClosureDocument(std::string_view uri);

    /**
     * @brief Maps a canonical filesystem path to its indexed URI.
     */
    void SetIndexedUri(std::string_view path, std::string_view uri);

    /**
     * @brief Looks up indexed URI for a canonical filesystem path.
     */
    [[nodiscard]] std::optional<std::string> GetIndexedUri(std::string_view path) const;

    /**
     * @brief Removes indexed path entry.
     */
    void RemoveIndexedPath(std::string_view path);

    /**
     * @brief An exported symbol indexed across modules.
     */
    struct ExportedSymbol
    {
        std::string name;
        std::string containerName;
        std::string fileUri;
        uint32_t line = 0;
        uint32_t character = 0;
    };

    /**
     * @brief Adds an exported symbol into the index, keeping the symbol table sorted.
     * @param[in] symbol Symbol information to insert.
     */
    void AddExportedSymbol(ExportedSymbol symbol);

    /**
     * @brief Sets the entire collection of exported symbols, sorting them for binary search.
     * @param[in] symbols Vector of exported symbols.
     */
    void SetExportedSymbols(std::vector<ExportedSymbol> symbols);

    /**
     * @brief Performs a prefix search for exported symbols matching prefix case-insensitively.
     * @param[in] prefix Prefix to match against symbol names.
     * @return Vector of matched symbols starting with prefix.
     */
    [[nodiscard]] std::vector<ExportedSymbol> FindSymbolsByPrefix(std::string_view prefix) const;

    /**
     * @brief Clears all include graph, closure, and exported symbol state.
     */
    void Clear();

  private:
    mutable std::mutex m_mutex;
    utils::WorkspaceIncludeGraph m_includeGraph;
    ankerl::unordered_dense::map<std::string, std::string, TransparentStringHash, std::equal_to<>> m_closureDocuments;
    ankerl::unordered_dense::map<std::string, std::vector<std::string>, TransparentStringHash, std::equal_to<>>
        m_openDocumentClosures;
    ankerl::unordered_dense::map<std::string, std::string, TransparentStringHash, std::equal_to<>> m_indexedUriByPath;
    std::vector<ExportedSymbol> m_exportedSymbols;
};
} // namespace angel_lsp
