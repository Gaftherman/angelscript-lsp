#include "analysis/resolution/IllegalHandleCheck.h"

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "config/EngineRuleConfig.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static std::optional<std::string> ExtractTypedefBase(std::string_view lookupName, const SymbolTable& table)
{
    const auto syms = table.FindSymbolsPtr(lookupName);
    if (!syms)
    {
        return std::nullopt;
    }
    for (const auto& sym : *syms)
    {
        if (sym.type == SymbolType::Typedef && std::holds_alternative<TypedefSignature>(sym.signature))
        {
            const std::string_view rawBase =
                utils::StripGlobalPrefix(CleanExpressionTypeView(sym.GetTypedef().baseType));
            if (!rawBase.empty())
            {
                return std::string(rawBase);
            }
        }
    }
    return std::nullopt;
}

static std::optional<std::string> ResolveNoHandleType(std::string_view typeName, std::string_view enclosingNs,
                                                      const SymbolTable& table,
                                                      const TransparentStringSet& noHandleTypes)
{
    if (noHandleTypes.empty() || typeName.empty())
    {
        return std::nullopt;
    }

    std::string currentStorage;
    std::string_view current = utils::StripGlobalPrefix(typeName);
    constexpr int k_maxTypedefSteps = 8;
    for (int depth = 0; depth < k_maxTypedefSteps; ++depth)
    {
        if (noHandleTypes.contains(current))
        {
            return std::string(current);
        }

        std::optional<std::string> nextType = ExtractTypedefBase(current, table);
        if (!nextType && !enclosingNs.empty() && !HasScopeQualifier(current))
        {
            std::optional<std::string> scopedMatch;
            utils::ForEachEnclosingScope(enclosingNs,
                                         [&](std::string_view scope)
                                         {
                                             if (const auto scopedSyms = table.FindMemberSymbolPtr(scope, current))
                                             {
                                                 for (const auto& sym : *scopedSyms)
                                                 {
                                                     if (!sym.containerName.empty())
                                                     {
                                                         std::string qualified =
                                                             utils::JoinScopedName(sym.containerName, sym.name);
                                                         const std::string_view qualifiedView(qualified);
                                                         if (noHandleTypes.contains(qualifiedView))
                                                         {
                                                             scopedMatch = std::move(qualified);
                                                             return true;
                                                         }
                                                     }
                                                     if (sym.type == SymbolType::Typedef &&
                                                         std::holds_alternative<TypedefSignature>(sym.signature))
                                                     {
                                                         const std::string_view rawBase = utils::StripGlobalPrefix(
                                                             CleanExpressionTypeView(sym.GetTypedef().baseType));
                                                         if (!rawBase.empty())
                                                         {
                                                             nextType = std::string(rawBase);
                                                             return true;
                                                         }
                                                     }
                                                 }
                                             }
                                             return false;
                                         });
            if (scopedMatch)
            {
                return scopedMatch;
            }
        }

        if (!nextType || *nextType == current)
        {
            break;
        }
        currentStorage = std::move(*nextType);
        current = currentStorage;
    }

    return std::nullopt;
}

static std::string_view ReadQualifiedTypeIdent(std::string_view sv, std::string& outCompact, std::string_view& outRest)
{
    size_t i = 0;
    const size_t n = sv.size();
    bool hasWhitespace = false;

    const auto skipWs = [&]()
    {
        while (i < n && (sv[i] == ' ' || sv[i] == '\t' || sv[i] == '\r' || sv[i] == '\n'))
        {
            hasWhitespace = true;
            ++i;
        }
    };

    if (i + 1 < n && sv[i] == ':' && sv[i + 1] == ':')
    {
        i += 2;
        skipWs();
    }

    const auto readIdent = [&]() -> bool
    {
        if (i >= n)
        {
            return false;
        }
        const unsigned char firstChar = static_cast<unsigned char>(sv[i]);
        if (!std::isalpha(firstChar) && firstChar != '_')
        {
            return false;
        }
        ++i;
        while (i < n)
        {
            const unsigned char ch = static_cast<unsigned char>(sv[i]);
            if (!std::isalnum(ch) && ch != '_')
            {
                break;
            }
            ++i;
        }
        return true;
    };

    if (!readIdent())
    {
        outRest = sv;
        return {};
    }

    while (true)
    {
        const size_t saved = i;
        const bool savedWs = hasWhitespace;
        skipWs();
        if (i + 1 < n && sv[i] == ':' && sv[i + 1] == ':')
        {
            i += 2;
            skipWs();
            if (readIdent())
            {
                continue;
            }
        }
        i = saved;
        hasWhitespace = savedWs;
        break;
    }

    const std::string_view raw = sv.substr(0, i);
    outRest = sv.substr(i);
    if (!hasWhitespace)
    {
        return raw;
    }
    outCompact = utils::CompactScopedName(raw);
    return outCompact;
}

