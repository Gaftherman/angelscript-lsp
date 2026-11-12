#include "lsp/Server.h"
#include "lsp/handlers/WorkspaceConfigHelpers.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp
{

static void UpdateInlayHintDisplayConfig(const lsp::LSPObject& section, const lsp::LSPObject* ihObj,
                                         config::ServerConfig& cfg)
{
    if (auto maxParam = FindSectionInt(section, ihObj, "maxParameters", "inlayHints"); maxParam.has_value())
    {
        cfg.features.inlayHintsMaxParameters = static_cast<size_t>(std::max(0, *maxParam));
    }
    if (auto maxLen = FindSectionInt(section, ihObj, "maxLength", "inlayHints"); maxLen.has_value())
    {
        cfg.features.inlayHintsMaxLength = static_cast<size_t>(std::max(0, *maxLen));
    }
    if (auto suppress = FindSectionBool(section, ihObj, "suppressWhenArgumentMatchesName", "inlayHints");
        suppress.has_value())
    {
        cfg.features.inlayHintsSuppressWhenArgumentMatchesName = *suppress;
    }
    if (auto tooltip = FindSectionBool(section, ihObj, "enableTooltip", "inlayHints"); tooltip.has_value())
    {
        cfg.features.inlayHintsEnableTooltip = *tooltip;
    }
    if (auto loc = FindSectionBool(section, ihObj, "enableLocation", "inlayHints"); loc.has_value())
    {
        cfg.features.inlayHintsEnableLocation = *loc;
    }
    if (auto omitted = FindSectionString(section, ihObj, "omittedDefaultArguments", "inlayHints"); omitted.has_value())
    {
        if (*omitted == "off" || *omitted == "false")
        {
            cfg.features.inlayHintsOmittedDefaultArguments = config::OmittedDefaultArgumentsMode::Off;
        }
        else if (*omitted == "declaration")
        {
            cfg.features.inlayHintsOmittedDefaultArguments = config::OmittedDefaultArgumentsMode::Declaration;
        }
        else
        {
            cfg.features.inlayHintsOmittedDefaultArguments = config::OmittedDefaultArgumentsMode::NameAndValue;
        }
    }
}

static void UpdateInlayHintKindConfig(const lsp::LSPObject& section, const lsp::LSPObject* ihObj,
                                      config::ServerConfig& cfg)
{
    if (auto params = FindSectionBool(section, ihObj, "parameters", "inlayHints"); params.has_value())
    {
        cfg.features.inlayHintsParameters = *params;
    }
    else if (auto paramNames = FindSectionBool(section, ihObj, "parameterNames", "inlayHints"); paramNames.has_value())
    {
        cfg.features.inlayHintsParameters = *paramNames;
    }
    if (auto varTypes = FindSectionBool(section, ihObj, "variableTypes", "inlayHints"); varTypes.has_value())
    {
        cfg.features.inlayHintsVariableTypes = *varTypes;
    }
    if (auto fnRetTypes = FindSectionBool(section, ihObj, "functionReturnTypes", "inlayHints"); fnRetTypes.has_value())
    {
        cfg.features.inlayHintsFunctionReturnTypes = *fnRetTypes;
    }
    if (auto enumVals = FindSectionBool(section, ihObj, "enumMemberValues", "inlayHints"); enumVals.has_value())
    {
        cfg.features.inlayHintsEnumMemberValues = *enumVals;
    }
    if (auto refMods = FindSectionBool(section, ihObj, "parameterReferenceModifiers", "inlayHints");
        refMods.has_value())
    {
        cfg.features.inlayHintsParameterReferenceModifiers = *refMods;
    }
}

static void UpdateInlayHintFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* ihObj = nullptr;
    if (const auto* ihVal = section.find("inlayHints"); ihVal && ihVal->isObject())
    {
        ihObj = &ihVal->object();
    }
    UpdateInlayHintDisplayConfig(section, ihObj, cfg);
    UpdateInlayHintKindConfig(section, ihObj, cfg);
}

static void UpdateHoverFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* hoverObj = nullptr;
    if (const auto* hoverVal = section.find("hover"); hoverVal && hoverVal->isObject())
    {
        hoverObj = &hoverVal->object();
    }
    if (auto len = FindSectionBool(section, hoverObj, "stringLiteralLength", "hover"); len.has_value())
    {
        cfg.features.hoverStringLiteralLength = *len;
    }
    if (auto pathRes = FindSectionBool(section, hoverObj, "stringLiteralPathResolution", "hover"); pathRes.has_value())
    {
        cfg.features.hoverStringLiteralPathResolution = *pathRes;
    }
    if (auto showLoc = FindSectionBool(section, hoverObj, "showFileLocation", "hover"); showLoc.has_value())
    {
        cfg.features.hoverShowFileLocation = *showLoc;
    }
    if (hoverObj)
    {
        if (const auto* paths = hoverObj->find("assetSearchPaths"); paths && paths->isArray())
        {
            cfg.features.assetSearchPaths.clear();
            for (const auto& item : paths->array())
            {
                if (item.isString() && !item.string().empty())
                {
                    cfg.features.assetSearchPaths.push_back(item.string());
                }
            }
        }
    }
}

