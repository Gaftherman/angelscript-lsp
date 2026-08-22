#include "features/workspace_symbol/WorkspaceSymbolHandler.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Converts a string_view to a lowercase std::string.
         * @param str Input string view.
         * @return Lowercase copy of input.
         */
        inline std::string ToLower(std::string_view str)
        {
            std::string result;
            result.reserve(str.size());
            for (char c : str)
            {
                result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return result;
        }

        /**
         * @brief Maps AngelScript SymbolType to corresponding LSP SymbolKind.
         * @param type Internal SymbolType enum.
         * @param containerName Enclosing container name.
         * @return Equivalent LSP SymbolKind.
         */
        inline lsp::SymbolKind ToSymbolKind(analysis::SymbolType type, const std::string &containerName)
        {
            switch (type)
            {
            case analysis::SymbolType::Class:
                return lsp::SymbolKind::Class;
            case analysis::SymbolType::Interface:
                return lsp::SymbolKind::Interface;
            case analysis::SymbolType::Enum:
                return lsp::SymbolKind::Enum;
            case analysis::SymbolType::Namespace:
                return lsp::SymbolKind::Namespace;
            case analysis::SymbolType::Funcdef:
                return lsp::SymbolKind::Function;
            case analysis::SymbolType::Typedef:
                return lsp::SymbolKind::Class;
            case analysis::SymbolType::Property:
                return lsp::SymbolKind::Property;
            case analysis::SymbolType::Function:
                return containerName.empty() ? lsp::SymbolKind::Function : lsp::SymbolKind::Method;
            case analysis::SymbolType::Variable:
                return containerName.empty() ? lsp::SymbolKind::Variable : lsp::SymbolKind::Field;
            default:
                return lsp::SymbolKind::Variable;
            }
        }

        /**
         * @brief Container for a scored search match candidate.
         */
        struct ScoredSymbol
        {
            int score = 0;
            lsp::SymbolInformation info;
        };

        /**
         * @brief Result of matching a symbol against a search query.
         */
        struct MatchResult
        {
            bool matched = false;
            int score = 0;
        };

        /**
         * @brief Computes match ranking score between query and symbol.
         * @param queryLower Lowercase query string.
         * @param queryOriginal Original query string with case.
         * @param sym Candidate symbol.
         * @return MatchResult indicating if matched and the ranking score.
         */
        MatchResult MatchSymbol(std::string_view queryLower, std::string_view queryOriginal, const analysis::Symbol &sym)
        {
            if (queryLower.empty())
            {
                return { true, 1 };
            }

            std::string nameLower = ToLower(sym.name);

            // 1. Exact match on name
            if (nameLower == queryLower)
            {
                int score = (sym.name == queryOriginal) ? 110 : 100;
                return { true, score };
            }

            // 2. Prefix match on name
            if (nameLower.starts_with(queryLower))
            {
                int score = (sym.name.starts_with(queryOriginal)) ? 85 : 80;
                return { true, score };
            }

            // 3. Substring match on name
            if (nameLower.find(queryLower) != std::string::npos)
            {
                return { true, 50 };
            }

            // 4. Match on qualified name
            std::string qualLower = ToLower(sym.qualifiedName);
            if (qualLower.find(queryLower) != std::string::npos)
            {
                return { true, 40 };
            }

            // 5. Fuzzy subsequence match on name
            size_t qIdx = 0;
            for (size_t i = 0; i < nameLower.size() && qIdx < queryLower.size(); ++i)
            {
                if (nameLower[i] == queryLower[qIdx])
                {
                    qIdx++;
                }
            }
            if (qIdx == queryLower.size())
            {
                return { true, 20 };
            }

            return { false, 0 };
        }
    }

    std::optional<WorkspaceSymbolResult> GetWorkspaceSymbols(const WorkspaceSymbolRequest &request)
    {
        std::string queryLower = ToLower(request.query);
        std::vector<ScoredSymbol> matches;
        std::unordered_set<std::string> seenKeys;

        request.symbolTable.ForEachSymbol(
            [&](const std::string &/*qualifiedName*/, const std::vector<analysis::Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    if (sym.type == analysis::SymbolType::CallReference)
                    {
                        continue;
                    }

                    // Deduplicate identical declarations (e.g. enum dual indexing)
                    std::string dedupKey = sym.fileUri + ":" + std::to_string(sym.startLine) + ":" + std::to_string(sym.startCharacter) + ":" + sym.name;
                    if (seenKeys.contains(dedupKey))
                    {
                        continue;
                    }

                    auto match = MatchSymbol(queryLower, request.query, sym);
                    if (match.matched)
                    {
                        seenKeys.insert(std::move(dedupKey));

                        lsp::SymbolInformation info;
                        info.name = sym.name;
                        info.kind = ToSymbolKind(sym.type, sym.containerName);

                        if (!sym.containerName.empty())
                        {
                            info.containerName = sym.containerName;
                        }

                        info.location = lsp::Location{
                            lsp::DocumentUri::parse(sym.fileUri),
                            lsp::Range{
                                lsp::Position{ sym.startLine, sym.startCharacter },
                                lsp::Position{ sym.endLine, sym.endCharacter }
                            }
                        };

                        matches.push_back(ScoredSymbol{ match.score, std::move(info) });
                    }
                }
            });

        std::sort(matches.begin(), matches.end(),
                  [](const ScoredSymbol &a, const ScoredSymbol &b)
                  {
                      if (a.score != b.score)
                      {
                          return a.score > b.score;
                      }
                      return a.info.name < b.info.name;
                  });

        WorkspaceSymbolResult result;
        size_t count = std::min(matches.size(), request.maxResults);
        result.reserve(count);

        for (size_t i = 0; i < count; ++i)
        {
            result.push_back(std::move(matches[i].info));
        }

        return result;
    }
}
