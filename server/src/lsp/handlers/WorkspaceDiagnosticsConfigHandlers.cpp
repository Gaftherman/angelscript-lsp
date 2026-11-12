#include "lsp/Server.h"
#include "lsp/handlers/WorkspaceConfigHelpers.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace angel_lsp
{

bool UpdateEngineConfiguration(Server& server, const lsp::LSPObject& section)
{
    bool engineChanged = false;
    const lsp::LSPObject* engineObj = nullptr;
    if (const auto* e = section.find("engine"); e && e->isObject())
    {
        engineObj = &e->object();
    }

    auto applyBool = [&](std::string_view name, bool& target)
    {
        if (auto v = FindSectionBool(section, engineObj, name); v && target != *v)
        {
            target = *v;
            engineChanged = true;
        }
    };
    auto applyInt = [&](std::string_view name, int& target)
    {
        if (auto v = FindSectionInt(section, engineObj, name); v && target != *v)
        {
            target = *v;
            engineChanged = true;
        }
    };

    server.BindEngineConfiguration(applyBool, applyInt);

    return engineChanged;
}

static bool UpdateUnusedFunctionExclusions(const lsp::LSPObject& section, const lsp::LSPObject* diagObj,
                                           config::DiagnosticsConfig& diag)
{
    bool changed = false;
    if (auto ignored = FindSectionStringArray(section, diagObj, "ignoredUnusedFunctions", "diagnostics"))
    {
        std::unordered_set<std::string> next(ignored->begin(), ignored->end());
        if (diag.ignoredUnusedFunctions != next)
        {
            diag.ignoredUnusedFunctions = std::move(next);
            changed = true;
        }
    }
    if (auto patterns = FindSectionStringArray(section, diagObj, "ignoredUnusedFunctionPatterns", "diagnostics"))
    {
        if (diag.ignoredUnusedFunctionPatterns != *patterns)
        {
            diag.ignoredUnusedFunctionPatterns = std::move(*patterns);
            changed = true;
        }
    }
    return changed;
}

static void UpdateUnusedDiagnostics(const lsp::LSPObject& section, const lsp::LSPObject* diagObj,
                                    config::DiagnosticsConfig& diag, bool& changed)
{
    auto applyExplicitBool = [&](std::string_view name, bool& target, bool& explicitFlag)
    {
        if (auto v = FindSectionBool(section, diagObj, name, "diagnostics"))
        {
            explicitFlag = true;
            if (target != *v)
            {
                target = *v;
                changed = true;
            }
        }
    };

    applyExplicitBool("reportUnusedFunctions", diag.reportUnusedFunctions, diag.reportUnusedFunctionsExplicit);
    applyExplicitBool("reportUnusedGlobalVariables", diag.reportUnusedGlobalVariables,
                      diag.reportUnusedGlobalVariablesExplicit);
    applyExplicitBool("reportUnusedClasses", diag.reportUnusedClasses, diag.reportUnusedClassesExplicit);
    applyExplicitBool("reportUnusedMembers", diag.reportUnusedMembers, diag.reportUnusedMembersExplicit);
}

bool UpdateDiagnosticsConfiguration(Server& server, const lsp::LSPObject& section)
{
    bool diagnosticsChanged = false;
    const lsp::LSPObject* diagObj = nullptr;
    if (const auto* d = section.find("diagnostics"); d && d->isObject())
    {
        diagObj = &d->object();
    }

    auto& config = server.GetMutableConfig();

    auto applyBool = [&](std::string_view name, bool& target)
    {
        if (auto v = FindSectionBool(section, diagObj, name, "diagnostics"); v && target != *v)
        {
            target = *v;
            diagnosticsChanged = true;
        }
    };

    applyBool("reportUnknownTypes", config.diagnostics.reportUnknownTypes);
    applyBool("reportAccessorPortability", config.diagnostics.reportAccessorPortability);
    applyBool("reportAccessorDisabled", config.diagnostics.reportAccessorDisabled);
    applyBool("reportBoolConversion", config.diagnostics.reportBoolConversion);
    applyBool("reportMissingFuncdef", config.diagnostics.reportMissingFuncdef);
    applyBool("reportIntegerDivision", config.diagnostics.reportIntegerDivision);
    applyBool("reportPossibleNullDereference", config.diagnostics.reportPossibleNullDereference);
    applyBool("reportAllNullDereferences", config.diagnostics.reportAllNullDereferences);

    UpdateUnusedDiagnostics(section, diagObj, config.diagnostics, diagnosticsChanged);

    if (auto v = FindSectionInt(section, diagObj, "reportHandleComparisonEquality", "diagnostics");
        v && config.diagnostics.reportHandleComparisonEquality != *v)
    {
        config.diagnostics.reportHandleComparisonEquality = *v;
        diagnosticsChanged = true;
    }

    if (auto v = FindSectionString(section, diagObj, "missingAssetPathSeverity", "diagnostics");
        v && config.diagnostics.missingAssetPathSeverity != *v)
    {
        config.diagnostics.missingAssetPathSeverity = *v;
        diagnosticsChanged = true;
    }

    if (UpdateUnusedFunctionExclusions(section, diagObj, config.diagnostics))
    {
        diagnosticsChanged = true;
    }

    return diagnosticsChanged;
}

} // namespace angel_lsp