static void UpdateCodeLensFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* clObj = nullptr;
    if (const auto* clVal = section.find("codeLens"); clVal && clVal->isObject())
    {
        clObj = &clVal->object();
    }
    if (auto enabled = FindSectionBool(section, clObj, "enabled", "codeLens"); enabled.has_value())
    {
        cfg.features.enableCodeLens = *enabled;
    }
    if (auto showFn = FindSectionBool(section, clObj, "showOnFunctions", "codeLens"); showFn.has_value())
    {
        cfg.features.codeLensShowOnFunctions = *showFn;
    }
    if (auto showCls = FindSectionBool(section, clObj, "showOnClassesAndInterfaces", "codeLens"); showCls.has_value())
    {
        cfg.features.codeLensShowOnClassesAndInterfaces = *showCls;
    }
    if (auto showProps = FindSectionBool(section, clObj, "showOnPropertiesAndVariables", "codeLens");
        showProps.has_value())
    {
        cfg.features.codeLensShowOnPropertiesAndVariables = *showProps;
    }
    if (auto showEnums = FindSectionBool(section, clObj, "showOnEnums", "codeLens"); showEnums.has_value())
    {
        cfg.features.codeLensShowOnEnums = *showEnums;
    }
    if (auto showZero = FindSectionBool(section, clObj, "showZeroReferences", "codeLens"); showZero.has_value())
    {
        cfg.features.codeLensShowZeroReferences = *showZero;
    }
    if (auto showMixin = FindSectionBool(section, clObj, "showMixinExpansion", "codeLens"); showMixin.has_value())
    {
        cfg.features.codeLensShowMixinExpansion = *showMixin;
    }
}

static void UpdateMetadataFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* metaObj = nullptr;
    if (const auto* metaVal = section.find("metadata"); metaVal && metaVal->isObject())
    {
        metaObj = &metaVal->object();
    }
    if (auto meta = FindSectionBool(section, metaObj, "enabled", "metadata"); meta.has_value())
    {
        cfg.features.enableMetadata = *meta;
    }
}

static void UpdateCompletionFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* compObj = nullptr;
    if (const auto* compVal = section.find("completion"); compVal && compVal->isObject())
    {
        compObj = &compVal->object();
    }
    if (auto smart = FindSectionBool(section, compObj, "smartTypeRanking", "completion"); smart.has_value())
    {
        cfg.features.completionSmartTypeRanking = *smart;
    }
    if (auto parens = FindSectionBool(section, compObj, "completeFunctionParens", "completion"); parens.has_value())
    {
        cfg.features.completionCompleteFunctionParens = *parens;
    }
    if (auto qualify = FindSectionBool(section, compObj, "qualifyEnumValues", "completion"); qualify.has_value())
    {
        cfg.features.completionQualifyEnumValues = *qualify;
    }
}

void UpdateFeatureConfiguration(Server& server, const lsp::LSPObject& section)
{
    const lsp::LSPObject* featObj = nullptr;
    if (const auto* featVal = section.find("features"); featVal && featVal->isObject())
    {
        featObj = &featVal->object();
    }
    auto& config = server.GetMutableConfig();
    if (auto vmd = FindSectionBool(section, featObj, "enableVirtualMixinDocuments", "features"); vmd.has_value())
    {
        config.features.enableVirtualMixinDocuments = *vmd;
        server.GetMutableSymbolTable().SetVirtualMixinDocumentsEnabled(config.features.enableVirtualMixinDocuments);
        server.GetMutableSymbolTable().SetConfiguredTypes(config.types.arrayTypeName, config.types.stringTypeName);
    }
    if (auto ih = FindSectionBool(section, featObj, "inlayHints", "features"); ih.has_value())
    {
        config.features.enableInlayHints = *ih;
    }
    if (auto fmt = FindSectionBool(section, featObj, "formatting", "features"); fmt.has_value())
    {
        config.features.enableFormatting = *fmt;
    }
    if (auto otf = FindSectionBool(section, featObj, "onTypeFormatting", "features"); otf.has_value())
    {
        config.features.enableOnTypeFormatting = *otf;
    }
    if (auto cs = FindSectionBool(section, featObj, "commentSuppressions", "features"); cs.has_value())
    {
        config.features.enableCommentSuppressions = *cs;
    }

    UpdateMetadataFeatureConfig(section, config);
    UpdateCompletionFeatureConfig(section, config);
    UpdateInlayHintFeatureConfig(section, config);
    UpdateHoverFeatureConfig(section, config);
    UpdateCodeLensFeatureConfig(section, config);
}

} // namespace angel_lsp
