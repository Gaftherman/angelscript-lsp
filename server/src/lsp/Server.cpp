#include "lsp/Server.h"

#include "lsp/pipeline/LspSessionCoordinator.h"
#include "lsp/state/WorkspaceStateStore.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp
{

std::vector<std::string> Server::WorkspaceRoots() const
{
    return m_workspaceStore->GetWorkspaceRoots();
}

std::shared_ptr<const std::vector<std::string>> Server::SearchDirectories() const
{
    return m_workspaceStore->GetSearchDirectories();
}

std::string Server::EngineProfile() const
{
    return m_workspaceStore->GetEngineProfile();
}

std::shared_ptr<const TransparentStringSet> Server::DefinedWords() const
{
    return m_workspaceStore->GetDefinedWords();
}

bool Server::SetDefinedWordsFrom(const std::string& source, std::vector<std::string> words)
{
    return m_workspaceStore->SetDefinedWordsFrom(source, std::move(words));
}

std::string_view Server::ImplicitIncludeExtension() const noexcept
{
    return m_config.implicitIncludeExtension ? std::string_view(m_config.info.fileExtension) : std::string_view{};
}

void Server::SetWorkspaceScanComplete(bool complete) noexcept
{
    m_sessionCoordinator->SetWorkspaceScanComplete(complete);
}

analysis::formatting::BraceStyle Server::CurrentBraceStyle() const
{
    return m_sessionCoordinator->GetFormatBraceStyleKR() ? analysis::formatting::BraceStyle::KAndR
                                                         : analysis::formatting::BraceStyle::Allman;
}

bool Server::CurrentSpacesInsideParentheses() const noexcept
{
    return m_sessionCoordinator->GetFormatSpacesInsideParentheses();
}

bool Server::CurrentKeepEmptyBlocksOnSingleLine() const noexcept
{
    return m_sessionCoordinator->GetFormatKeepEmptyBlocksOnSingleLine();
}

analysis::formatting::PointerAlignment Server::CurrentPointerAlignment() const noexcept
{
    return m_sessionCoordinator->GetFormatPointerAlignment();
}

void Server::BindEngineConfiguration(const std::function<void(std::string_view, bool&)>& applyBool,
                                     const std::function<void(std::string_view, int&)>& applyInt)
{
    applyBool("allowUnsafeReferences", m_config.engine.allowUnsafeReferences);
    applyBool("privatePropAsProtected", m_config.engine.privatePropAsProtected);
    applyBool("disallowGlobalVars", m_config.engine.disallowGlobalVars);
    applyBool("allowMultilineStrings", m_config.engine.allowMultilineStrings);
    applyBool("disallowValueAssignForRef", m_config.engine.disallowValueAssignForRef);
    applyBool("disableIntegerDivision", m_config.engine.disableIntegerDivision);
    applyBool("disallowEmptyListElements", m_config.engine.disallowEmptyListElements);
    applyBool("foreachSupport", m_config.engine.foreachSupport);
    applyBool("requireEnumScope", m_config.engine.requireEnumScope);
    applyBool("alwaysImplDefaultConstruct", m_config.engine.alwaysImplDefaultConstruct);
    applyBool("allowUnicodeIdentifiers", m_config.engine.allowUnicodeIdentifiers);
    applyBool("ignoreDuplicateSharedIntf", m_config.engine.ignoreDuplicateSharedIntf);

    applyInt("propertyAccessorMode", m_config.engine.propertyAccessorMode);
    applyInt("boolConversionMode", m_config.engine.boolConversionMode);
    applyInt("useCharacterLiterals", m_config.engine.useCharacterLiterals);
    applyInt("alterSyntaxNamedArgs", m_config.engine.alterSyntaxNamedArgs);
    applyInt("compilerWarnings", m_config.engine.compilerWarnings);
}

} // namespace angel_lsp
