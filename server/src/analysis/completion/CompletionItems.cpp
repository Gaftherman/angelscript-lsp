#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

bool IsConstructorOrDestructor(const analysis::Symbol& sym, std::string_view typeName)
{
    if (sym.type != analysis::SymbolType::Function)
    {
        return false;
    }

    const std::string_view shortName = analysis::LastScopeSegment(typeName);

    if (sym.name == shortName)
    {
        return true;
    }

    return !sym.name.empty() && sym.name.front() == '~' && std::string_view(sym.name).substr(1) == shortName;
}

std::string FormatMethodDetail(const analysis::Symbol& sym, const analysis::TemplateBinding& binding,
                               const std::vector<std::string>& templateArgs)
{
    if (sym.type != analysis::SymbolType::Function)
    {
        return "";
    }
    analysis::Symbol substitutedSym = sym;
    auto fn = sym.GetFunction();
    if (binding.usable)
    {
        for (size_t i = 0; i < binding.parameters.size(); ++i)
        {
            fn.returnType = analysis::SubstituteTypeParam(fn.returnType, binding.parameters[i], binding.arguments[i]);
            for (auto& param : fn.parameters)
            {
                param.typeName =
                    analysis::SubstituteTypeParam(param.typeName, binding.parameters[i], binding.arguments[i]);
                param.baseTypeName =
                    analysis::SubstituteTypeParam(param.baseTypeName, binding.parameters[i], binding.arguments[i]);
            }
        }
    }
    else if (!templateArgs.empty())
    {
        fn.returnType = analysis::SubstituteTypeParam(fn.returnType, "T", templateArgs[0]);
        for (auto& param : fn.parameters)
        {
            param.typeName = analysis::SubstituteTypeParam(param.typeName, "T", templateArgs[0]);
            param.baseTypeName = analysis::SubstituteTypeParam(param.baseTypeName, "T", templateArgs[0]);
        }
    }
    substitutedSym.signature = fn;
    return analysis::FormatFunctionDeclaration(substitutedSym, false);
}

std::string FormatProximitySortText(SymbolProximity prox, std::string_view label)
{
    static constexpr std::array<std::string_view, 6> k_proxPrefixes = {"0_", "1_", "2_", "3_", "4_", "5_"};
    std::string_view underscoreTier = "0_";
    if (label.starts_with("__"))
    {
        underscoreTier = "2_";
    }
    else if (label.starts_with('_'))
    {
        underscoreTier = "1_";
    }

    const size_t idx = static_cast<size_t>(prox);
    const std::string_view proxPrefix = (idx < k_proxPrefixes.size()) ? k_proxPrefixes[idx] : "9_";

    std::string result;
    result.reserve(proxPrefix.size() + underscoreTier.size() + label.size());
    result.append(proxPrefix);
    result.append(underscoreTier);
    result.append(label);
    return result;
}

SymbolProximity DetermineSymbolProximity(const analysis::Symbol& sym, std::string_view currentUri,
                                         std::string_view predefinedExt)
{
    if (!currentUri.empty() && sym.fileUri == currentUri)
    {
        return SymbolProximity::CurrentFile;
    }
    if (utils::IsPredefinedFile(sym.fileUri, predefinedExt))
    {
        return SymbolProximity::Predefined;
    }
    return SymbolProximity::ExternalFile;
}

std::string_view GetPredefinedExtension(const CompletionQuery& request)
{
    return request.config ? std::string_view(request.config->info.predefinedFileExtension)
                          : std::string_view(".as.predefined");
}

