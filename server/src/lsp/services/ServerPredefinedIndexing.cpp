#include "analysis/types/EngineConfigConverter.h"
#include "lsp/Server.h"
#include "parser/SyntaxTree.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <spdlog/fmt/fmt.h>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace angel_lsp
{
static std::optional<std::string> ReadPredefinedFile(std::string_view normPath, std::string_view filePath,
                                                     angel_lsp::utils::LspLogger* logger)
{
    if (logger)
    {
        logger->LogInfo(fmt::format("[File Read] Opening and reading predefined file: {}", filePath));
    }
    angel_lsp::utils::HighResTimer readTimer;
    std::ifstream file(std::filesystem::path(normPath), std::ios::binary);
    if (!file.is_open())
    {
        file.open(std::filesystem::path(filePath), std::ios::binary);
    }
    if (!file.is_open())
    {
        if (logger)
        {
            logger->LogError(fmt::format("[File Read] Cannot open predefined file: {}", filePath));
        }
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string sanitized = angel_lsp::utils::SanitizePredefinedContent(ss.str());
    if (logger)
    {
        logger->LogInfo(fmt::format("[File Read] Finished reading predefined file: {} in {} ({} bytes)", filePath,
                                    angel_lsp::utils::FormatDuration(readTimer.ElapsedMs()), sanitized.size()));
    }
    return sanitized;
}

void Server::CommitPredefinedIndex(std::string_view uri, std::string_view content, parser::TreePtr tree)
{
    m_scopeIndex.ClearDocument(uri);
    m_callGraph.ClearDocument(uri);
    auto preindexedDoc = std::make_shared<const document::Document>(
        document::DocumentSnapshot{std::string(uri), std::string(content), 0, 1}, std::move(tree));
    m_predefinedManager.SetPreindexedDocument(uri, preindexedDoc);
    if (m_workspaceStore)
    {
        m_workspaceStore->SetPreindexedDocument(uri, preindexedDoc);
    }
}

static uint64_t ComputeDefinedWordsHash(const std::shared_ptr<const TransparentStringSet>& defsSnapshot)
{
    uint64_t definesHash = 0;
    if (defsSnapshot)
    {
        for (const auto& w : *defsSnapshot)
        {
            definesHash ^= std::hash<std::string>{}(w) + 0x9e3779b9 + (definesHash << 6) + (definesHash >> 2);
        }
    }
    return definesHash;
}

PredefinedIndexTiming Server::IndexAndCommitPredefinedStub(std::string_view uri, std::string_view content,
                                                           angel_lsp::parser::AngelScriptParser& parser)
{
    utils::HighResTimer parseTimer;
    parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(content));
    double parseMs = parseTimer.ElapsedMs();

    utils::HighResTimer symTimer;
    ReplaceSymbolsFromTree(uri, content, &tree);
    double symMs = symTimer.ElapsedMs();

    utils::HighResTimer scopeTimer;
    CommitPredefinedIndex(uri, content, std::move(tree));
    double scopeMs = scopeTimer.ElapsedMs();

    return PredefinedIndexTiming{parseMs, symMs, scopeMs};
}

void Server::ParserPredefined(std::string_view filePath, angel_lsp::parser::AngelScriptParser& parser, bool forceReload)
{
    std::unordered_set<std::string> visited;
    ParserPredefinedInternal(filePath, parser, forceReload, visited);
}

void Server::ParserPredefinedInternal(std::string_view filePath, angel_lsp::parser::AngelScriptParser& parser,
                                      bool forceReload, std::unordered_set<std::string>& visited)
{
    std::error_code ec;
    const std::filesystem::path p(filePath);
    const auto canon = std::filesystem::weakly_canonical(p, ec);
    const std::string normPath = angel_lsp::utils::IncludeResolver::NormalizePath(ec ? p : canon);
    if (!visited.insert(normPath).second)
    {
        return;
    }

    if (!forceReload && m_predefinedManager.IsCanonicalPathLoaded(normPath))
    {
        return;
    }

    utils::HighResTimer totalTimer;
    std::string uri = UriFromPath(normPath);

    auto contentOpt = ReadPredefinedFile(normPath, filePath, m_logger.get());
    if (!contentOpt)
    {
        UnloadPredefinedUri(uri);
        return;
    }
    std::string content = std::move(*contentOpt);
    if (angel_lsp::analysis::IsOfficialEngineConfig(content))
    {
        LogInfo(fmt::format("[Predefined Index] Transpiling official AngelScript engine config: {}", filePath));
        auto conv = angel_lsp::analysis::ConvertEngineConfigToPredefined(content);
        content = std::move(conv.predefinedContent);
        m_config.engine = conv.engineProperties;
    }

    const uint64_t definesHash = ComputeDefinedWordsHash(DefinedWords());
    const uint64_t contentHash = std::hash<std::string_view>{}(content);

    if (!forceReload && m_predefinedManager.HasMatchingFingerprint(normPath, contentHash, definesHash))
    {
        return;
    }

    if (!ClaimPredefinedFile(uri, forceReload))
    {
        return;
    }

    LogInfo(fmt::format("[Predefined Index] Starting index for predefined stub: {}", filePath));
    m_predefinedManager.SetDocumentText(uri, content);

    auto timing = IndexAndCommitPredefinedStub(uri, content, parser);

    m_predefinedManager.MarkCanonicalPathLoaded(normPath);
    m_predefinedManager.RecordFingerprint(normPath, contentHash, definesHash);

    if (SetDefinedWordsFrom(normPath, angel_lsp::utils::ScanDefinedWords(content)))
    {
        LogInfo(fmt::format("Defined words changed after loading: {}", filePath));
    }

    const double totalMs = totalTimer.ElapsedMs();
    LogInfo(fmt::format("[Predefined Index] Finished index for predefined stub: {} in {} "
                        "(Parse: {}, Symbols: {}, Scopes: {})",
                        filePath, utils::FormatDuration(totalMs), utils::FormatDuration(timing.parseMs),
                        utils::FormatDuration(timing.symMs), utils::FormatDuration(timing.scopeMs)));
    LogInfo(fmt::format("Loaded predefined file: {}", filePath));

    PredefinedLoadContext ctx{parser, forceReload, visited};
    LoadPredefinedIncludes(content, filePath, ctx);
}

void Server::PreScanStubDefinedWords(std::span<const std::string> stubPaths)
{
    for (const auto& path : stubPaths)
    {
        std::error_code ec;
        const std::filesystem::path p(path);
        const auto canon = std::filesystem::weakly_canonical(p, ec);
        const std::string normPath = angel_lsp::utils::IncludeResolver::NormalizePath(ec ? p : canon);
        if (auto contentOpt = ReadPredefinedFile(normPath, path, nullptr))
        {
            SetDefinedWordsFrom(normPath, angel_lsp::utils::ScanDefinedWords(*contentOpt));
        }
    }
}

void Server::LoadPredefinedIncludes(std::string_view content, std::string_view filePath, PredefinedLoadContext& ctx)
{
    std::vector<std::string> searchDirs;
    if (const auto dirs = SearchDirectories())
    {
        searchDirs.insert(searchDirs.end(), dirs->begin(), dirs->end());
    }
    for (const auto& wsRoot : WorkspaceRoots())
    {
        if (std::string wsPath = angel_lsp::utils::UriToPath(wsRoot); !wsPath.empty())
        {
            searchDirs.push_back(std::move(wsPath));
        }
    }
    const auto allowedRoots = IncludeAllowedRoots();

    for (const auto& inc : angel_lsp::utils::IncludeResolver::ExtractIncludes(content))
    {
        std::string resolved =
            angel_lsp::utils::IncludeResolver::ResolveIncludePath(angel_lsp::utils::IncludeResolveRequest{
                .includePath = inc.rawPath,
                .currentFilePath = std::string(filePath),
                .searchDirectories = searchDirs,
                .allowedRoots = allowedRoots,
                .implicitExtension = ImplicitIncludeExtension(),
            });
        if (!resolved.empty())
        {
            const std::string normInc = angel_lsp::utils::IncludeResolver::NormalizePath(resolved);
            {
                std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                m_predefinedTransitiveIncludes.insert(normInc);
            }
            ParserPredefinedInternal(normInc, ctx.parser, ctx.forceReload, ctx.visited);
        }
    }
}

void Server::SetPredefinedReady(bool ready)
{
    if (ready)
    {
        m_symbolTable.EnsureRuleIndex();
        auto base = std::make_unique<analysis::SymbolTable>();
        base->ClonePredefinedBaseFrom(m_symbolTable, m_config.info.predefinedFileExtension);
        base->EnsureRuleIndex();
        GetWorkspaceStore().SetPredefinedBaseTable(std::move(base));
    }
    else
    {
        GetWorkspaceStore().InvalidatePredefinedBaseTable();
    }
    m_sessionCoordinator->SetPredefinedReady(ready);
}

bool Server::WaitForPredefinedReady(std::chrono::milliseconds timeout) const
{
    return m_sessionCoordinator->WaitUntilPredefinedReady(timeout);
}

bool Server::IsPredefinedReady() const noexcept
{
    return m_sessionCoordinator->IsPredefinedReady();
}
} // namespace angel_lsp
