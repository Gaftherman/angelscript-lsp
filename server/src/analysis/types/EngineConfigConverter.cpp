#include "analysis/types/EngineConfigConverter.h"

#include "analysis/types/EngineConfigInternal.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace angel_lsp::analysis
{
bool IsOfficialEngineConfig(std::string_view content)
{
    if (content.empty())
        return false;
    const std::string_view sample = content.substr(0, std::min<size_t>(content.size(), 2048));
    if (sample.find("// AngelScript") != std::string_view::npos)
        return true;
    if (sample.starts_with("ep 0 ") || sample.find("\nep 0 ") != std::string_view::npos ||
        sample.starts_with("ep 1 ") || sample.find("\nep 1 ") != std::string_view::npos)
        return true;
    return (sample.starts_with("objtype \"") || sample.find("\nobjtype \"") != std::string_view::npos);
}

EngineConfigResult ConvertEngineConfigToPredefined(std::string_view configContent)
{
    EngineConfigResult result;
    std::vector<NamespaceData> scopes;
    std::string currNs;

    size_t lineStart = 0;
    while (lineStart < configContent.size())
    {
        const size_t lineEnd = std::min(configContent.find('\n', lineStart), configContent.size());
        const std::string_view line = configContent.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        const auto tok = engine_config::TokenizeLine(line);
        if (tok.empty())
            continue;

        if (tok[0] == "ep" && tok.size() >= 3)
        {
            int propId = 0;
            long long val = 0;
            std::from_chars(tok[1].data(), tok[1].data() + tok[1].size(), propId);
            std::from_chars(tok[2].data(), tok[2].data() + tok[2].size(), val);
            engine_config::ApplyEngineProperty(propId, val, result.engineProperties);
        }
        else if (tok[0] == "namespace" && tok.size() >= 2)
        {
            currNs = tok[1];
        }
        else
        {
            auto& scope = engine_config::FindOrCreateScope(scopes, currNs);
            engine_config::ProcessMemberDirective(tok, scope);
            engine_config::ProcessDeclarationDirective(tok, scope);
        }
    }

    result.predefinedContent = "// Predefined stub generated from AngelScript engine configuration dump\n\n";
    for (const auto& scope : scopes)
        engine_config::FormatNamespace(scope, result.predefinedContent);

    return result;
}
} // namespace angel_lsp::analysis
