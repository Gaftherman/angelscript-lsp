#include "analysis/documentation/HoverMarkdown.h"
#include "i18n/i18n.h"
#include "parser/Primitives.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

namespace angel_lsp::analysis
{
bool IsCallableSymbol(const Symbol& sym)
{
    return sym.type == SymbolType::Function || std::holds_alternative<FunctionSignature>(sym.signature);
}

size_t CountMoreOverloads(const std::vector<Symbol>& symbols)
{
    if (symbols.size() <= 1 || !IsCallableSymbol(symbols.front()))
    {
        return 0;
    }
    size_t count = 0;
    const std::string_view primaryName = symbols.front().name;
    for (size_t i = 1; i < symbols.size(); ++i)
    {
        if (symbols[i].name == primaryName && IsCallableSymbol(symbols[i]))
        {
            ++count;
        }
    }
    return count;
}

std::string UrlEncodeCommandArgs(std::string_view input)
{
    static constexpr char k_hexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(input.size() * 2);
    for (unsigned char ch : input)
    {
        const bool isUnreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                                  ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (isUnreserved)
        {
            encoded.push_back(static_cast<char>(ch));
        }
        else
        {
            encoded.push_back('%');
            encoded.push_back(k_hexDigits[(ch >> 4) & 0x0F]);
            encoded.push_back(k_hexDigits[ch & 0x0F]);
        }
    }
    return encoded;
}

std::string BuildPeekOverloadsLink(const std::vector<Symbol>& symbols, const HoverPeekContext& context)
{
    std::ostringstream json;
    json << "[{\"uri\":\"" << context.uri << "\",\"line\":" << context.line << ",\"character\":" << context.character
         << ",\"locations\":[";

    const std::string_view primaryName = symbols.front().name;
    bool first = true;
    for (const auto& sym : symbols)
    {
        if (sym.name != primaryName || !IsCallableSymbol(sym))
        {
            continue;
        }
        if (!first)
        {
            json << ",";
        }
        first = false;
        const std::string_view symUri = sym.fileUri.empty() ? context.uri : std::string_view(sym.fileUri);
        const uint32_t sLine = sym.selectionRange.startLine > 0 ? sym.selectionRange.startLine : sym.startLine;
        const uint32_t eLine = sym.selectionRange.endLine > 0 ? sym.selectionRange.endLine : sLine;
        json << "{\"uri\":\"" << symUri << "\",\"startLine\":" << sLine
             << ",\"startChar\":" << sym.selectionRange.startCharacter << ",\"endLine\":" << eLine
             << ",\"endChar\":" << sym.selectionRange.endCharacter << "}";
    }
    json << "]}]";

    const std::string label = angel_lsp::i18n::FormatMessage(context.i18n, "hover-peek-overloads", "Peek overloads");
    return "[" + label + "](command:angelscript.peekOverloads?" + UrlEncodeCommandArgs(json.str()) + ")";
}

std::string FormatMoreOverloads(const std::vector<Symbol>& symbols, size_t count, const HoverPeekContext& context)
{
    const std::string summaryText =
        (count == 1)
            ? angel_lsp::i18n::FormatMessage(context.i18n, "hover-more-overloads-singular", "1 more overload")
            : angel_lsp::i18n::FormatMessage(context.i18n, "hover-more-overloads-plural", "{} more overloads", count);

    std::ostringstream oss;
    oss << "**+" << summaryText << "**\n\n" << BuildPeekOverloadsLink(symbols, context);
    return oss.str();
}

std::string CollapseWhitespace(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    bool inSpace = false;
    for (char ch : text)
    {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
            if (!inSpace)
            {
                result.push_back(' ');
                inSpace = true;
            }
        }
        else
        {
            result.push_back(ch);
            inSpace = false;
        }
    }

    auto start = result.find_first_not_of(' ');
    if (start == std::string::npos)
    {
        return "";
    }
    auto end = result.find_last_not_of(' ');
    return result.substr(start, end - start + 1);
}

std::string FormatPrimitiveDoc(std::string_view typeName, const angel_lsp::i18n::I18n* i18n)
{
    auto info = parser::primitives::GetPrimitiveDocInfo(typeName);
    if (!info)
    {
        return {};
    }
    std::string key = "hover-primitive-desc-";
    key += typeName;
    std::string desc = angel_lsp::i18n::FormatMessage(i18n, key, std::string(info->description));
    if (info->range.empty())
    {
        return desc;
    }
    if (typeName == "bool")
    {
        return desc + "\n\n" +
               angel_lsp::i18n::FormatMessage(i18n, "hover-primitive-values", "Values: `{}`", info->range);
    }
    return desc + "\n\n" + angel_lsp::i18n::FormatMessage(i18n, "hover-primitive-range", "Range: `{}`", info->range);
}

std::string FormatFileSize(uintmax_t bytes)
{
    std::ostringstream ss;
    if (bytes < 1024)
    {
        ss << bytes << " B";
    }
    else if (bytes < 1024 * 1024)
    {
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << " KB";
    }
    else
    {
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    }
    return ss.str();
}

std::string FormatResolvedAssetMarkdown(std::string_view unquoted, std::string_view resolved, bool showLength,
                                        const angel_lsp::i18n::I18n* i18n)
{
    std::error_code ec;
    auto fsize = std::filesystem::file_size(std::string(resolved), ec);
    std::string sizeStr = ec ? "Unknown" : FormatFileSize(fsize);
    std::string normalizedPath = std::string(resolved);
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');

    std::string statusBullet =
        angel_lsp::i18n::FormatMessage(i18n, "hover-asset-status-exists", "- **Status**: Exists");
    std::string sizeBullet = angel_lsp::i18n::FormatMessage(i18n, "hover-asset-size", "- **Size**: {}", sizeStr);
    std::string pathBullet =
        angel_lsp::i18n::FormatMessage(i18n, "hover-asset-path", "- **Path**: `{}`", normalizedPath);

    std::string md =
        "*(asset)* `" + std::string(unquoted) + "`\n\n" + statusBullet + "\n" + sizeBullet + "\n" + pathBullet;
    if (showLength)
    {
        md += "\n" + angel_lsp::i18n::FormatMessage(i18n, "hover-string-length", "- **Length**: {} characters",
                                                    unquoted.size());
    }
    return md;
}
} // namespace angel_lsp::analysis