static bool StartsWithConstToken(std::string_view sv) noexcept
{
    if (!sv.starts_with("const"))
    {
        return false;
    }
    if (sv.size() == 5)
    {
        return true;
    }
    const unsigned char next = static_cast<unsigned char>(sv[5]);
    return !std::isalnum(next) && next != '_';
}

static void PushTemplateArgViews(std::string_view inner, std::vector<std::string_view>& worklist)
{
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < inner.size(); ++i)
    {
        if (inner[i] == '<')
        {
            ++depth;
        }
        else if (inner[i] == '>')
        {
            --depth;
        }
        else if (inner[i] == ',' && depth == 0)
        {
            worklist.push_back(inner.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start <= inner.size())
    {
        worklist.push_back(inner.substr(start));
    }
}

IllegalHandleCheckResult CheckIllegalHandleType(const IllegalHandleCheckRequest& req, const DiagnosticContext& ctx)
{
    if (req.isAddressOfOperand)
    {
        if (req.baseTypeName.empty() || parser::primitives::IsAutoType(req.baseTypeName))
        {
            return {};
        }
        if (IsPrimitiveTypeName(req.baseTypeName))
        {
            return {diagnostics::codes::HandleOnPrimitive, std::string(req.baseTypeName)};
        }
        if (ctx.request.engineRules && !ctx.request.engineRules->noHandleTypes.empty())
        {
            if (auto matched = ResolveNoHandleType(req.baseTypeName, req.enclosingNs, ctx.request.symbolTable,
                                                   ctx.request.engineRules->noHandleTypes))
            {
                return {diagnostics::codes::HandleOnNoHandleType, std::move(*matched)};
            }
        }
        return {};
    }

    if (req.checkPrimitiveFlag && req.hasPrimitiveHandle && !parser::primitives::IsAutoType(req.baseTypeName))
    {
        return {diagnostics::codes::HandleOnPrimitive, std::string(req.baseTypeName)};
    }

    if (!ctx.request.engineRules || ctx.request.engineRules->noHandleTypes.empty())
    {
        return {};
    }

    const TransparentStringSet& noHandleTypes = ctx.request.engineRules->noHandleTypes;
    std::vector<std::string_view> worklist;
    if (!req.writtenType.empty())
    {
        worklist.push_back(req.writtenType);
    }

    std::string compactBuf;
    while (!worklist.empty())
    {
        std::string_view sv = worklist.back();
        worklist.pop_back();

        TrimTypeWhitespace(sv);
        if (const size_t refLen = TrailingReferenceModifierLength(sv); refLen > 0)
        {
            sv.remove_suffix(refLen);
            TrimTypeWhitespace(sv);
        }
        while (StartsWithConstToken(sv))
        {
            sv.remove_prefix(5);
            TrimTypeWhitespace(sv);
        }
        if (sv.empty())
        {
            continue;
        }

        std::string_view rest;
        const std::string_view baseName = ReadQualifiedTypeIdent(sv, compactBuf, rest);
        if (baseName.empty())
        {
            continue;
        }
        TrimTypeWhitespace(rest);

        if (rest.starts_with('<'))
        {
            int angleDepth = 0;
            size_t closePos = std::string_view::npos;
            for (size_t k = 0; k < rest.size(); ++k)
            {
                if (rest[k] == '<')
                {
                    ++angleDepth;
                }
                else if (rest[k] == '>')
                {
                    --angleDepth;
                    if (angleDepth == 0)
                    {
                        closePos = k;
                        break;
                    }
                }
            }
            if (closePos != std::string_view::npos)
            {
                PushTemplateArgViews(rest.substr(1, closePos - 1), worklist);
                rest = rest.substr(closePos + 1);
                TrimTypeWhitespace(rest);
            }
            else
            {
                continue;
            }
        }

        while (StartsWithConstToken(rest))
        {
            rest.remove_prefix(5);
            TrimTypeWhitespace(rest);
        }

        if (rest.starts_with('@') && !parser::primitives::IsAutoType(baseName))
        {
            if (auto matched = ResolveNoHandleType(baseName, req.enclosingNs, ctx.request.symbolTable, noHandleTypes))
            {
                return {diagnostics::codes::HandleOnNoHandleType, std::move(*matched)};
            }
        }
    }

    return {};
}

} // namespace angel_lsp::analysis
