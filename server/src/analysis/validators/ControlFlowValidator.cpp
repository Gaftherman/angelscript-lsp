/**
 * @file ControlFlowValidator.cpp
 * @brief Implementation of ControlFlowValidator for return, break, continue, switch, and foreach statements.
 * @ingroup Analysis
 */

#include "ControlFlowValidator.h"
#include <ankerl/unordered_dense.h>
#include <spdlog/fmt/fmt.h>

namespace analysis::validators
{
    std::vector<lsp::Diagnostic> ControlFlowValidator::ValidateControlFlow(
        TSNode funcNode,
        const Document &doc,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(funcNode))
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        auto walkControlFlow = [&](auto self, TSNode bNode, int loopDepth, int switchDepth) -> void
        {
            if (ts_node_is_null(bNode))
            {
                return;
            }

            const char *cTypeC = ts_node_type(bNode);
            std::string cType = cTypeC ? std::string(cTypeC) : "";

            int currentLoopDepth = loopDepth;
            int currentSwitchDepth = switchDepth;

            if (cType == "for_statement" || cType == "while_statement" || cType == "do_while_statement" || cType == "foreach_statement" || cType == "for" || cType == "while" || cType == "do" || cType == "foreach")
            {
                currentLoopDepth++;
            }
            else if (cType == "switch_statement" || cType == "switch")
            {
                currentSwitchDepth++;
            }
            else if (cType == "break_statement" || cType == "break")
            {
                if (currentLoopDepth == 0 && currentSwitchDepth == 0)
                {
                    TSPoint start = ts_node_start_point(bNode);
                    TSPoint end = ts_node_end_point(bNode);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = std::string(strs.diagBreakOutsideLoop);
                    diags.push_back(d);
                }
            }
            else if (cType == "continue_statement" || cType == "continue")
            {
                if (currentLoopDepth == 0)
                {
                    TSPoint start = ts_node_start_point(bNode);
                    TSPoint end = ts_node_end_point(bNode);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = std::string(strs.diagContinueOutsideLoop);
                    diags.push_back(d);
                }
            }

            uint32_t count = ts_node_child_count(bNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                self(self, ts_node_child(bNode, i), currentLoopDepth, currentSwitchDepth);
            }
        };

        walkControlFlow(walkControlFlow, funcNode, 0, 0);

        return diags;
    }

    std::vector<lsp::Diagnostic> ControlFlowValidator::ValidateSwitch(
        TSNode switchNode,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(switchNode))
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        ankerl::unordered_dense::set<std::string> seenCases;

        auto walkSwitch = [&](auto self, TSNode node) -> void
        {
            if (ts_node_is_null(node))
            {
                return;
            }

            const char *cTypeC = ts_node_type(node);
            std::string cType = cTypeC ? std::string(cTypeC) : "";

            if (cType == "case_clause" || cType == "case")
            {
                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_child(node, i);
                    std::string_view text = doc.SourceAt(child);
                    if (text != "case" && text != ":")
                    {
                        std::string caseText = std::string(text);
                        if (seenCases.contains(caseText))
                        {
                            TSPoint start = ts_node_start_point(child);
                            TSPoint end = ts_node_end_point(child);

                            lsp::Diagnostic d;
                            d.range.start.line = start.row;
                            d.range.start.character = start.column;
                            d.range.end.line = end.row;
                            d.range.end.character = end.column;
                            d.severity = lsp::DiagnosticSeverity::Error;
                            d.source = "angelscript";
                            d.message = fmt::format(fmt::runtime(strs.diagDuplicateCaseValue), caseText);
                            diags.push_back(d);
                        }
                        else
                        {
                            seenCases.insert(caseText);
                        }
                        break;
                    }
                }
            }

            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                self(self, ts_node_child(node, i));
            }
        };

        walkSwitch(walkSwitch, switchNode);

        return diags;
    }

    std::vector<lsp::Diagnostic> ControlFlowValidator::ValidateForeach(
        TSNode foreachNode,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        return diags;
    }

} // namespace analysis::validators
