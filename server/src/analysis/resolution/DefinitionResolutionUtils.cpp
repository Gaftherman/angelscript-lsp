#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/DefinitionResolution.h"
#include "analysis/resolution/DefinitionResolutionInternal.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

const LocalReference* FindReferenceAt(const Scope* scope, uint32_t line, uint32_t character) noexcept
{
    if (!scope)
    {
        return nullptr;
    }
    for (const auto& ref : scope->references)
    {
        if (ref.startLine == line && ref.startCharacter <= character && character <= ref.endCharacter)
        {
            return &ref;
        }
    }
    return nullptr;
}

MappedLocationTarget MapSymbolLocation(const Symbol& sym, std::string_view requestUri)
{
    const bool hasFullRange = sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0;
    const SourceRange range =
        hasFullRange ? sym.fullRange : SourceRange{sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter};
    const std::string_view targetUri = sym.fileUri.empty() ? requestUri : std::string_view(sym.fileUri);
    return MappedLocationTarget{targetUri, range};
}

/**
 * @brief Picks the narrowest namespace declared in @p uri whose line span contains @p line.
 * @param[in] uri Document URI; symbols stored under a file path are matched after URI conversion.
 * @param[in] line Zero-based cursor line.
 * @param[in] table Symbol table providing the document's namespace declarations.
 * @return Qualified name of the innermost namespace, or an empty string at global scope.
 * @note Returns an owned copy because the symbol vectors are only borrowed for the duration of the table visit.
 */
std::string FindEnclosingNamespace(std::string_view uri, uint32_t line, const SymbolTable& table)
{
    std::string enclosingNs;
    uint32_t bestSpan = UINT32_MAX;
    table.ForEachSymbolInFile(
        uri,
        [&]([[maybe_unused]] std::string_view key, const std::vector<Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.type != SymbolType::Namespace || (sym.fileUri != uri && utils::PathToUri(sym.fileUri) != uri))
                {
                    continue;
                }
                const auto range = sym.fullRange.endLine != 0 ? sym.fullRange : sym.selectionRange;
                if (line >= range.startLine && line <= range.endLine)
                {
                    const uint32_t span = range.endLine - range.startLine;
                    if (span <= bestSpan)
                    {
                        bestSpan = span;
                        enclosingNs = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                    }
                }
            }
        });
    return enclosingNs;
}

} // namespace angel_lsp::analysis
