#include "lsp/services/ServerDiagnosticFormatting.h"

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "lsp/services/ServerAnalysisTypes.h"

#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>

namespace angel_lsp
{
lsp::DiagnosticSeverity ToProtocolSeverity(analysis::DiagnosticSeverity severity)
{
    switch (severity)
    {
    case analysis::DiagnosticSeverity::Error:
        return lsp::DiagnosticSeverity::Error;
    case analysis::DiagnosticSeverity::Warning:
        return lsp::DiagnosticSeverity::Warning;
    case analysis::DiagnosticSeverity::Information:
        return lsp::DiagnosticSeverity::Information;
    case analysis::DiagnosticSeverity::Hint:
        return lsp::DiagnosticSeverity::Hint;
    }
    return lsp::DiagnosticSeverity::Error;
}

std::string FormatPrefixedDiagnosticMessage(analysis::DiagnosticSeverity severity, std::string_view displayCode,
                                            std::string_view rawMessage)
{
    if (displayCode.empty() || rawMessage.starts_with("["))
    {
        return std::string(rawMessage);
    }

    std::string prefix;
    switch (severity)
    {
    case analysis::DiagnosticSeverity::Error:
        prefix = fmt::format("[ERROR: {}] ", displayCode);
        break;
    case analysis::DiagnosticSeverity::Warning:
        prefix = fmt::format("[WARN: {}] ", displayCode);
        break;
    case analysis::DiagnosticSeverity::Hint:
        prefix = fmt::format("[HINT: {}] ", displayCode);
        break;
    default:
        prefix = fmt::format("[INFO: {}] ", displayCode);
        break;
    }
    return prefix + std::string(rawMessage);
}

std::string_view GetDiagnosticSettingsUri(std::string_view code)
{
    if (code.empty())
    {
        return {};
    }
    static constexpr CodeSettingPair kCodeUris[] = {
        {angel_lsp::diagnostics::codes::AccessorPortability,
         "vscode://settings/angelscript.diagnostics.reportAccessorPortability"},
        {"H402", "vscode://settings/angelscript.diagnostics.reportAccessorPortability"},
        {angel_lsp::diagnostics::codes::AccessorDisabled,
         "vscode://settings/angelscript.diagnostics.reportAccessorDisabled"},
        {"H403", "vscode://settings/angelscript.diagnostics.reportAccessorDisabled"},
        {angel_lsp::diagnostics::codes::BoolConversion,
         "vscode://settings/angelscript.diagnostics.reportBoolConversion"},
        {"H401", "vscode://settings/angelscript.diagnostics.reportBoolConversion"},
        {angel_lsp::diagnostics::codes::FuncdefMissing,
         "vscode://settings/angelscript.diagnostics.reportMissingFuncdef"},
        {"H404", "vscode://settings/angelscript.diagnostics.reportMissingFuncdef"},
        {angel_lsp::diagnostics::codes::IntegerDivision,
         "vscode://settings/angelscript.diagnostics.reportIntegerDivision"},
        {"H405", "vscode://settings/angelscript.diagnostics.reportIntegerDivision"},
        {angel_lsp::diagnostics::codes::PossibleNullDereference,
         "vscode://settings/angelscript.diagnostics.reportPossibleNullDereference"},
        {"W114", "vscode://settings/angelscript.diagnostics.reportPossibleNullDereference"},
        {angel_lsp::diagnostics::codes::HandleComparisonEquality,
         "vscode://settings/angelscript.diagnostics.reportHandleComparisonEquality"},
        {"W115", "vscode://settings/angelscript.diagnostics.reportHandleComparisonEquality"},
        {"W116", "vscode://settings/angelscript.diagnostics.missingAssetPathSeverity"},
        {angel_lsp::diagnostics::codes::UnusedFunction,
         "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {"W107", "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {angel_lsp::diagnostics::codes::UnusedMethod,
         "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {"W108", "vscode://settings/angelscript.diagnostics.reportUnusedFunctions"},
        {angel_lsp::diagnostics::codes::UnusedGlobalVariable,
         "vscode://settings/angelscript.diagnostics.reportUnusedGlobalVariables"},
        {"W117", "vscode://settings/angelscript.diagnostics.reportUnusedGlobalVariables"},
        {angel_lsp::diagnostics::codes::UnusedClass, "vscode://settings/angelscript.diagnostics.reportUnusedClasses"},
        {"W109", "vscode://settings/angelscript.diagnostics.reportUnusedClasses"},
        {angel_lsp::diagnostics::codes::UnusedField, "vscode://settings/angelscript.diagnostics.reportUnusedMembers"},
        {"W104", "vscode://settings/angelscript.diagnostics.reportUnusedMembers"},
        {angel_lsp::diagnostics::codes::UnknownType, "vscode://settings/angelscript.diagnostics.reportUnknownTypes"},
        {"E002", "vscode://settings/angelscript.diagnostics.reportUnknownTypes"},
    };
    for (const auto& entry : kCodeUris)
    {
        if (entry.code == code)
        {
            return entry.uri;
        }
    }
    return "vscode://settings/angelscript.diagnosticSeverity";
}
} // namespace angel_lsp
