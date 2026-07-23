/**
 * @file EnumTypeDefValidator.cpp
 * @brief Implementation of EnumTypeDefValidator for 'enum' and 'typedef' declarations.
 * @ingroup Analysis
 */

#include "EnumTypeDefValidator.h"
#include <spdlog/fmt/fmt.h>
#include <ankerl/unordered_dense.h>

namespace analysis::validators
{
    std::vector<lsp::Diagnostic> EnumTypeDefValidator::ValidateEnum(
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

        // 1. Extract enum name identifier
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
            std::string_view enumName = doc.SourceAt(nameNode);
            SymbolTable combined = localTable;
            combined.MergeGlobals(globalTable);
            std::vector<Symbol *> globals = combined.FindAllGlobalsByName(enumName);
            size_t enumCount = 0;
            for (const auto *sym : globals)
            {
                if (sym->kind == SymbolKind::Enum)
                {
                    enumCount++;
                }
            }
            if (enumCount > 1)
            {
                TSPoint start = ts_node_start_point(nameNode);
                TSPoint end = ts_node_end_point(nameNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagDuplicateEnumName), enumName);
                diags.push_back(d);
            }
        }

        // 2. Traverse internal enumerators & check collisions + initializer types
        ankerl::unordered_dense::set<std::string> enumeratorsSeen;

        uint32_t childCount = ts_node_child_count(node);
        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode child = ts_node_child(node, i);
            const char *cType = ts_node_type(child);
            if (cType && (std::string(cType) == "enum_member" || std::string(cType) == "enumerator"))
            {
                TSNode memNameNode = ts_node_child_by_field_name(child, "name", sizeof("name") - 1);
                if (ts_node_is_null(memNameNode))
                {
                    memNameNode = ts_node_child(child, 0);
                }

                if (!ts_node_is_null(memNameNode))
                {
                    std::string_view memName = doc.SourceAt(memNameNode);
                    std::string memStr(memName);

                    if (enumeratorsSeen.contains(memStr))
                    {
                        TSPoint start = ts_node_start_point(memNameNode);
                        TSPoint end = ts_node_end_point(memNameNode);

                        lsp::Diagnostic d;
                        d.range.start.line = start.row;
                        d.range.start.character = start.column;
                        d.range.end.line = end.row;
                        d.range.end.character = end.column;
                        d.severity = lsp::DiagnosticSeverity::Error;
                        d.source = "angelscript";
                        d.message = fmt::format(fmt::runtime(strs.diagDuplicateEnumerator), memStr);
                        diags.push_back(d);
                    }
                    else
                    {
                        enumeratorsSeen.insert(memStr);
                    }

                    // Check initializer '=' EXPR
                    TSNode valueNode = ts_node_child_by_field_name(child, "value", sizeof("value") - 1);
                    if (ts_node_is_null(valueNode))
                    {
                        uint32_t subCount = ts_node_child_count(child);
                        bool foundEq = false;
                        for (uint32_t j = 0; j < subCount; ++j)
                        {
                            TSNode subChild = ts_node_child(child, j);
                            std::string_view subText = doc.SourceAt(subChild);
                            if (subText == "=")
                            {
                                foundEq = true;
                                continue;
                            }
                            if (foundEq)
                            {
                                valueNode = subChild;
                                break;
                            }
                        }
                    }

                    if (!ts_node_is_null(valueNode))
                    {
                        auto inferredOpt = TypeEvaluator::InferType(valueNode, doc, globalTable, localTable);
                        if (inferredOpt.has_value())
                        {
                            std::string inferred = inferredOpt.value();
                            if (inferred != "int" && inferred != "uint" && inferred != "int8" &&
                                inferred != "int16" && inferred != "int32" && inferred != "int64" &&
                                inferred != "uint8" && inferred != "uint16" && inferred != "uint32" && inferred != "uint64")
                            {
                                TSPoint vStart = ts_node_start_point(valueNode);
                                TSPoint vEnd = ts_node_end_point(valueNode);

                                lsp::Diagnostic d;
                                d.range.start.line = vStart.row;
                                d.range.start.character = vStart.column;
                                d.range.end.line = vEnd.row;
                                d.range.end.character = vEnd.column;
                                d.severity = lsp::DiagnosticSeverity::Error;
                                d.source = "angelscript";
                                d.message = fmt::format(fmt::runtime(strs.diagInvalidEnumInitializer), inferred);
                                diags.push_back(d);
                            }
                        }
                    }
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> EnumTypeDefValidator::ValidateTypedef(
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

        // Extract alias identifier (usually last identifier child of typedef_declaration)
        TSNode aliasNode = ts_node_child_by_field_name(node, "name", sizeof("name") - 1);
        if (ts_node_is_null(aliasNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (int i = (int)count - 1; i >= 0; --i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && std::string(cType) == "identifier")
                {
                    aliasNode = child;
                    break;
                }
            }
        }

        if (!ts_node_is_null(aliasNode))
        {
            std::string_view aliasName = doc.SourceAt(aliasNode);
            SymbolTable combined = localTable;
            combined.MergeGlobals(globalTable);
            std::vector<Symbol *> globals = combined.FindAllGlobalsByName(aliasName);
            size_t count = 0;
            for (const auto *sym : globals)
            {
                if (sym->kind == SymbolKind::Typedef || sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Enum)
                {
                    count++;
                }
            }
            if (count > 1)
            {
                TSPoint start = ts_node_start_point(aliasNode);
                TSPoint end = ts_node_end_point(aliasNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagTypedefCollision), aliasName);
                diags.push_back(d);
            }
        }

        return diags;
    }

} // namespace analysis::validators
