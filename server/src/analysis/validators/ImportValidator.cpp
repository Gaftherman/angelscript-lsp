/**
 * @file ImportValidator.cpp
 * @brief Implementation of ImportValidator for 'import' directives.
 * @ingroup Analysis
 */

#include "ImportValidator.h"
#include <spdlog/fmt/fmt.h>

namespace analysis::validators
{
    std::vector<lsp::Diagnostic> ImportValidator::Validate(
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
        std::string_view fullText = doc.SourceAt(node);
        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);

        // 1. Check for 'from' keyword and module string
        size_t fromPos = fullText.find("from");
        if (fromPos == std::string_view::npos)
        {
            lsp::Diagnostic d;
            d.range.start.line = start.row;
            d.range.start.character = start.column;
            d.range.end.line = end.row;
            d.range.end.character = end.column;
            d.severity = lsp::DiagnosticSeverity::Error;
            d.source = "angelscript";
            d.message = std::string(strs.diagInvalidImportModule);
            diags.push_back(d);
            return diags;
        }

        size_t strStart = fullText.find_first_of("\"'", fromPos + 4);
        if (strStart == std::string_view::npos)
        {
            lsp::Diagnostic d;
            d.range.start.line = start.row;
            d.range.start.character = start.column;
            d.range.end.line = end.row;
            d.range.end.character = end.column;
            d.severity = lsp::DiagnosticSeverity::Error;
            d.source = "angelscript";
            d.message = std::string(strs.diagInvalidImportModule);
            diags.push_back(d);
            return diags;
        }

        char quote = fullText[strStart];
        size_t strEnd = fullText.find(quote, strStart + 1);
        if (strEnd == std::string_view::npos)
        {
            lsp::Diagnostic d;
            d.range.start.line = start.row;
            d.range.start.character = start.column;
            d.range.end.line = end.row;
            d.range.end.character = end.column;
            d.severity = lsp::DiagnosticSeverity::Error;
            d.source = "angelscript";
            d.message = std::string(strs.diagInvalidImportModule);
            diags.push_back(d);
            return diags;
        }

        // 2. Check function name identifier
        TSNode nameNode = ts_node_child_by_field_name(node, "name", sizeof("name") - 1);
        if (ts_node_is_null(nameNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && std::string(cType) == "identifier")
                {
                    nameNode = child;
                    break;
                }
            }
        }

        if (!ts_node_is_null(nameNode))
        {
            std::string_view name = doc.SourceAt(nameNode);
            std::vector<Symbol *> globals = globalTable.FindAllGlobalsByName(name);
            size_t importCount = 0;
            for (const auto *sym : globals)
            {
                if (sym->kind == SymbolKind::Function)
                {
                    importCount++;
                }
            }
            if (importCount > 1)
            {
                TSPoint nStart = ts_node_start_point(nameNode);
                TSPoint nEnd = ts_node_end_point(nameNode);

                lsp::Diagnostic d;
                d.range.start.line = nStart.row;
                d.range.start.character = nStart.column;
                d.range.end.line = nEnd.row;
                d.range.end.character = nEnd.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagDuplicateImport), name);
                diags.push_back(d);
            }
        }

        return diags;
    }

} // namespace analysis::validators
