#include "analysis/documentation/HoverMarkdown.h"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
std::string FormatSymbolsMarkdown(const std::vector<Symbol>& symbols, const std::optional<Symbol>& best,
                                  std::string_view accessorPropertyType, std::string_view nodeText,
                                  const HoverDocLookupContext& docCtx, const HoverPeekContext& peekCtx)
{
    std::ostringstream oss;
    oss << "```angelscript\n";
    oss << FormatSynthesizedPropertyHeader(symbols, accessorPropertyType, nodeText);

    if (!symbols.empty())
    {
        oss << FormatDeclarationText(symbols.front(), docCtx.symbolTable);
    }
    oss << "\n```";

    const size_t moreCount = CountMoreOverloads(symbols);
    if (moreCount > 0)
    {
        oss << "\n\n" << FormatMoreOverloads(symbols, moreCount, peekCtx);
    }

    std::vector<std::string> docs;
    CollectOverloadDocs(docCtx, symbols, best, docs);
    if (!docs.empty())
    {
        oss << "\n\n---";
        for (const auto& d : docs)
        {
            oss << "\n\n" << d;
        }
    }

    if (!symbols.empty() && docCtx.symbolTable)
    {
        AppendMixinMembersSection(symbols.front(), *docCtx.symbolTable, docCtx.i18n, oss);
    }

    AppendFileLocationFooter(symbols, docCtx.config, oss);
    return oss.str();
}
} // namespace angel_lsp::analysis
