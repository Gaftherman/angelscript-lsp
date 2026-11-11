#include "lsp/Server.h"
#include "lsp/services/ServerAnalysisTypes.h"
#include "utils/Utils.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{

static VirtualMixinTarget ParseVirtualMixinUri(std::string_view uri)
{
    std::string_view s = uri;
    static constexpr std::string_view kVirtualSchemeFull = "angelscript-virtual://";
    static constexpr std::string_view kVirtualSchemeShort = "angelscript-virtual:";

    if (s.starts_with(kVirtualSchemeFull))
    {
        s.remove_prefix(kVirtualSchemeFull.size());
    }
    else if (s.starts_with(kVirtualSchemeShort))
    {
        s.remove_prefix(kVirtualSchemeShort.size());
        while (!s.empty() && s.front() == '/')
        {
            s.remove_prefix(1);
        }
    }

    VirtualMixinTarget target;
    auto slashPos = s.find('/');
    std::string_view mixinPart = (slashPos != std::string_view::npos) ? s.substr(slashPos + 1) : s;
    if (slashPos != std::string_view::npos)
    {
        target.hostClass = angel_lsp::utils::UrlDecode(s.substr(0, slashPos));
    }
    if (mixinPart.ends_with(".as"))
    {
        mixinPart.remove_suffix(3);
    }
    target.mixinName = angel_lsp::utils::UrlDecode(mixinPart);
    return target;
}

static const angel_lsp::analysis::Symbol* FindMixinClassSymbol(const angel_lsp::analysis::SymbolTable& symTable,
                                                               const std::string& mixinName,
                                                               std::vector<angel_lsp::analysis::Symbol>& outCandidates)
{
    outCandidates = symTable.FindSymbols(mixinName);
    for (const auto& cand : outCandidates)
    {
        if (cand.type == angel_lsp::analysis::SymbolType::Class)
        {
            return &cand;
        }
    }

    std::string shortName = std::string(angel_lsp::analysis::LastScopeSegment(mixinName));
    auto shortCandidates = symTable.FindTypeSymbolsByShortName(shortName);
    for (const auto& cand : shortCandidates)
    {
        if (cand.type == angel_lsp::analysis::SymbolType::Class)
        {
            return &cand;
        }
    }
    return nullptr;
}

static MixinSymbolLocation ResolveMixinLocation(const angel_lsp::analysis::Symbol* mixinSym)
{
    MixinSymbolLocation loc;
    if (!mixinSym)
    {
        return loc;
    }
    loc.fileUri = mixinSym->fileUri;
    loc.startLine = mixinSym->startLine;
    loc.endLine = mixinSym->fullRange.endLine > 0 ? mixinSym->fullRange.endLine : mixinSym->endLine;
    loc.found = true;

    if (std::holds_alternative<angel_lsp::analysis::ClassSignature>(mixinSym->signature))
    {
        const auto& clsSig = std::get<angel_lsp::analysis::ClassSignature>(mixinSym->signature);
        const std::vector<std::string> bases = clsSig.bases;
        if (!bases.empty())
        {
            loc.resolvedBaseClass = fmt::format(" | Base: {}", fmt::join(bases, ", "));
        }
    }
    return loc;
}

static void AppendMixinSourceLines(std::string_view sourceText, uint32_t startLine, uint32_t endLine, std::string& out)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < sourceText.size())
    {
        size_t end = sourceText.find('\n', start);
        if (end == std::string_view::npos)
        {
            lines.push_back(std::string_view(sourceText).substr(start));
            break;
        }
        std::string_view line = std::string_view(sourceText).substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }

    if (startLine < lines.size())
    {
        size_t effectiveEndLine = std::max<size_t>(startLine, endLine);
        size_t clampEnd = std::min<size_t>(effectiveEndLine, lines.size() - 1);
        for (size_t i = startLine; i <= clampEnd; ++i)
        {
            out.append(lines[i]);
            out.push_back('\n');
        }
    }
}

static void AppendFallbackMixin(std::string_view mixinName, const std::vector<angel_lsp::analysis::Symbol>& candidates,
                                std::string& out)
{
    out += fmt::format("mixin class {}\n{{\n", mixinName);
    for (const auto& cand : candidates)
    {
        if (cand.type == angel_lsp::analysis::SymbolType::Function && cand.containerName == mixinName)
        {
            out += fmt::format("    void {}();\n", cand.name);
        }
    }
    out += "}\n";
}

std::string Server::GenerateVirtualMixinDocument(std::string_view uri)
{
    const auto target = ParseVirtualMixinUri(uri);
    std::vector<angel_lsp::analysis::Symbol> candidates;
    const auto* mixinSym = FindMixinClassSymbol(m_symbolTable, target.mixinName, candidates);
    const auto loc = ResolveMixinLocation(mixinSym);

    static_assert(angel_lsp::analysis::SymbolTable::kVirtualMixinHeaderLineCount == 3);
    std::string result;
    result += fmt::format("// Virtual expanded mixin {} for host class {}\n", target.mixinName, target.hostClass);
    result += fmt::format("// Origin: {}{}\n\n", loc.fileUri, loc.resolvedBaseClass);

    std::string sourceText;
    if (!loc.fileUri.empty())
    {
        if (auto docText = FindDocumentText(loc.fileUri))
        {
            sourceText = *docText;
        }
        else
        {
            std::string filePath = angel_lsp::utils::UriToPath(loc.fileUri);
            std::ifstream file(filePath, std::ios::binary);
            if (file.is_open())
            {
                std::ostringstream ss;
                ss << file.rdbuf();
                sourceText = ss.str();
            }
        }
    }

    if (loc.found && !sourceText.empty())
    {
        AppendMixinSourceLines(sourceText, loc.startLine, loc.endLine, result);
    }
    else
    {
        AppendFallbackMixin(target.mixinName, candidates, result);
    }

    return result;
}

lsp::json::Value Server::HandleRequestsVirtualDocumentContent(lsp::json::Value&& params)
{
    std::string uriStr;
    if (params.isObject())
    {
        const auto& obj = params.object();
        if (const auto* val = obj.find("uri"); val && val->isString())
        {
            uriStr = val->string();
        }
    }
    else if (params.isString())
    {
        uriStr = params.string();
    }

    if (uriStr.empty())
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams, "Missing 'uri' parameter");
    }

    std::string content = GenerateVirtualMixinDocument(uriStr);

    lsp::json::Object answer;
    answer["content"] = lsp::json::Value(std::move(content));
    return lsp::json::Value(std::move(answer));
}

} // namespace angel_lsp
