/**
 * @file CodeActionInterfaceImplementer.cpp
 * @brief Quick-fix provider for generating missing interface method implementations in classes.
 */

#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CodeActionFacts.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/types/TypeSanitization.h"
#include "config/ServerConfig.h"
#include "parser/Primitives.h"
#include "utils/TransparentHash.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Returns default literal return string for a return type.
 * @param[in] returnType Return type text.
 * @return Default literal return text ("null", "false", "0", "\"\"").
 */
static std::string GetDefaultReturnValue(std::string_view returnType, std::string_view stringTypeName)
{
    std::string cleanRet = analysis::CleanBaseType(returnType);
    if (analysis::IsHandleTypeView(returnType))
    {
        return "null";
    }
    if (cleanRet == "bool")
    {
        return "false";
    }
    if (!stringTypeName.empty() && cleanRet == stringTypeName)
    {
        return "\"\"";
    }
    return parser::primitives::IsNumeric(cleanRet) ? "0" : "";
}

/**
 * @brief Formats stub declarations for missing interface methods.
 * @param[in] missingMethods List of missing method symbols.
 * @param[in] stringTypeName Workspace-configured string type name.
 * @return Formatted stub code string.
 */
static std::string FormatMissingInterfaceMethodStubs(const std::vector<analysis::Symbol>& missingMethods,
                                                     std::string_view stringTypeName)
{
    std::string stubs;
    for (const auto& m : missingMethods)
    {
        const auto& fn = m.GetFunction();
        std::string ret = fn.returnType.empty() ? "void" : fn.returnType;
        if (fn.modifiers.isReturnReference && analysis::TrailingReferenceModifierLength(ret) == 0)
        {
            ret += "&";
        }
        stubs += "\n    " + ret + " " + m.name + "(";
        for (size_t p = 0; p < fn.parameters.size(); ++p)
        {
            if (p > 0)
            {
                stubs += ", ";
            }
            const auto& param = fn.parameters[p];
            auto parameter = param;
            parameter.defaultValue.clear();
            stubs += analysis::FormatParameter(parameter);
        }
        stubs += ")";
        if (fn.modifiers.isConst)
        {
            stubs += " const";
        }
        stubs += " override\n    {\n";
        if (ret != "void")
        {
            const auto value = GetDefaultReturnValue(ret, stringTypeName);
            if (value.empty() || fn.modifiers.isReturnReference)
            {
                return {};
            }
            stubs += "        return " + value + ";\n";
        }
        stubs += "    }\n";
    }
    return stubs;
}

/**
 * @brief Collects diagnostic indices matching missing interface implementations for a class symbol.
 * @param[in] diagnostics Diagnostic views from the code action query.
 * @param[in] clsSym Class symbol to filter diagnostics by line range.
 * @return Vector of matching diagnostic indices.
 */
static std::vector<uint32_t>
CollectMatchingInterfaceDiagnosticIndices(std::span<const CodeActionDiagnosticView> diagnostics,
                                          const analysis::Symbol& clsSym)
{
    std::vector<uint32_t> matching;
    for (uint32_t i = 0; i < static_cast<uint32_t>(diagnostics.size()); ++i)
    {
        const auto& diag = diagnostics[i];
        if (MatchDiagnosticCode(diag, "as-err-interface-impl-missing") && diag.range.startLine <= clsSym.endLine &&
            diag.range.endLine >= clsSym.startLine)
        {
            matching.push_back(i);
        }
    }
    return matching;
}

/**
 * @brief Generates quick-fix entries for implementing missing interface methods in classes.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddImplementInterfaceFixes(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (!query.nodeIndex)
    {
        return;
    }

    const std::string_view strType = (query.config && !query.config->types.stringTypeName.empty())
                                         ? std::string_view(query.config->types.stringTypeName)
                                         : query.symbolTable.GetStringType();

    query.symbolTable.ForEachSymbolInFile(
        query.uri,
        [&query, &entries, strType]([[maybe_unused]] const std::string& qualifiedName,
                                    const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& clsSym : symbols)
            {
                if (clsSym.type != analysis::SymbolType::Class || clsSym.fileUri != query.uri)
                {
                    continue;
                }
                if (query.range.startLine > clsSym.endLine || query.range.endLine < clsSym.startLine)
                {
                    continue;
                }

                const auto missing = analysis::FindMissingInterfaceMethods(clsSym, query.symbolTable);
                ankerl::unordered_dense::map<std::string, std::vector<analysis::Symbol>> byInterface;
                TransparentStringSet blocked;
                for (const auto& entry : missing)
                {
                    if (!entry.canOverride)
                    {
                        blocked.insert(entry.interfaceName);
                    }
                    byInterface[entry.interfaceName].push_back(entry.method);
                }
                for (const auto& [cleanIface, methods] : byInterface)
                {
                    if (blocked.contains(cleanIface))
                    {
                        continue;
                    }
                    auto stubs = FormatMissingInterfaceMethodStubs(methods, strType);
                    if (stubs.empty())
                    {
                        continue;
                    }
                    uint32_t insertLine = clsSym.endLine;
                    uint32_t insertCharacter = clsSym.endCharacter;
                    if (const auto brace = analysis::FindClassOpenBracePoint(*query.nodeIndex, clsSym.startLine,
                                                                             clsSym.startCharacter))
                    {
                        insertLine = brace->line;
                        insertCharacter = brace->character;
                    }

                    CodeActionEntry codeAction;
                    codeAction.title = i18n::FormatMessage(query.i18n, "action-implement-interface",
                                                           "Implement missing interface methods for '{}'",
                                                           std::string_view(cleanIface));
                    codeAction.kind = CodeActionEntryKind::QuickFix;
                    codeAction.isPreferred = true;

                    auto matchingDiags = CollectMatchingInterfaceDiagnosticIndices(query.diagnostics, clsSym);
                    if (!matchingDiags.empty())
                    {
                        codeAction.diagnosticIndices = std::move(matchingDiags);
                    }

                    codeAction.edits = std::vector<CodeActionTextEdit>{CodeActionTextEdit{
                        analysis::SourceRange{insertLine, insertCharacter, insertLine, insertCharacter},
                        std::move(stubs)}};

                    entries.push_back(std::move(codeAction));
                }
            }
        });
}

} // namespace angel_lsp::analysis::code_action
