#include "analysis/completion/CompletionEngine.h"

#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Tells whether the word being typed is introduced by a bare `::`, which names the global scope.
 * @param[in] prefix Text of the line before the cursor.
 * @param[in] queryPrefix Identifier fragment at the end of `prefix`.
 * @return True for `::Na`; false for `A::Na`, `A:::Na` and plain words.
 */
static bool IsGlobalScopeQualified(std::string_view prefix, std::string_view queryPrefix)
{
    if (queryPrefix.size() > prefix.size())
    {
        return false;
    }
    const size_t start = prefix.size() - queryPrefix.size();
    if (start < 2 || prefix.substr(start - 2, 2) != "::")
    {
        return false;
    }
    if (start == 2)
    {
        return true;
    }
    const char before = prefix[start - 3];
    return before != ':' && std::isalnum(static_cast<unsigned char>(before)) == 0 && before != '_';
}

void CollectUnscopedSymbols(const CompletionQuery& request, const analysis::Scope* innermostScope,
                            std::string_view prefix, CompletionCollector& collector)
{
    const int accessorMode = request.config ? request.config->engine.propertyAccessorMode : 2;
    const bool accessorsAreProperties = accessorMode >= 2;
    const bool accessorKeywordRequired = accessorMode == 3;

    const std::string_view queryPrefix = ExtractQueryPrefix(prefix);
    if (IsGlobalScopeQualified(prefix, queryPrefix))
    {
        // `::Name` skips locals, class members and enclosing namespaces: only the global scope is named.
        CollectGlobalSymbols(accessorsAreProperties, accessorKeywordRequired, queryPrefix, collector);
        return;
    }
    CollectScopeDefinitions(innermostScope, collector);
    CollectEnclosingClassMembers(collector);
    CollectVisibleNamespaceSymbols(innermostScope, queryPrefix, collector);
    CollectGlobalSymbols(accessorsAreProperties, accessorKeywordRequired, queryPrefix, collector);
    CollectDeclarationSnippets(collector);
    CollectClassOperatorSnippets(innermostScope, collector);
    CollectKeywords(collector);
}

std::vector<CompletionEntry> CollectCompletionEntries(const CompletionQuery& request)
{
    std::string prefix = GetLinePrefix(request.sourceCode, request.position.line, request.position.character);
    size_t trailingColons = 0;
    while (trailingColons < prefix.size() && prefix[prefix.size() - 1 - trailingColons] == ':')
    {
        ++trailingColons;
    }
    if (trailingColons > 0 && trailingColons != 2)
    {
        return {};
    }

    if (auto earlyItems = HandleIncludeOrLexicalSuppression(request, prefix))
    {
        return *earlyItems;
    }

    std::vector<CompletionEntry> items;
    std::unordered_set<std::string> seenLabels;
    CompletionCollector collector{items, seenLabels, request};

    if (TryCompleteInheritance(prefix, collector))
    {
        SortCompletionItemsByProximity(items);
        return items;
    }

    if (TryCompleteScopeResolution(prefix, collector))
    {
        SortCompletionItemsByProximity(items);
        return items;
    }

    if (TryCompleteTemplateArguments(prefix, collector))
    {
        SortCompletionItemsByProximity(items);
        return items;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* innermostScope =
        rootScope ? FindInnermostScope(rootScope.get(), request.position.line, request.position.character) : nullptr;

    if (TryCompleteMemberAccess(prefix, innermostScope, collector))
    {
        SortCompletionItemsByProximity(items);
        return items;
    }

    CollectUnscopedSymbols(request, innermostScope, prefix, collector);

    const bool enableSmartRanking = !request.config || request.config->features.completionSmartTypeRanking;
    if (enableSmartRanking)
    {
        ApplySmartTypeRanking(items, prefix, innermostScope, request);
    }
    SortCompletionItemsByProximity(items);

    return items;
}

std::optional<std::string> ResolveCompletionDocumentation(std::string_view qualifiedName,
                                                          const SymbolTable& symbolTable,
                                                          const DocumentTextReader& readDocument)
{
    if (qualifiedName.empty() || !readDocument)
    {
        return std::nullopt;
    }

    auto syms = symbolTable.FindSymbolsPtr(qualifiedName);
    if (syms)
    {
        for (const auto& symbol : *syms)
        {
            const std::string* text = readDocument(symbol.fileUri);
            if (!text)
            {
                continue;
            }

            std::string documentation = analysis::ExtractDocComment(*text, symbol.startLine);
            if (documentation.empty())
            {
                continue;
            }
            return documentation;
        }
    }

    return std::nullopt;
}

} // namespace angel_lsp::analysis::completion
