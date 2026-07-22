/**
 * @file UsingValidator.cpp
 * @brief Implementation of UsingValidator for 'using namespace' directives.
 * @ingroup Analysis
 */

#include "UsingValidator.h"
#include <spdlog/fmt/fmt.h>
#include <ankerl/unordered_dense.h>

namespace analysis::validators
{
    std::vector<lsp::Diagnostic> UsingValidator::Validate(
        TSNode node,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(node))
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        // Extract text of namespace identifier after 'using namespace'
        std::string_view fullText = doc.SourceAt(node);
        size_t pos = fullText.find("namespace");
        if (pos == std::string_view::npos)
        {
            return diags;
        }

        size_t nsStart = fullText.find_first_not_of(" \t", pos + 9);
        if (nsStart == std::string_view::npos)
        {
            return diags;
        }

        size_t nsEnd = fullText.find_first_of(" \t;\r\n", nsStart);
        std::string nsName(fullText.substr(nsStart, nsEnd == std::string_view::npos ? std::string_view::npos : nsEnd - nsStart));

        if (nsName.empty())
        {
            return diags;
        }

        // Check if namespace exists in globalTable, localTable, or registered using namespaces
        bool exists = (globalTable.FindGlobalByName(nsName) != nullptr) ||
                      (globalTable.FindFirst(nsName) != nullptr) ||
                      (!globalTable.FindInContainer(nsName).empty()) ||
                      (localTable.FindGlobalByName(nsName) != nullptr) ||
                      (!localTable.FindInContainer(nsName).empty());

        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);

        if (!exists)
        {
            lsp::Diagnostic d;
            d.range.start.line = start.row;
            d.range.start.character = start.column;
            d.range.end.line = end.row;
            d.range.end.character = end.column;
            d.severity = lsp::DiagnosticSeverity::Error;
            d.source = "angelscript";
            d.message = fmt::format(fmt::runtime(strs.diagUndeclaredNamespace), nsName);
            diags.push_back(d);
        }

        // Check for duplicate using namespace in localTable usingNamespaces
        const auto &usingList = localTable.GetUsingNamespaces();
        size_t matchCount = 0;
        for (const auto &u : usingList)
        {
            if (u == nsName)
            {
                matchCount++;
            }
        }
        if (matchCount > 1)
        {
            lsp::Diagnostic d;
            d.range.start.line = start.row;
            d.range.start.character = start.column;
            d.range.end.line = end.row;
            d.range.end.character = end.column;
            d.severity = lsp::DiagnosticSeverity::Warning;
            d.source = "angelscript";
            d.message = fmt::format(fmt::runtime(strs.diagDuplicateUsingDirective), nsName);
            diags.push_back(d);
        }

        return diags;
    }

} // namespace analysis::validators