void AddItemIfNew(CompletionCollector& collector, CompletionCandidate candidate)
{
    std::string dedupeKey = candidate.label;
    if (candidate.kind == CompletionEntryKind::EnumMember && !candidate.detail.empty())
    {
        dedupeKey += "@" + candidate.detail;
    }
    if (candidate.label.empty() || collector.seenLabels.contains(dedupeKey))
    {
        return;
    }
    collector.seenLabels.insert(std::move(dedupeKey));

    CompletionEntry item;
    item.label = std::move(candidate.label);
    item.kind = candidate.kind;
    if (!candidate.insertText.empty())
    {
        item.insertText = std::move(candidate.insertText);
    }
    else if (!candidate.snippet.empty())
    {
        item.insertText = std::move(candidate.snippet);
        item.isSnippet = true;
    }
    if (!candidate.sortText.empty())
    {
        item.sortText = std::move(candidate.sortText);
    }
    else
    {
        item.sortText = FormatProximitySortText(SymbolProximity::ExternalFile, item.label);
    }
    if (!candidate.detail.empty())
    {
        item.detail = std::move(candidate.detail);
    }
    if (!candidate.doc.empty())
    {
        item.documentation = std::move(candidate.doc);
    }
    if (!candidate.resolveKey.empty())
    {
        item.resolveKey = std::move(candidate.resolveKey);
    }
    collector.items.push_back(std::move(item));
}

std::string CallSnippet(std::string_view name, const std::vector<analysis::ParameterInformation>& params)
{
    if (name.empty())
    {
        return {};
    }

    std::string snippet(name);
    snippet += "(";
    for (size_t i = 0; i < params.size(); ++i)
    {
        if (i > 0)
        {
            snippet += ", ";
        }

        std::string label = params[i].typeName;
        if (!params[i].name.empty())
        {
            label += label.empty() ? params[i].name : " " + params[i].name;
        }
        if (label.empty())
        {
            label = "arg" + std::to_string(i + 1);
        }

        std::string escaped;
        for (char c : label)
        {
            if (c == '}' || c == '$' || c == '\\')
            {
                escaped += '\\';
            }
            escaped += c;
        }

        snippet += "${" + std::to_string(i + 1) + ":" + escaped + "}";
    }
    snippet += ")$0";
    return snippet;
}

std::vector<std::string> GetPrimitiveTypeNames()
{
    std::vector<std::string> all;
    all.reserve(parser::primitives::k_all.size());
    for (const std::string_view name : parser::primitives::k_all)
    {
        all.emplace_back(name);
    }
    return all;
}

std::string TemplateInsertSnippet(const analysis::Symbol& sym)
{
    if (sym.type != analysis::SymbolType::Class || !std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        return {};
    }

    const auto& cls = std::get<analysis::ClassSignature>(sym.signature);
    if (!cls.isTemplate || cls.templateParams.empty())
    {
        return {};
    }

    std::string snippet = sym.name + "<";
    for (size_t i = 0; i < cls.templateParams.size(); ++i)
    {
        if (i > 0)
        {
            snippet += ", ";
        }
        snippet += "${" + std::to_string(i + 1) + ":" + cls.templateParams[i] + "}";
    }
    snippet += ">$0";
    return snippet;
}

std::string CallSnippetForName(std::string_view name, const analysis::SymbolTable& table)
{
    const auto symbols = table.FindSymbolsPtr(name);
    if (!symbols)
    {
        return {};
    }
    const analysis::Symbol* only = nullptr;
    for (const auto& sym : *symbols)
    {
        if (sym.type != analysis::SymbolType::Function)
        {
            continue;
        }
        if (only != nullptr)
        {
            return {};
        }
        only = &sym;
    }

    return only == nullptr ? std::string{} : CallSnippet(only->name, only->GetFunction().parameters);
}

std::string TemplateSnippetForName(std::string_view name, const analysis::SymbolTable& table)
{
    if (const auto symbols = table.FindSymbolsPtr(name))
    {
        for (const auto& sym : *symbols)
        {
            std::string snippet = TemplateInsertSnippet(sym);
            if (!snippet.empty())
            {
                return snippet;
            }
        }
    }
    return {};
}

const std::vector<std::string>& GetKeywords()
{
    static const std::vector<std::string> keywords = []
    {
        std::vector<std::string> all;
        all.reserve(parser::keywords::k_reserved.size() + parser::keywords::k_contextual.size() + 1);
        for (const std::string_view word : parser::keywords::k_reserved)
        {
            all.emplace_back(word);
        }
        for (const std::string_view word : parser::keywords::k_contextual)
        {
            all.emplace_back(word);
        }
        all.emplace_back("string");
        return all;
    }();
    return keywords;
}

} // namespace angel_lsp::analysis::completion
