#include "lsp/Server.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp
{

/**
 * @brief Constructs a TextEdit for replacing an include directive's path substring.
 * @param[in] line The source text line containing the directive.
 * @param[in] directive The extracted include directive metadata.
 * @param[in] replacement The new relative path replacement string.
 * @return TextEdit if opening and closing delimiters are matched, or std::nullopt otherwise.
 */
static std::optional<lsp::TextEdit> TryBuildDirectiveEdit(std::string_view line,
                                                          const angel_lsp::utils::IncludeDirective& directive,
                                                          std::string_view replacement)
{
    const char open = directive.isAngled ? '<' : '"';
    const char close = directive.isAngled ? '>' : '"';
    const size_t openPos = line.find(open);
    if (openPos == std::string_view::npos)
    {
        return std::nullopt;
    }
    const size_t closePos = line.find(close, openPos + 1);
    if (closePos == std::string_view::npos)
    {
        return std::nullopt;
    }

    lsp::TextEdit edit;
    edit.range.start.line = static_cast<uint32_t>(directive.line);
    edit.range.start.character = static_cast<uint32_t>(openPos + 1);
    edit.range.end.line = static_cast<uint32_t>(directive.line);
    edit.range.end.character = static_cast<uint32_t>(closePos);
    edit.newText = std::string(replacement);
    return edit;
}

static std::optional<lsp::TextDocumentEdit> BuildIncludeRewrite(const Server& server, std::string_view includerPath,
                                                                const std::string& oldTargetPath,
                                                                const std::string& newTargetPath)
{
    // Read from the editor's buffer when it has one - it may hold unsaved edits, and rewriting
    // against the copy on disk would produce an edit whose line numbers do not match what the
    // user is looking at.
    const std::string includerUri = Server::UriFromPath(includerPath);
    std::string text;
    if (auto open = server.FindDocumentText(includerUri))
    {
        text = *open;
    }
    else
    {
        std::ifstream file(std::filesystem::path(includerPath), std::ios::binary);
        if (!file.is_open())
            return std::nullopt;
        text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // The path as the directive should now spell it: relative to the including file, with
    // forward slashes, which is how every #include in the corpus is written and what
    // ResolveIncludePath will read back.
    std::error_code relativeError;
    std::filesystem::path relative = std::filesystem::relative(
        std::filesystem::path(newTargetPath), std::filesystem::path(includerPath).parent_path(), relativeError);
    if (relativeError || relative.empty())
        return std::nullopt;
    const std::string replacement = relative.generic_string();

    std::vector<lsp::TextEdit> edits;
    for (const auto& directive : angel_lsp::utils::IncludeResolver::ExtractIncludes(text))
    {
        const std::string resolved = angel_lsp::utils::IncludeResolver::ResolveIncludePath(
            angel_lsp::utils::IncludeResolveRequest{directive.rawPath, includerPath, *server.SearchDirectories(),
                                                    server.IncludeAllowedRoots(), server.ImplicitIncludeExtension()});
        if (resolved != oldTargetPath)
            continue;

        const std::string_view line = angel_lsp::utils::GetLine(text, static_cast<uint32_t>(directive.line));
        if (auto edit = TryBuildDirectiveEdit(line, directive, replacement))
        {
            edits.push_back(std::move(*edit));
        }
    }

    if (edits.empty())
        return std::nullopt;

    lsp::TextDocumentEdit documentEdit;
    lsp::OptionalVersionedTextDocumentIdentifier identifier;
    identifier.uri = lsp::DocumentUri(lsp::Uri::parse(includerUri));
    documentEdit.textDocument = identifier;
    for (auto& edit : edits)
    {
        documentEdit.edits.push_back(std::move(edit));
    }

    return documentEdit;
}

void Server::InvalidateCreatedClosureFiles(const std::vector<std::string>& createdPaths)
{
    angel_lsp::parser::AngelScriptParser createdParser(m_logger.get());
    for (const std::string& path : createdPaths)
    {
        ReindexTrackedClosureFile(path, createdParser);
    }
}

void Server::HandleNotificationsWorkspace_DidCreateFiles(lsp::notifications::Workspace_DidCreateFiles::Params&& params)
{
    std::vector<std::string> createdPaths;
    for (const auto& created : params.files)
    {
        const std::string path = CanonicalPathFromUri(DocumentKey(created.uri.toString()));
        if (!path.empty() && path.ends_with(m_config.info.fileExtension))
        {
            createdPaths.push_back(path);
        }
    }

    if (createdPaths.empty())
        return;

    InvalidateCreatedClosureFiles(createdPaths);

    const auto searchDirectories = SearchDirectories();
    for (const auto& [openUri, text] : GetDocumentStore().GetSnapshot())
    {
        const std::string openPath = CanonicalPathFromUri(openUri);
        if (!openPath.empty())
        {
            GetIncludeGraph().UpdateFile(
                utils::WorkspaceIncludeGraph::UpdateFileRequest{openPath,
                                                                text,
                                                                *searchDirectories,
                                                                IncludeAllowedRoots(),
                                                                std::string(ImplicitIncludeExtension()),
                                                                {DefinedWords(), m_config.preprocessor}});
        }
    }

    ReanalyseOpenDocuments();
}

void Server::HandleNotificationsWorkspace_DidDeleteFiles(lsp::notifications::Workspace_DidDeleteFiles::Params&& params)
{
    // The editor tells us directly rather than through the file watcher, which means it arrives
    // even when the watcher's own glob would have missed the file, and it arrives once rather
    // than as whatever burst the filesystem happened to produce.
    bool anythingChanged = false;

    for (const auto& deleted : params.files)
    {
        const std::string uriStr = DocumentKey(deleted.uri.toString());
        const std::string path = CanonicalPathFromUri(uriStr);
        if (path.empty())
            continue;

        bool wasPublished = false;
        {
            std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
            wasPublished = m_publishedForModules.erase(uriStr) > 0;
        }
        PurgeClosureFile(uriStr);
        if (wasPublished || GetDocumentStore().IsOpen(uriStr))
        {
            PublishDiagnostics(uriStr, {});
        }
        anythingChanged = GetIncludeGraph().RemoveFile(path) || anythingChanged;
        GetDocumentStore().RemoveClientUri(uriStr);
    }

    if (!anythingChanged)
        return;

    ReanalyseOpenDocuments();
}

void Server::HandleNotificationsWorkspace_DidRenameFiles(lsp::notifications::Workspace_DidRenameFiles::Params&& params)
{
    angel_lsp::parser::AngelScriptParser renameParser(m_logger.get());
    std::vector<lsp::TextDocumentEdit> documentEdits;
    bool anythingChanged = false;

    for (const auto& renamed : params.files)
    {
        const std::string oldUri = DocumentKey(renamed.oldUri.toString());
        const std::string newUri = DocumentKey(renamed.newUri.toString());
        const std::string oldPath = CanonicalPathFromUri(oldUri);
        const std::string newPath = CanonicalPathFromUri(newUri);
        if (oldPath.empty() || newPath.empty())
            continue;

        // Collected BEFORE the old file leaves the graph: the edge that says who included it is
        // the only record of which files need rewriting, and RemoveFile takes it with it.
        const std::vector<std::string> includers = GetIncludeGraph().GetFilesIncluding(oldPath);

        PurgeClosureFile(oldUri);
        GetIncludeGraph().RemoveFile(oldPath);
        GetDocumentStore().RemoveClientUri(oldUri);
        anythingChanged = true;

        for (const std::string& includer : includers)
        {
            auto edit = BuildIncludeRewrite(*this, includer, oldPath, newPath);
            if (edit.has_value())
                documentEdits.push_back(std::move(*edit));
        }

        // The file at its new name is indexed only if something already reached it - the same
        // rule the watched-files handler applies, and for the same reason: reading every
        // renamed file off disk would turn a directory rename into a full workspace parse.
        bool shouldIndex = !includers.empty();
        if (!shouldIndex)
        {
            std::lock_guard<std::mutex> lock(m_closureMutex);
            shouldIndex = m_indexedUriByPath.contains(oldPath);
        }
        if (shouldIndex)
            IndexClosureFile(newPath, renameParser);
    }

    if (!documentEdits.empty())
    {
        using DocumentChange = lsp::OneOf<lsp::TextDocumentEdit, lsp::CreateFile, lsp::RenameFile, lsp::DeleteFile>;
        lsp::WorkspaceEdit workspaceEdit;
        lsp::Array<DocumentChange> changes;
        for (auto& edit : documentEdits)
            changes.push_back(DocumentChange(std::move(edit)));
        workspaceEdit.documentChanges = std::move(changes);

        lsp::ApplyWorkspaceEditParams applyParams;
        applyParams.label = std::string("Update #include paths");
        applyParams.edit = std::move(workspaceEdit);

        // Sent rather than applied: the edit belongs to the editor's undo stack, and a server
        // writing the files itself would leave the user unable to undo a rename's consequences
        // along with the rename.
        m_messageHandler->sendRequest<lsp::requests::Workspace_ApplyEdit>(
            std::move(applyParams), [](auto&&) {}, [](const auto&) {});
    }

    if (anythingChanged)
        ReanalyseOpenDocuments();
}

} // namespace angel_lsp
