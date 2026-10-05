#include "features/completion/CompletionHandler.h"
#include "analysis/DocComment.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SignatureFormatter.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
namespace
{

using analysis::CanonicalizeArrayType;

/**
 * @brief True when a member is its class's constructor or destructor.
 * @param[in] sym Candidate symbol.
 * @param[in] typeName Container type name.
 * @return True if symbol is a constructor or destructor.
 */
bool IsConstructorOrDestructor(const analysis::Symbol& sym, const std::string& typeName)
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

/**
 * @brief Formats function signature with substituted template arguments.
 * @param[in] sym Function symbol.
 * @param[in] binding Deduced template bindings.
 * @param[in] templateArgs Explicit template argument names.
 * @return Formatted declaration string.
 */
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

/**
 * @brief Collects immediate base classes and interfaces for a given type name.
 * @param[in] symbolTable Global symbol table.
 * @param[in] currentTypeName Type name to inspect.
 * @return Vector of base type names.
 */
std::vector<std::string> CollectDirectBases(const analysis::SymbolTable& symbolTable,
                                            const std::string& currentTypeName)
{
    std::vector<std::string> bases;
    auto symsPtr = symbolTable.FindSymbolsPtr(currentTypeName);
    std::vector<analysis::Symbol> fallbackSyms;
    if ((!symsPtr || symsPtr->empty()) && currentTypeName.find("::") == std::string::npos)
    {
        fallbackSyms = symbolTable.FindTypeSymbolsByShortName(currentTypeName);
    }
    const auto& syms = (symsPtr && !symsPtr->empty()) ? *symsPtr : fallbackSyms;
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::Class)
        {
            const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            if (qName != currentTypeName)
            {
                bases.push_back(qName);
            }
            const auto& classSig = sym.GetClass();
            for (const auto& baseName : classSig.bases)
            {
                bases.push_back(baseName);
            }
        }
        else if (sym.type == analysis::SymbolType::Interface)
        {
            const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            if (qName != currentTypeName)
            {
                bases.push_back(qName);
            }
            const auto& ifaceSig = sym.GetInterface();
            for (const auto& baseName : ifaceSig.inheritedInterfaces)
            {
                bases.push_back(baseName);
            }
        }
    }
    return bases;
}

/**
 * @brief Collects the class and interface inheritance hierarchy using flat worklist traversal.
 * @param[in] symbolTable Global symbol table.
 * @param[in] initialTypeName Starting type name.
 * @return Vector of type names in the hierarchy.
 */
std::vector<std::string> GetInheritedTypeHierarchy(const analysis::SymbolTable& symbolTable,
                                                   const std::string& initialTypeName)
{
    if (initialTypeName.empty())
    {
        return {};
    }
    std::vector<std::string> hierarchy;
    std::unordered_set<std::string> visited;
    std::vector<std::string> worklist;
    worklist.push_back(initialTypeName);

    while (!worklist.empty())
    {
        std::string current = std::move(worklist.back());
        worklist.pop_back();

        if (current.empty() || !visited.insert(current).second)
        {
            continue;
        }
        hierarchy.push_back(current);

        auto bases = CollectDirectBases(symbolTable, current);
        for (auto it = bases.rbegin(); it != bases.rend(); ++it)
        {
            if (!visited.contains(*it))
            {
                worklist.push_back(std::move(*it));
            }
        }
    }
    return hierarchy;
}

/**
 * @brief Extracts the prefix substring of a line up to the cursor character.
 * @param[in] sourceCode Entire document string.
 * @param[in] line 0-indexed line number.
 * @param[in] character 0-indexed column character.
 * @return Substring preceding cursor on that line.
 */
std::string GetLinePrefix(const std::string& sourceCode, uint32_t line, uint32_t character)
{
    size_t currentLine = 0;
    size_t lineStart = 0;

    for (size_t i = 0; i < sourceCode.size(); ++i)
    {
        if (currentLine == line)
        {
            lineStart = i;
            break;
        }
        if (sourceCode[i] == '\n')
        {
            currentLine++;
        }
    }

    if (currentLine != line)
    {
        return "";
    }

    size_t lineEnd = sourceCode.find('\n', lineStart);
    if (lineEnd == std::string::npos)
    {
        lineEnd = sourceCode.size();
    }

    size_t prefixLen = std::min(static_cast<size_t>(character), lineEnd - lineStart);
    return sourceCode.substr(lineStart, prefixLen);
}

/**
 * @brief Categorization proximity for deterministic scope- and file-aware completion sorting.
 */
enum class SymbolProximity : uint8_t
{
    Local = 0,
    CurrentFile = 1,
    ExternalFile = 2,
    Predefined = 3,
    Snippet = 4,
    Keyword = 5
};

/**
 * @brief Formats sort text prefix for a symbol proximity tier.
 * @param[in] prox Symbol proximity tier.
 * @param[in] label Symbol display label.
 * @return Formatted sort text string.
 */
inline std::string FormatProximitySortText(SymbolProximity prox, std::string_view label)
{
    static constexpr std::array<std::string_view, 6> k_proxPrefixes = {"0_", "1_", "2_", "3_", "4_", "5_"};
    const size_t idx = static_cast<size_t>(prox);
    if (idx < k_proxPrefixes.size())
    {
        return std::string(k_proxPrefixes[idx]) + std::string(label);
    }
    return std::string("9_") + std::string(label);
}

/**
 * @brief Determines proximity tier of a symbol relative to current request URI.
 * @param[in] sym Target symbol.
 * @param[in] currentUri URI of currently active document.
 * @param[in] predefinedExt Predefined file extension.
 * @return Computed SymbolProximity.
 */
inline SymbolProximity DetermineSymbolProximity(const analysis::Symbol& sym, const std::string& currentUri,
                                                std::string_view predefinedExt)
{
    if (!currentUri.empty() && sym.fileUri == currentUri)
    {
        return SymbolProximity::CurrentFile;
    }
    if (angel_lsp::utils::IsPredefinedFile(sym.fileUri, predefinedExt))
    {
        return SymbolProximity::Predefined;
    }
    return SymbolProximity::ExternalFile;
}

/**
 * @brief Candidate completion item metadata for deduplication and insertion.
 */
struct CompletionCandidate
{
    std::string label;
    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
    std::string detail = {};
    std::string doc = {};
    std::string resolveKey = {};
    std::string snippet = {};
    std::string sortText = {};
    std::string insertText = {};
};

/**
 * @brief Context bundling destination collection vectors and request state.
 */
struct CompletionCollector
{
    std::vector<lsp::CompletionItem>& items;
    std::unordered_set<std::string>& seenLabels;
    const CompletionRequest& request;
};

/**
 * @brief Retrieves configured predefined file extension from completion request.
 * @param[in] request Completion request.
 * @return Predefined file extension string view.
 */
inline std::string_view GetPredefinedExtension(const CompletionRequest& request)
{
    return request.config ? std::string_view(request.config->info.predefinedFileExtension)
                          : std::string_view(".as.predefined");
}

/**
 * @brief Adds a completion item to collector if its label has not yet been offered.
 * @param[in,out] collector Completion collector context.
 * @param[in] candidate Completion item candidate attributes.
 */
void AddItemIfNew(CompletionCollector& collector, CompletionCandidate candidate)
{
    std::string dedupeKey = candidate.label;
    if (candidate.kind == lsp::CompletionItemKind::EnumMember && !candidate.detail.empty())
    {
        dedupeKey += "@" + candidate.detail;
    }
    if (candidate.label.empty() || collector.seenLabels.contains(dedupeKey))
    {
        return;
    }
    collector.seenLabels.insert(std::move(dedupeKey));

    lsp::CompletionItem item;
    item.label = std::move(candidate.label);
    item.kind = lsp::CompletionItemKindEnum(candidate.kind);
    if (!candidate.insertText.empty())
    {
        item.insertText = std::move(candidate.insertText);
    }
    else if (!candidate.snippet.empty())
    {
        item.insertText = std::move(candidate.snippet);
        item.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
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
        item.documentation =
            lsp::MarkupContent{lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), std::move(candidate.doc)};
    }
    if (!candidate.resolveKey.empty())
    {
        item.data = lsp::LSPAny(std::move(candidate.resolveKey));
    }
    collector.items.push_back(std::move(item));
}

/**
 * @brief Builds snippet string for function call placeholders.
 * @param[in] name Function name.
 * @param[in] params Function parameter list.
 * @return Formatted snippet string.
 */
std::string CallSnippet(const std::string& name, const std::vector<analysis::ParameterInformation>& params)
{
    if (name.empty())
    {
        return {};
    }

    std::string snippet = name + "(";
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

/**
 * @brief Retrieves the configured or default string type name.
 * @param[in] request Auto-completion request context.
 * @return String type name.
 */
static const std::string& ConfiguredStringTypeName(const CompletionRequest& request) noexcept
{
    static const std::string kDefaultString = "string";
    return request.config ? request.config->types.stringTypeName : kDefaultString;
}

/**
 * @brief Retrieves the configured or default array container type name.
 * @param[in] request Auto-completion request context.
 * @return Array container type name.
 */
static const std::string& ConfiguredArrayTypeName(const CompletionRequest& request) noexcept
{
    static const std::string kDefaultArray = "array";
    return request.config ? request.config->types.arrayTypeName : kDefaultArray;
}

/**
 * @brief Static list of AngelScript VM primitive type names.
 * @return Vector of primitive strings.
 */
static std::vector<std::string> GetPrimitiveTypeNames()
{
    std::vector<std::string> all;
    all.reserve(parser::primitives::k_all.size());
    for (const std::string_view name : parser::primitives::k_all)
    {
        all.emplace_back(name);
    }
    return all;
}

/**
 * @brief Checks if a symbol represents a user-defined or typedef type.
 * @param[in] sym Target symbol.
 * @return True if Class, Interface, Enum, Typedef, or Funcdef.
 */
bool IsTypeSymbol(const analysis::Symbol& sym)
{
    switch (sym.type)
    {
    case analysis::SymbolType::Class:
    case analysis::SymbolType::Interface:
    case analysis::SymbolType::Enum:
    case analysis::SymbolType::Typedef:
    case analysis::SymbolType::Funcdef:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Constructs template insert snippet for a class symbol.
 * @param[in] sym Candidate class symbol.
 * @return Formatted snippet with placeholders, or empty string.
 */
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

/**
 * @brief Where in the source's lexical structure a byte offset falls.
 */
enum class LexicalContext
{
    Code,
    Comment,
    StringLiteral,
};

/**
 * @brief Skips single-line comment.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this comment.
 */
bool SkipLineComment(std::string_view source, size_t& i, size_t end)
{
    const size_t close = source.find('\n', i + 2);
    if (close == std::string_view::npos || close >= end)
    {
        return true;
    }
    i = close + 1;
    return false;
}

/**
 * @brief Skips multi-line block comment.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this comment.
 */
bool SkipBlockComment(std::string_view source, size_t& i, size_t end)
{
    const size_t close = source.find("*/", i + 2);
    if (close == std::string_view::npos || close + 2 > end)
    {
        return true;
    }
    i = close + 2;
    return false;
}

/**
 * @brief Skips triple-quoted heredoc string literal.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this string.
 */
bool SkipHeredoc(std::string_view source, size_t& i, size_t end)
{
    const size_t close = source.find("\"\"\"", i + 3);
    if (close == std::string_view::npos || close + 3 > end)
    {
        return true;
    }
    i = close + 3;
    return false;
}

/**
 * @brief Skips single-line quoted string literal.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this string.
 */
bool SkipQuotedString(std::string_view source, size_t& i, size_t end)
{
    const char c = source[i];
    size_t j = i + 1;
    while (j < source.size() && source[j] != '\n' && source[j] != c)
    {
        if (source[j] == '\\' && j + 1 < source.size() && source[j + 1] != '\n')
        {
            ++j;
        }
        ++j;
    }
    if (end <= j)
    {
        return true;
    }
    i = j + 1;
    return false;
}

/**
 * @brief Scans comment or string literal at current index, updating offset and context.
 * @param[in] source Source code view.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @param[out] outContext Resulting lexical context if inside a token.
 * @return True if offset falls inside scanned comment or string.
 */
bool TryScanCommentOrString(std::string_view source, size_t& i, size_t end, LexicalContext& outContext)
{
    const char c = source[i];
    if (c == '/' && i + 1 < source.size())
    {
        if (source[i + 1] == '/')
        {
            outContext = LexicalContext::Comment;
            return SkipLineComment(source, i, end);
        }
        if (source[i + 1] == '*')
        {
            outContext = LexicalContext::Comment;
            return SkipBlockComment(source, i, end);
        }
    }
    if (c == '"' && i + 2 < source.size() && source[i + 1] == '"' && source[i + 2] == '"')
    {
        outContext = LexicalContext::StringLiteral;
        return SkipHeredoc(source, i, end);
    }
    if (c == '"' || c == '\'')
    {
        outContext = LexicalContext::StringLiteral;
        return SkipQuotedString(source, i, end);
    }
    return false;
}

/**
 * @brief Classifies the lexical context at a specific byte offset.
 * @param[in] source Source code view.
 * @param[in] offset Target byte offset.
 * @return LexicalContext category.
 */
LexicalContext ContextAtOffset(std::string_view source, size_t offset)
{
    const size_t end = std::min(offset, source.size());
    size_t i = 0;

    while (i < end)
    {
        LexicalContext ctx = LexicalContext::Code;
        if (TryScanCommentOrString(source, i, end, ctx))
        {
            return ctx;
        }
        ++i;
    }

    return LexicalContext::Code;
}

/**
 * @brief Checks if prefix terminates at a `case` or `default` label colon.
 * @param[in] prefix Prefix string.
 * @return True if after case label colon.
 */
bool IsAfterCaseLabelColon(const std::string& prefix)
{
    if (prefix.empty() || prefix.back() != ':')
    {
        return false;
    }
    if (prefix.size() >= 2 && prefix[prefix.size() - 2] == ':')
    {
        return false;
    }

    static const std::regex caseLabelRegex(R"((^|[\s{};])(case\s+[^;]*|default\s*):$)");
    return std::regex_search(prefix, caseLabelRegex);
}

/**
 * @brief Checks if prefix places cursor inside an unclosed template bracket `<...>`.
 * @param[in] prefix Prefix string view.
 * @return True if cursor is inside template argument brackets.
 */
bool IsInsideTemplateArguments(std::string_view prefix)
{
    int depth = 0;
    for (size_t i = prefix.size(); i-- > 0;)
    {
        const char c = prefix[i];
        if (c == ';' || c == '{' || c == '}' || c == '(' || c == ')')
        {
            return false;
        }
        if (c == '>')
        {
            ++depth;
        }
        else if (c == '<')
        {
            if (depth > 0)
            {
                --depth;
                continue;
            }
            if (i == 0)
            {
                return false;
            }
            const char before = prefix[i - 1];
            return std::isalnum(static_cast<unsigned char>(before)) || before == '_';
        }
    }
    return false;
}

/**
 * @brief Generates call snippet for an unambiguous function name.
 * @param[in] name Function name.
 * @param[in] table Global symbol table.
 * @return Call snippet string, or empty.
 */
std::string CallSnippetForName(const std::string& name, const analysis::SymbolTable& table)
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

/**
 * @brief Generates template insert snippet for a named template class.
 * @param[in] name Class name.
 * @param[in] table Global symbol table.
 * @return Snippet string, or empty.
 */
std::string TemplateSnippetForName(const std::string& name, const analysis::SymbolTable& table)
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

/**
 * @brief Returns list of AngelScript reserved and contextual keywords.
 * @return Reference to static keyword vector.
 */
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

/**
 * @brief Constructs a completion item for an include path candidate.
 * @param[in] candidate Absolute candidate path.
 * @param[in] fromDirectory Origin directory for relative paths.
 * @param[in] request Completion request.
 * @param[in] quoteColumn 0-indexed column of opening quote.
 * @return Constructed completion item, or nullopt if invalid.
 */
std::optional<lsp::CompletionItem> CreateIncludeCompletionItem(const std::string& candidate,
                                                               const std::filesystem::path& fromDirectory,
                                                               const CompletionRequest& request, uint32_t quoteColumn)
{
    if (candidate.empty() ||
        utils::IncludeResolver::NormalizePath(candidate) == utils::IncludeResolver::NormalizePath(request.documentPath))
    {
        return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(std::filesystem::path(candidate), fromDirectory, ec);
    if (ec || relative.empty())
    {
        return std::nullopt;
    }

    std::string insertText = relative.generic_string();
    if (!request.implicitExtension.empty() && insertText.size() > request.implicitExtension.size() &&
        insertText.ends_with(request.implicitExtension))
    {
        insertText.resize(insertText.size() - request.implicitExtension.size());
    }

    lsp::CompletionItem item;
    item.label = insertText;
    item.kind = lsp::CompletionItemKind::File;
    item.detail = candidate;
    item.sortText = (insertText.starts_with("../") ? "1" : "0") + insertText;

    lsp::TextEdit edit;
    edit.range.start.line = request.position.line;
    edit.range.start.character = quoteColumn;
    edit.range.end.line = request.position.line;
    edit.range.end.character = request.position.character;
    edit.newText = insertText;
    item.textEdit = edit;

    return item;
}

/**
 * @brief Completes the file path inside an `#include "..."`.
 * @param[in] request Completion request.
 * @param[in] linePrefix Line text preceding the cursor.
 * @return Optional vector of include file completion items.
 */
std::optional<std::vector<lsp::CompletionItem>> CompleteIncludePath(const CompletionRequest& request,
                                                                    const std::string& linePrefix)
{
    if (request.documentPath.empty() || !request.listIncludeCandidates)
    {
        return std::nullopt;
    }

    static const std::regex includePrefixRegex(R"(^[ \t]*#include[ \t]*"([^"]*)$)");
    std::smatch match;
    if (!std::regex_search(linePrefix, match, includePrefixRegex))
    {
        return std::nullopt;
    }

    const std::string typed = match[1].str();
    const auto quoteColumn = static_cast<uint32_t>(linePrefix.size() - typed.size());
    const std::filesystem::path fromDirectory = std::filesystem::path(request.documentPath).parent_path();

    std::vector<lsp::CompletionItem> items;
    std::unordered_set<std::string> offered;

    for (const std::string& candidate : request.listIncludeCandidates())
    {
        auto maybeItem = CreateIncludeCompletionItem(candidate, fromDirectory, request, quoteColumn);
        if (maybeItem && offered.insert(maybeItem->label).second)
        {
            items.push_back(std::move(*maybeItem));
        }
    }

    return items;
}

/**
 * @brief Handles early return for include paths or suppressed lexical contexts.
 * @param[in] request Completion request.
 * @param[in] prefix Prefix text before cursor.
 * @return Optional vector of completion items (may be empty if suppressed).
 */
std::optional<std::vector<lsp::CompletionItem>> HandleIncludeOrLexicalSuppression(const CompletionRequest& request,
                                                                                  const std::string& prefix)
{
    if (auto includeItems = CompleteIncludePath(request, prefix))
    {
        return includeItems;
    }
    const size_t cursorOffset = utils::LineStartOffset(request.sourceCode, request.position.line) + prefix.size();
    if (ContextAtOffset(request.sourceCode, cursorOffset) != LexicalContext::Code || IsAfterCaseLabelColon(prefix))
    {
        return std::vector<lsp::CompletionItem>{};
    }
    return std::nullopt;
}

/**
 * @brief Collects enum members under a scope qualifier.
 * @param[in] qualifier Qualifier type name.
 * @param[in,out] collector Completion collector context.
 */
void CollectEnumMembersUnderQualifier(const std::string& qualifier, CompletionCollector& collector)
{
    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    auto enumMatches = collector.request.symbolTable.FindSymbolsPtr(qualifier);
    if (enumMatches && !enumMatches->empty())
    {
        for (const auto& sym : *enumMatches)
        {
            if (sym.type == analysis::SymbolType::Enum)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                for (const auto& mem : sym.GetEnum().members)
                {
                    std::string sortText = FormatProximitySortText(prox, mem.name);
                    AddItemIfNew(collector, {mem.name, lsp::CompletionItemKind::EnumMember, qualifier + "::" + mem.name,
                                             "", "", "", std::move(sortText)});
                }
            }
        }
    }
    else if (qualifier.find("::") == std::string::npos)
    {
        auto shortMatches = collector.request.symbolTable.FindTypeSymbolsByShortName(qualifier);
        for (const auto& sym : shortMatches)
        {
            if (sym.type == analysis::SymbolType::Enum)
            {
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                for (const auto& mem : sym.GetEnum().members)
                {
                    std::string sortText = FormatProximitySortText(prox, mem.name);
                    AddItemIfNew(collector, {mem.name, lsp::CompletionItemKind::EnumMember, qualifier + "::" + mem.name,
                                             "", "", "", std::move(sortText)});
                }
            }
        }
    }
}

/**
 * @brief Collects container member symbols under a scope qualifier.
 * @param[in] qualifier Qualifier container name.
 * @param[in,out] collector Completion collector context.
 */
/**
 * @brief Resolves completion item kind and detail for a container member symbol.
 * @param[in] sym Container member symbol.
 * @param[out] kind Computed LSP completion item kind.
 * @param[out] detail Computed detail string.
 */
/**
 * @brief Reconstructs the qualified namespace path for a lexical scope node.
 * @param[in] scope Starting lexical scope node.
 * @param[in] request Completion request context.
 * @return Qualified namespace path, or empty string if at global scope.
 */
std::string BuildScopeNamespacePath(const analysis::Scope* scope, const CompletionRequest& request)
{
    if (!scope || !request.tree)
    {
        return "";
    }
    TSNode rootNode = ts_tree_root_node(request.tree);
    TSPoint pt{scope->startLine, scope->startCharacter};
    TSNode curNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    for (const auto& c : analysis::GetEnclosingContainers(curNode, request.sourceCode))
    {
        if (c.kind == analysis::ContainerKind::Namespace)
        {
            return c.qualifiedName;
        }
    }
    return "";
}

/**
 * @brief Appends a namespace name to the list if non-empty and not yet present.
 * @param[in,out] out Target namespace list.
 * @param[in] ns Candidate namespace string.
 */
void AppendUniqueNamespace(std::vector<std::string>& out, const std::string& ns)
{
    if (!ns.empty() && std::find(out.begin(), out.end(), ns) == out.end())
    {
        out.push_back(ns);
    }
}

/**
 * @brief Collects all namespaces visible at the cursor via enclosing blocks and `using namespace` directives.
 * @param[in] request Completion request context.
 * @param[in] innermostScope Innermost lexical scope at cursor, if available.
 * @return Ordered list of unique visible namespace names.
 */
std::vector<std::string> CollectVisibleNamespaces(const CompletionRequest& request,
                                                  const analysis::Scope* innermostScope)
{
    std::vector<std::string> visible;
    std::vector<std::string> enclosing;
    if (request.tree)
    {
        TSNode rootNode = ts_tree_root_node(request.tree);
        TSPoint pt{request.position.line, request.position.character};
        TSNode curNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
        for (const auto& c : analysis::GetEnclosingContainers(curNode, request.sourceCode))
        {
            if (c.kind == analysis::ContainerKind::Namespace)
            {
                AppendUniqueNamespace(enclosing, c.qualifiedName);
                AppendUniqueNamespace(visible, c.qualifiedName);
            }
        }
    }
    for (const analysis::Scope* cur = innermostScope; cur != nullptr; cur = cur->parent)
    {
        if (cur->kind == analysis::ScopeKind::Namespace)
        {
            std::string nsPath = BuildScopeNamespacePath(cur, request);
            AppendUniqueNamespace(enclosing, nsPath);
            AppendUniqueNamespace(visible, nsPath);
        }
    }
    if (request.tree)
    {
        TSNode rootNode = ts_tree_root_node(request.tree);
        for (const auto& imported : analysis::CollectUsingNamespaces(rootNode, request.sourceCode))
        {
            AppendUniqueNamespace(visible, imported);
            for (const auto& enc : enclosing)
            {
                AppendUniqueNamespace(visible, enc + "::" + imported);
            }
        }
    }
    return visible;
}

/**
 * @brief Resolves completion item kind and detail for a container member symbol.
 * @param[in] sym Container member symbol.
 * @param[out] kind Computed LSP completion item kind.
 * @param[out] detail Computed detail string.
 */
void ResolveContainerMemberItemDetails(const analysis::Symbol& sym, lsp::CompletionItemKind& kind, std::string& detail)
{
    kind = lsp::CompletionItemKind::Variable;
    switch (sym.type)
    {
    case analysis::SymbolType::Function:
        kind = lsp::CompletionItemKind::Function;
        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
        break;
    case analysis::SymbolType::Variable:
        if (sym.GetVariable().isEnumConstant)
        {
            kind = lsp::CompletionItemKind::EnumMember;
            const auto& var = sym.GetVariable();
            detail = var.typeName.empty() ? sym.qualifiedName : (var.typeName + "::" + sym.name);
        }
        else
        {
            kind = lsp::CompletionItemKind::Variable;
            detail = sym.GetVariable().typeName;
        }
        break;
    case analysis::SymbolType::Class:
        kind = lsp::CompletionItemKind::Class;
        break;
    case analysis::SymbolType::Interface:
        kind = lsp::CompletionItemKind::Interface;
        break;
    case analysis::SymbolType::Enum:
        kind = lsp::CompletionItemKind::Enum;
        break;
    case analysis::SymbolType::Namespace:
        kind = lsp::CompletionItemKind::Module;
        break;
    default:
        break;
    }
}

/**
 * @brief Checks if a qualifier corresponds to an enum type.
 * @param[in] qualifier Scope qualifier text.
 * @param[in] symbolTable Symbol table to query.
 * @return True if qualifier matches an enum symbol.
 */
bool IsEnumQualifier(const std::string& qualifier, const analysis::SymbolTable& symbolTable)
{
    const auto symList = symbolTable.FindSymbolsPtr(qualifier);
    if (symList)
    {
        for (const auto& sym : *symList)
        {
            if (sym.type == analysis::SymbolType::Enum)
            {
                return true;
            }
        }
    }
    if (qualifier.find("::") == std::string::npos)
    {
        const auto shortMatches = symbolTable.FindTypeSymbolsByShortName(qualifier);
        for (const auto& sym : shortMatches)
        {
            if (sym.type == analysis::SymbolType::Enum)
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Adds member items of a specific container to the collector.
 * @param[in] container Container name to query.
 * @param[in] qualifier Scope qualifier text.
 * @param[in] ruleIndex Pre-indexed rule information.
 * @param[in,out] collector Completion collector context.
 */
void AddContainerMembers(const std::string& container, const std::string& qualifier,
                         const analysis::rules::RuleIndex* ruleIndex, CompletionCollector& collector)
{
    const auto& cm = ruleIndex->Members(container);
    for (const auto& key : cm.memberKeys)
    {
        const auto symList = collector.request.symbolTable.FindSymbolsPtr(key);
        if (!symList)
        {
            continue;
        }
        for (const auto& sym : *symList)
        {
            if (sym.containerName == container || sym.containerName == qualifier)
            {
                lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
                std::string detail;
                ResolveContainerMemberItemDetails(sym, kind, detail);
                const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
                SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
                std::string sortText = FormatProximitySortText(prox, sym.name);
                AddItemIfNew(collector,
                             {sym.name, kind, std::move(detail), "", sym.qualifiedName, "", std::move(sortText)});
            }
        }
    }
}

void CollectContainerMembersUnderQualifier(const std::string& qualifier, CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }

    AddContainerMembers(qualifier, qualifier, ruleIndex.get(), collector);
    if (qualifier.find("::") == std::string::npos)
    {
        auto qit = ruleIndex->qualifiedTypesByShortName.find(qualifier);
        if (qit != ruleIndex->qualifiedTypesByShortName.end())
        {
            for (const auto& q : qit->second)
            {
                AddContainerMembers(q, qualifier, ruleIndex.get(), collector);
            }
        }
    }
    for (const auto& ns : CollectVisibleNamespaces(collector.request, nullptr))
    {
        const std::string qualifiedContainer = ns + "::" + qualifier;
        AddContainerMembers(qualifiedContainer, qualifier, ruleIndex.get(), collector);
    }
}

/**
 * @brief Attempts to complete members following a scope qualifier `Qualifier::`.
 * @param[in] prefix Prefix text before cursor.
 * @param[in,out] collector Completion collector context.
 * @return True if scope resolution completed.
 */
bool TryCompleteScopeResolution(const std::string& prefix, CompletionCollector& collector)
{
    static const std::regex scopeResolutionRegex(R"(((?:[a-zA-Z_][a-zA-Z0-9_]*::)+)([a-zA-Z_][a-zA-Z0-9_]*)?$)");
    std::smatch scopeMatch;
    if (!std::regex_search(prefix, scopeMatch, scopeResolutionRegex))
    {
        return false;
    }
    std::string qualifier = scopeMatch[1].str();
    if (qualifier.ends_with("::"))
    {
        qualifier.resize(qualifier.size() - 2);
    }
    CollectEnumMembersUnderQualifier(qualifier, collector);
    for (const auto& ns : CollectVisibleNamespaces(collector.request, nullptr))
    {
        CollectEnumMembersUnderQualifier(ns + "::" + qualifier, collector);
    }
    if (IsEnumQualifier(qualifier, collector.request.symbolTable))
    {
        return true;
    }
    CollectContainerMembersUnderQualifier(qualifier, collector);
    return true;
}

/**
 * @brief Completes type names when cursor is inside template argument brackets `<...>`.
 * @param[in] prefix Prefix text before cursor.
 * @param[in,out] collector Completion collector context.
 * @return True if template arguments completed.
 */
bool TryCompleteTemplateArguments(const std::string& prefix, CompletionCollector& collector)
{
    if (!IsInsideTemplateArguments(prefix))
    {
        return false;
    }
    collector.request.symbolTable.ForEachGlobalTypeSymbol(
        [&](const analysis::Symbol& sym)
        {
            lsp::CompletionItemKind kind = lsp::CompletionItemKind::Class;
            switch (sym.type)
            {
            case analysis::SymbolType::Interface:
                kind = lsp::CompletionItemKind::Interface;
                break;
            case analysis::SymbolType::Enum:
                kind = lsp::CompletionItemKind::Enum;
                break;
            case analysis::SymbolType::Typedef:
            case analysis::SymbolType::Funcdef:
                kind = lsp::CompletionItemKind::TypeParameter;
                break;
            default:
                break;
            }
            const std::string snippet = collector.request.snippetSupport ? TemplateInsertSnippet(sym) : std::string{};
            const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
            SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
            std::string sortText = FormatProximitySortText(prox, sym.name);
            AddItemIfNew(collector, {sym.name, kind, "", "", sym.qualifiedName, snippet, std::move(sortText)});
        });

    for (const auto& primitive : GetPrimitiveTypeNames())
    {
        std::string sortText = FormatProximitySortText(SymbolProximity::Predefined, primitive);
        AddItemIfNew(collector, {primitive, lsp::CompletionItemKind::Keyword, "", "", "", "", std::move(sortText)});
    }
    return true;
}

/**
 * @brief Resolves base container type name, handling unqualified template class lookups.
 * @param[in] canonicalType Canonical type name.
 * @param[in] symbolTable Global symbol table.
 * @return Fully qualified base container name.
 */
std::string FindCanonicalBaseContainer(const std::string& canonicalType, const analysis::SymbolTable& symbolTable)
{
    auto targetTemplate = analysis::ParseTemplateType(canonicalType);
    return symbolTable.QualifyShortTypeName(targetTemplate.containerName);
}

/**
 * @brief Substitutes template type parameters in a type signature.
 * @param[in] text Input type string.
 * @param[in] binding Template bindings.
 * @param[in] templateArgs Explicit template arguments.
 * @return Substituted type string.
 */
std::string SubstituteTypeParameters(std::string text, const analysis::TemplateBinding& binding,
                                     const std::vector<std::string>& templateArgs)
{
    if (binding.usable)
    {
        for (size_t i = 0; i < binding.parameters.size(); ++i)
        {
            text = analysis::SubstituteTypeParam(text, binding.parameters[i], binding.arguments[i]);
        }
        return text;
    }
    if (!templateArgs.empty())
    {
        return analysis::SubstituteTypeParam(text, "T", templateArgs[0]);
    }
    return text;
}

/**
 * @brief Template binding and argument context for member completion.
 */
struct MemberTemplateContext
{
    const analysis::TemplateBinding& binding;
    const std::vector<std::string>& templateArgs;
};

/**
 * @brief Converts a member symbol into a completion candidate item.
 * @param[in] sym Target member symbol.
 * @param[in] typeName Container type name.
 * @param[in] tCtx Template context bundling bindings and args.
 * @param[in,out] collector Completion collector context.
 */
void PopulateMemberSymbolCandidate(const analysis::Symbol& sym, const std::string& typeName,
                                   const MemberTemplateContext& tCtx, CompletionCollector& collector)
{
    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Field;
    std::string detail;
    std::string snippet;
    if (sym.type == analysis::SymbolType::Function)
    {
        kind = lsp::CompletionItemKind::Method;
        detail = FormatMethodDetail(sym, tCtx.binding, tCtx.templateArgs);
        const bool completeParens =
            !collector.request.config || collector.request.config->features.completionCompleteFunctionParens;
        if (collector.request.snippetSupport && completeParens)
        {
            snippet = sym.GetFunction().parameters.empty() ? (sym.name + "()$0") : (sym.name + "($0)");
        }
    }
    else if (sym.type == analysis::SymbolType::Variable)
    {
        kind = lsp::CompletionItemKind::Field;
        detail = SubstituteTypeParameters(sym.GetVariable().typeName, tCtx.binding, tCtx.templateArgs);
    }
    else if (sym.type == analysis::SymbolType::Property)
    {
        kind = lsp::CompletionItemKind::Property;
    }

    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName, std::move(snippet), std::move(sortText)});

    const int accessorMode = collector.request.config ? collector.request.config->engine.propertyAccessorMode : 2;
    if (accessorMode < 2)
    {
        return;
    }
    const bool accessorKeywordRequired = (accessorMode == 3);
    const std::string propertyName = analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
    if (!propertyName.empty())
    {
        std::string propertyType = analysis::PropertyTypeFromAccessors(analysis::FindPropertyAccessors(
            typeName, propertyName, collector.request.symbolTable, accessorKeywordRequired));
        propertyType = SubstituteTypeParameters(propertyType, tCtx.binding, tCtx.templateArgs);
        std::string propSortText = FormatProximitySortText(prox, propertyName);
        AddItemIfNew(collector, {propertyName, lsp::CompletionItemKind::Property, propertyType, "", sym.qualifiedName,
                                 "", std::move(propSortText)});
    }
}

/**
 * @brief Collects all member symbols for a container type.
 * @param[in] typeName Container type name.
 * @param[in] binding Template binding information.
 * @param[in] templateArgs Explicit template arguments.
 * @param[in,out] collector Completion collector context.
 */
void PopulateMembersForType(const std::string& typeName, const analysis::TemplateBinding& binding,
                            const std::vector<std::string>& templateArgs, CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    MemberTemplateContext tCtx{binding, templateArgs};
    const auto& members = ruleIndex->Members(typeName);
    for (const auto& key : members.memberKeys)
    {
        const auto symbols = collector.request.symbolTable.FindSymbolsPtr(key);
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.containerName == typeName && !IsConstructorOrDestructor(sym, typeName))
            {
                PopulateMemberSymbolCandidate(sym, typeName, tCtx, collector);
            }
        }
    }
}

/**
 * @brief Resolves name of the class enclosing cursor position.
 * @param[in] request Completion request.
 * @return Enclosing class name, or empty string.
 */
std::string FindEnclosingClassName(const CompletionRequest& request)
{
    if (request.tree)
    {
        TSNode rootNode = ts_tree_root_node(request.tree);
        TSPoint pt{request.position.line, request.position.character};
        TSNode curNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
        auto containers = analysis::GetEnclosingContainers(curNode, request.sourceCode);
        for (const auto& c : containers)
        {
            if (c.kind == analysis::ContainerKind::Class || c.kind == analysis::ContainerKind::Interface)
            {
                return c.qualifiedName.empty() ? c.name : c.qualifiedName;
            }
        }
    }
    std::string className;
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.type == analysis::SymbolType::Class && sym.fileUri == request.uri)
                {
                    if (request.position.line >= sym.startLine && request.position.line <= sym.endLine)
                    {
                        className = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                        return;
                    }
                }
            }
        });
    return className;
}

/**
 * @brief Checks if a dot is part of a numeric literal like '3.'.
 * @param[in] prefix Prefix text before cursor.
 * @param[in] dotPos Index of the dot character.
 * @return True if dot is part of a number literal.
 */
bool IsNumericLiteralDot(std::string_view prefix, size_t dotPos)
{
    if (dotPos == 0 || !std::isdigit(static_cast<unsigned char>(prefix[dotPos - 1])))
    {
        return false;
    }
    size_t d = dotPos - 1;
    while (d > 0 && std::isdigit(static_cast<unsigned char>(prefix[d - 1])))
    {
        --d;
    }
    return d == 0 || (!std::isalpha(static_cast<unsigned char>(prefix[d - 1])) && prefix[d - 1] != '_');
}

/**
 * @brief Locates the AST node representing the receiver expression preceding a member access dot.
 * @param[in] rootNode Root AST node of the document.
 * @param[in] sourceCode Document source text.
 * @param[in] dotByteOffset Byte offset of the member access delimiter ('.').
 * @return AST node representing the receiver expression, or a null node if unresolvable.
 */
static TSNode FindReceiverFromParentAtDot(TSNode rootNode, size_t dotByteOffset)
{
    TSNode dotNode = ts_node_descendant_for_byte_range(rootNode, static_cast<uint32_t>(dotByteOffset),
                                                       static_cast<uint32_t>(dotByteOffset + 1));
    if (ts_node_is_null(dotNode))
    {
        return TSNode{};
    }
    TSNode parent = ts_node_parent(dotNode);
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }
    if (std::string_view(ts_node_type(parent)) == "member_expression")
    {
        TSNode obj = parser::GetChildByField(parent, parser::fields::Object);
        if (!ts_node_is_null(obj))
        {
            return obj;
        }
    }
    TSNode prev = ts_node_prev_sibling(dotNode);
    if (!ts_node_is_null(prev))
    {
        return prev;
    }
    return TSNode{};
}

/**
 * @brief Checks if an AST node type acts as an outer expression boundary.
 * @param[in] pType Node type identifier string.
 * @return True if the node terminates upward expression traversal.
 */
static bool IsExpressionBoundaryType(std::string_view pType)
{
    return pType == "script" || pType == "statement_block" || pType == "expression_statement" ||
           pType == "variable_declaration" || pType == "assignment_expression" || pType == "return_statement";
}

static TSNode FindReceiverPrecedingDot(TSNode rootNode, std::string_view sourceCode, size_t dotByteOffset)
{
    size_t b = dotByteOffset;
    while (b > 0 && (sourceCode[b - 1] == ' ' || sourceCode[b - 1] == '\t' || sourceCode[b - 1] == '\r' ||
                     sourceCode[b - 1] == '\n'))
    {
        --b;
    }
    if (b == 0)
    {
        return TSNode{};
    }
    TSNode leaf = ts_node_descendant_for_byte_range(rootNode, static_cast<uint32_t>(b - 1), static_cast<uint32_t>(b));
    if (ts_node_is_null(leaf))
    {
        return TSNode{};
    }
    TSNode curr = leaf;
    while (!ts_node_is_null(ts_node_parent(curr)))
    {
        TSNode p = ts_node_parent(curr);
        std::string_view pType = ts_node_type(p);
        if (IsExpressionBoundaryType(pType) || ts_node_end_byte(p) > dotByteOffset)
        {
            break;
        }
        curr = p;
    }
    return curr;
}

/**
 * @brief Finds the receiver AST node associated with a member access dot ('.').
 * @param[in] rootNode Root AST node of the document.
 * @param[in] sourceCode Document source text.
 * @param[in] dotByteOffset Byte offset of the member access delimiter ('.').
 * @return AST node representing the receiver expression, or a null node if unresolvable.
 */
TSNode FindReceiverNodeAtDot(TSNode rootNode, std::string_view sourceCode, size_t dotByteOffset)
{
    if (ts_node_is_null(rootNode) || dotByteOffset == 0)
    {
        return TSNode{};
    }
    TSNode receiver = FindReceiverFromParentAtDot(rootNode, dotByteOffset);
    if (!ts_node_is_null(receiver))
    {
        return receiver;
    }
    return FindReceiverPrecedingDot(rootNode, sourceCode, dotByteOffset);
}

static std::string ResolveNamedReceiverType(std::string_view nodeText, const CompletionRequest& request,
                                            const analysis::Scope* innermostScope)
{
    if (nodeText.empty())
    {
        return "";
    }
    if (nodeText == "this")
    {
        return FindEnclosingClassName(request);
    }
    if (innermostScope)
    {
        const analysis::LocalDefinition* def = analysis::ResolveInScope(innermostScope, nodeText);
        if (def && !def->typeName.empty())
        {
            return def->typeName;
        }
    }
    if (auto syms = request.symbolTable.FindSymbolsPtr(nodeText))
    {
        for (const auto& sym : *syms)
        {
            if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().typeName.empty())
            {
                return sym.GetVariable().typeName;
            }
        }
    }
    return "";
}

/**
 * @brief Resolves the type name of a receiver AST node using Layer 2 semantic services.
 * @param[in] receiverNode AST node representing the receiver.
 * @param[in] request Completion request.
 * @param[in] innermostScope Innermost lexical scope at cursor.
 * @return Resolved raw type name string, or empty string.
 */
std::string ResolveReceiverNodeType(TSNode receiverNode, const CompletionRequest& request,
                                    const analysis::Scope* innermostScope)
{
    if (ts_node_is_null(receiverNode))
    {
        return "";
    }
    std::string nodeText = analysis::GetNodeText(receiverNode, request.sourceCode);
    if (std::string named = ResolveNamedReceiverType(nodeText, request, innermostScope); !named.empty())
    {
        return named;
    }

    const auto& strType = ConfiguredStringTypeName(request);
    const auto& arrType = ConfiguredArrayTypeName(request);

    analysis::ExpressionTypeContext exprCtx{innermostScope, request.symbolTable, request.sourceCode, request.uri};
    exprCtx.stringTypeName = strType;
    exprCtx.arrayTypeName = arrType;
    std::string exprType = analysis::ResolveExpressionType(receiverNode, exprCtx);
    if (!exprType.empty() && exprType != "void" && exprType != "unknown")
    {
        return exprType;
    }

    return analysis::ResolveReceiverType(receiverNode, request.sourceCode, request.symbolTable,
                                         {innermostScope, "", request.uri});
}

/**
 * @brief Peels index bracket sequences preceding a dot access.
 * @param[in] prefix Source prefix preceding cursor.
 * @param[in,out] s Offset position before bracket sequence.
 * @return Number of index brackets peeled.
 */
static size_t PeelIndexBrackets(const std::string& prefix, size_t& s)
{
    size_t count = 0;
    while (s > 0 && prefix[s - 1] == ']')
    {
        --s;
        int depth = 1;
        while (s > 0 && depth > 0)
        {
            if (prefix[s - 1] == ']')
            {
                ++depth;
            }
            else if (prefix[s - 1] == '[')
            {
                --depth;
            }
            --s;
        }
        ++count;
        while (s > 0 && (prefix[s - 1] == ' ' || prefix[s - 1] == '\t'))
        {
            --s;
        }
    }
    return count;
}

/**
 * @brief Resolves fallback receiver type when AST node resolution is unavailable or incomplete.
 * @param[in] prefix Prefix text before cursor.
 * @param[in] dotCol Column offset of access dot.
 * @param[in] collector Completion collector context.
 * @param[in] innermostScope Lexical scope at cursor.
 * @return Resolved raw type name, or empty string.
 */
static std::string ResolveFallbackReceiverType(const std::string& prefix, size_t dotCol,
                                               const CompletionCollector& collector,
                                               const analysis::Scope* innermostScope)
{
    size_t s = dotCol;
    while (s > 0 && (prefix[s - 1] == ' ' || prefix[s - 1] == '\t'))
    {
        --s;
    }
    size_t indexCount = PeelIndexBrackets(prefix, s);
    size_t e = s;
    while (s > 0 && (std::isalnum(static_cast<unsigned char>(prefix[s - 1])) || prefix[s - 1] == '_'))
    {
        --s;
    }
    if (e <= s)
    {
        return "";
    }
    std::string ident(prefix.substr(s, e - s));
    std::string rawTypeName = ResolveNamedReceiverType(ident, collector.request, innermostScope);
    if (!rawTypeName.empty() && indexCount > 0)
    {
        const auto& arrayContainer = ConfiguredArrayTypeName(collector.request);
        rawTypeName =
            analysis::ResolveIndexedType(rawTypeName, indexCount, collector.request.symbolTable, arrayContainer);
    }
    return rawTypeName;
}

static std::optional<size_t> FindAccessDotCol(const std::string& prefix)
{
    size_t i = prefix.size();
    while (i > 0 && (std::isalnum(static_cast<unsigned char>(prefix[i - 1])) || prefix[i - 1] == '_'))
    {
        --i;
    }
    while (i > 0 && (prefix[i - 1] == ' ' || prefix[i - 1] == '\t'))
    {
        --i;
    }
    if (i == 0 || prefix[i - 1] != '.' || IsNumericLiteralDot(prefix, i - 1))
    {
        return std::nullopt;
    }
    return i - 1;
}

static void PopulateHierarchicalMembers(const std::string& rawTypeName, CompletionCollector& collector)
{
    const auto& arrayContainer = ConfiguredArrayTypeName(collector.request);
    std::string canonicalType = CanonicalizeArrayType(rawTypeName, arrayContainer);
    std::string baseContainer = FindCanonicalBaseContainer(canonicalType, collector.request.symbolTable);
    auto targetTemplate = analysis::ParseTemplateType(canonicalType);
    const auto binding = analysis::BindTemplateArguments(canonicalType, collector.request.symbolTable);

    auto hierarchy = GetInheritedTypeHierarchy(collector.request.symbolTable, baseContainer);
    for (const auto& typeName : hierarchy)
    {
        PopulateMembersForType(typeName, binding, targetTemplate.templateArgs, collector);
    }
}

/**
 * @brief Attempts to complete member expression following a `.`.
 * @param[in] prefix Prefix text before cursor.
 * @param[in] innermostScope Lexical scope at cursor.
 * @param[in,out] collector Completion collector context.
 * @return True if member completion completed.
 */
bool TryCompleteMemberAccess(const std::string& prefix, const analysis::Scope* innermostScope,
                             CompletionCollector& collector)
{
    auto dotCol = FindAccessDotCol(prefix);
    if (!dotCol)
    {
        return false;
    }

    std::string rawTypeName;
    if (collector.request.tree)
    {
        size_t lineStart = utils::LineStartOffset(collector.request.sourceCode, collector.request.position.line);
        size_t dotByteOffset = lineStart + *dotCol;
        TSNode rootNode = ts_tree_root_node(collector.request.tree);
        TSNode receiverNode = FindReceiverNodeAtDot(rootNode, collector.request.sourceCode, dotByteOffset);
        rawTypeName = ResolveReceiverNodeType(receiverNode, collector.request, innermostScope);
    }

    if (rawTypeName.empty())
    {
        rawTypeName = ResolveFallbackReceiverType(prefix, *dotCol, collector, innermostScope);
    }

    if (rawTypeName.empty() || analysis::IsCorePrimitive(rawTypeName) || rawTypeName == "void")
    {
        return true;
    }

    PopulateHierarchicalMembers(rawTypeName, collector);
    return true;
}

/**
 * @brief Adds an enum constant candidate, qualifying with enum type name when enabled.
 * @param[in,out] collector Completion collector context.
 * @param[in] sym Target enum constant symbol.
 */
static void AddEnumConstantCandidate(CompletionCollector& collector, const analysis::Symbol& sym)
{
    const auto& var = sym.GetVariable();
    lsp::CompletionItemKind kind = lsp::CompletionItemKind::EnumMember;
    std::string detail = var.typeName.empty() ? sym.name : var.typeName + "::" + sym.name;
    const bool qualifyEnum =
        !collector.request.config || collector.request.config->features.completionQualifyEnumValues;
    const std::string_view predefinedExt = GetPredefinedExtension(collector.request);
    SymbolProximity prox = DetermineSymbolProximity(sym, collector.request.uri, predefinedExt);
    std::string sortText = FormatProximitySortText(prox, sym.name);
    CompletionCandidate cand{sym.name, kind, detail, "", sym.qualifiedName, "", std::move(sortText)};
    if (qualifyEnum && !var.typeName.empty())
    {
        cand.insertText = var.typeName + "::" + sym.name;
    }
    AddItemIfNew(collector, std::move(cand));
}

/**
 * @brief Processes a local scope constant definition (enum constant) and adds completion candidate.
 * @param[in] def Local definition entry.
 * @param[in] scopeNs Enclosing namespace path of the scope.
 * @param[in,out] collector Completion collector context.
 * @param[in] prox Symbol proximity tier.
 */
void ProcessScopeConstantDefinition(const analysis::LocalDefinition& def, const std::string& scopeNs,
                                    CompletionCollector& collector, SymbolProximity prox)
{
    const std::string symbolKey = scopeNs.empty() ? def.name : (scopeNs + "::" + def.name);
    auto syms = collector.request.symbolTable.FindSymbols(symbolKey);
    if (syms.empty() && !scopeNs.empty())
    {
        syms = collector.request.symbolTable.FindSymbols(def.name);
    }
    bool added = false;
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::Variable && sym.GetVariable().isEnumConstant)
        {
            AddEnumConstantCandidate(collector, sym);
            added = true;
        }
    }
    if (!added)
    {
        std::string sortText = FormatProximitySortText(prox, def.name);
        AddItemIfNew(collector, {def.name, lsp::CompletionItemKind::EnumMember, def.typeName, "", symbolKey, "",
                                 std::move(sortText)});
    }
}

/**
 * @brief Processes a single local scope definition and adds completion candidate.
 * @param[in] def Local definition entry.
 * @param[in] scopeNs Enclosing namespace path of the scope.
 * @param[in,out] collector Completion collector context.
 * @param[in] prox Symbol proximity tier.
 */
void ProcessScopeDefinition(const analysis::LocalDefinition& def, const std::string& scopeNs,
                            CompletionCollector& collector, SymbolProximity prox)
{
    if (def.kind == analysis::LocalDefinitionKind::Constant)
    {
        ProcessScopeConstantDefinition(def, scopeNs, collector, prox);
        return;
    }
    const std::string symbolKey = scopeNs.empty() ? def.name : (scopeNs + "::" + def.name);
    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
    bool isCallable = false;
    std::string snippet;
    if (def.kind == analysis::LocalDefinitionKind::Function || def.kind == analysis::LocalDefinitionKind::Method)
    {
        kind = lsp::CompletionItemKind::Function;
        isCallable = true;
        const bool completeParens =
            !collector.request.config || collector.request.config->features.completionCompleteFunctionParens;
        if (collector.request.snippetSupport && completeParens)
        {
            snippet = CallSnippetForName(symbolKey, collector.request.symbolTable);
            if (snippet.empty() && !scopeNs.empty())
            {
                snippet = CallSnippetForName(def.name, collector.request.symbolTable);
            }
        }
    }
    else if (def.kind == analysis::LocalDefinitionKind::Type)
    {
        kind = lsp::CompletionItemKind::Class;
        if (collector.request.snippetSupport)
        {
            snippet = TemplateSnippetForName(symbolKey, collector.request.symbolTable);
            if (snippet.empty() && !scopeNs.empty())
            {
                snippet = TemplateSnippetForName(def.name, collector.request.symbolTable);
            }
        }
    }
    std::string sortText = FormatProximitySortText(prox, def.name);
    AddItemIfNew(collector, {def.name, kind, def.typeName, "", isCallable ? symbolKey : std::string{}, snippet,
                             std::move(sortText)});
}

/**
 * @brief Collects local variable and parameter definitions from innermost scope outward.
 * @param[in] innermostScope Lexical scope at cursor.
 * @param[in,out] collector Completion collector context.
 */
void CollectScopeDefinitions(const analysis::Scope* innermostScope, CompletionCollector& collector)
{
    if (!innermostScope)
    {
        return;
    }
    for (const analysis::Scope* cur = innermostScope; cur != nullptr; cur = cur->parent)
    {
        const bool isLocal = (cur->kind == analysis::ScopeKind::Block || cur->kind == analysis::ScopeKind::Function ||
                              cur->kind == analysis::ScopeKind::Closure);
        const SymbolProximity prox = isLocal ? SymbolProximity::Local : SymbolProximity::CurrentFile;
        const std::string scopeNs = BuildScopeNamespacePath(cur, collector.request);
        for (const auto& def : cur->definitions)
        {
            ProcessScopeDefinition(def, scopeNs, collector, prox);
        }
    }
}

static void CollectEnclosingMemberCandidate(const analysis::Symbol& sym, const std::string& enclosingClassName,
                                            CompletionCollector& collector)
{
    if (sym.containerName != enclosingClassName && sym.containerName != analysis::LastScopeSegment(enclosingClassName))
    {
        return;
    }
    if (sym.type == analysis::SymbolType::Variable && sym.GetVariable().isEnumConstant)
    {
        AddEnumConstantCandidate(collector, sym);
        return;
    }
    lsp::CompletionItemKind kind =
        (sym.type == analysis::SymbolType::Function) ? lsp::CompletionItemKind::Method : lsp::CompletionItemKind::Field;
    std::string snippet;
    std::string detail;
    if (sym.type == analysis::SymbolType::Function)
    {
        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
        const bool completeParens =
            !collector.request.config || collector.request.config->features.completionCompleteFunctionParens;
        if (collector.request.snippetSupport && completeParens)
        {
            snippet = sym.GetFunction().parameters.empty() ? (sym.name + "()$0") : (sym.name + "($0)");
        }
    }
    const auto prox = DetermineSymbolProximity(sym, collector.request.uri, GetPredefinedExtension(collector.request));
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName, snippet, std::move(sortText)});
}

/**
 * @brief Collects member methods and fields of the enclosing class.
 * @param[in,out] collector Completion collector context.
 */
void CollectEnclosingClassMembers(CompletionCollector& collector)
{
    std::string enclosingClassName = FindEnclosingClassName(collector.request);
    if (enclosingClassName.empty())
    {
        return;
    }
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    const auto& cm = ruleIndex->Members(enclosingClassName);
    for (const auto& key : cm.memberKeys)
    {
        const auto symList = collector.request.symbolTable.FindSymbolsPtr(key);
        if (!symList)
        {
            continue;
        }
        for (const auto& sym : *symList)
        {
            CollectEnclosingMemberCandidate(sym, enclosingClassName, collector);
        }
    }
}

static void CollectGlobalFunctionSymbol(const analysis::Symbol& sym, bool accessorsAreProperties,
                                        bool accessorKeywordRequired, CompletionCollector& collector)
{
    std::string detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
    std::string snippet;
    const bool completeParens =
        !collector.request.config || collector.request.config->features.completionCompleteFunctionParens;
    if (collector.request.snippetSupport && completeParens)
    {
        snippet = CallSnippet(sym.name, sym.GetFunction().parameters);
    }
    const auto prox = DetermineSymbolProximity(sym, collector.request.uri, GetPredefinedExtension(collector.request));
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, lsp::CompletionItemKind::Function, std::move(detail), "", sym.qualifiedName,
                             std::move(snippet), std::move(sortText)});
    if (accessorsAreProperties)
    {
        const std::string propName = analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
        if (!propName.empty())
        {
            std::string propType = analysis::PropertyTypeFromAccessors(analysis::FindGlobalPropertyAccessors(
                propName, collector.request.symbolTable, accessorKeywordRequired));
            std::string propSortText = FormatProximitySortText(prox, propName);
            AddItemIfNew(collector, {propName, lsp::CompletionItemKind::Property, std::move(propType), "",
                                     sym.qualifiedName, "", std::move(propSortText)});
        }
    }
}

/**
 * @brief Converts a global symbol table symbol into a completion item.
 * @param[in] sym Target global symbol.
 * @param[in] accessorsAreProperties Whether property accessors are exposed.
 * @param[in] accessorKeywordRequired Whether property keyword is required.
 * @param[in,out] collector Completion collector context.
 */
void CollectGlobalSymbolItem(const analysis::Symbol& sym, bool accessorsAreProperties, bool accessorKeywordRequired,
                             CompletionCollector& collector)
{
    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
    std::string detail;
    std::string snippet;
    switch (sym.type)
    {
    case analysis::SymbolType::Function:
        CollectGlobalFunctionSymbol(sym, accessorsAreProperties, accessorKeywordRequired, collector);
        return;
    case analysis::SymbolType::Class:
        kind = lsp::CompletionItemKind::Class;
        if (collector.request.snippetSupport)
        {
            snippet = TemplateInsertSnippet(sym);
        }
        if (!snippet.empty())
        {
            detail = sym.name + "<" + std::get<analysis::ClassSignature>(sym.signature).templateParams.front() + ">";
        }
        break;
    case analysis::SymbolType::Interface:
        kind = lsp::CompletionItemKind::Interface;
        break;
    case analysis::SymbolType::Enum:
        kind = lsp::CompletionItemKind::Enum;
        break;
    case analysis::SymbolType::Typedef:
    case analysis::SymbolType::Funcdef:
        kind = lsp::CompletionItemKind::TypeParameter;
        break;
    case analysis::SymbolType::Namespace:
        kind = lsp::CompletionItemKind::Module;
        break;
    case analysis::SymbolType::Variable:
        if (sym.GetVariable().isEnumConstant)
        {
            AddEnumConstantCandidate(collector, sym);
            return;
        }
        kind = lsp::CompletionItemKind::Variable;
        detail = sym.GetVariable().typeName;
        break;
    case analysis::SymbolType::Property:
        kind = lsp::CompletionItemKind::Property;
        break;
    default:
        break;
    }
    const auto prox = DetermineSymbolProximity(sym, collector.request.uri, GetPredefinedExtension(collector.request));
    std::string sortText = FormatProximitySortText(prox, sym.name);
    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName, snippet, std::move(sortText)});
}

/**
 * @brief Extracts the trailing identifier query prefix from a line prefix.
 * @param[in] linePrefix Line text up to the cursor position.
 * @return Extracted identifier query prefix, or empty if none.
 */
std::string_view ExtractQueryPrefix(std::string_view linePrefix)
{
    size_t i = linePrefix.size();
    while (i > 0 && (std::isalnum(static_cast<unsigned char>(linePrefix[i - 1])) || linePrefix[i - 1] == '_'))
    {
        --i;
    }
    return linePrefix.substr(i);
}

/**
 * @brief Checks if a symbol table bucket can contain symbols matching queryPrefix.
 * @param[in] qualifiedName Symbol table bucket key.
 * @param[in] queryPrefix User query prefix.
 * @param[in] accessorsAreProperties Whether get_/set_ accessors can synthesize properties.
 * @return True if bucket should be inspected.
 */
bool BucketMatchesPrefix(const std::string& qualifiedName, std::string_view queryPrefix, bool accessorsAreProperties)
{
    if (queryPrefix.empty() || angel_lsp::utils::CaseInsensitiveStartsWith(qualifiedName, queryPrefix))
    {
        return true;
    }
    if (accessorsAreProperties)
    {
        if (angel_lsp::utils::CaseInsensitiveStartsWith(qualifiedName, "get_") &&
            angel_lsp::utils::CaseInsensitiveStartsWith(std::string_view(qualifiedName).substr(4), queryPrefix))
        {
            return true;
        }
        if (angel_lsp::utils::CaseInsensitiveStartsWith(qualifiedName, "set_") &&
            angel_lsp::utils::CaseInsensitiveStartsWith(std::string_view(qualifiedName).substr(4), queryPrefix))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Collects all global scope symbols from the symbol table.
 * @param[in] accessorsAreProperties Whether property accessors are exposed.
 * @param[in] accessorKeywordRequired Whether property keyword is required.
 * @param[in] queryPrefix Prefix to filter symbol table buckets by.
 * @param[in,out] collector Completion collector context.
 */
void CollectGlobalSymbols(bool accessorsAreProperties, bool accessorKeywordRequired, std::string_view queryPrefix,
                          CompletionCollector& collector)
{
    collector.request.symbolTable.ForEachSymbolWithPrefix(
        queryPrefix,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            if (!BucketMatchesPrefix(qualifiedName, queryPrefix, accessorsAreProperties))
            {
                return;
            }
            for (const auto& sym : symList)
            {
                if (sym.containerName.empty() && sym.type != analysis::SymbolType::CallReference)
                {
                    CollectGlobalSymbolItem(sym, accessorsAreProperties, accessorKeywordRequired, collector);
                }
            }
        });

    if (collector.request.findModuleSymbols)
    {
        for (const auto& [name, detail] : collector.request.findModuleSymbols(queryPrefix))
        {
            AddItemIfNew(collector, {name, lsp::CompletionItemKind::Module, detail, "", name});
        }
    }
}

/**
 * @brief Collects symbols from enclosing namespaces and active `using namespace` directives.
 * @param[in] innermostScope Innermost lexical scope at cursor.
 * @param[in] queryPrefix Prefix to filter candidate symbol names by.
 * @param[in,out] collector Completion collector context.
 */
void CollectVisibleNamespaceSymbols(const analysis::Scope* innermostScope, std::string_view queryPrefix,
                                    CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    const int accessorMode = collector.request.config ? collector.request.config->engine.propertyAccessorMode : 2;
    const bool accessorsAreProperties = accessorMode >= 2;
    const bool accessorKeywordRequired = accessorMode == 3;
    for (const auto& ns : CollectVisibleNamespaces(collector.request, innermostScope))
    {
        const auto& cm = ruleIndex->Members(ns);
        for (const auto& key : cm.memberKeys)
        {
            const auto symList = collector.request.symbolTable.FindSymbolsPtr(key);
            if (!symList)
            {
                continue;
            }
            for (const auto& sym : *symList)
            {
                if (sym.containerName == ns && sym.type != analysis::SymbolType::CallReference &&
                    BucketMatchesPrefix(sym.name, queryPrefix, accessorsAreProperties))
                {
                    CollectGlobalSymbolItem(sym, accessorsAreProperties, accessorKeywordRequired, collector);
                }
            }
        }
    }
}

/**
 * @brief Inserts statement and declaration snippet completions.
 * @param[in,out] collector Completion collector context.
 */
void CollectDeclarationSnippets(CompletionCollector& collector)
{
    if (!collector.request.snippetSupport)
    {
        return;
    }
    const std::pair<const char*, const char*> declarationSnippets[] = {
        {"class", "class ${1:Name}\n{\n    ${1:Name}()\n    {\n        $0\n    }\n\n    ~${1:Name}()\n    {\n    }\n}"},
        {"interface", "interface ${1:Name}\n{\n    void ${2:DoThing}();\n}"},
        {"mixin", "mixin class ${1:Name}\n{\n    $0\n}"},
        {"function", "function(${1:int value})\n{\n    $0\n}"},
        {"enum", "enum ${1:Name}\n{\n    ${2:Member} = 0\n}"},
        {"funcdef", "funcdef ${1:void} ${2:Name}(${3:int value});"},
        {"switch", "switch (${1:value})\n{\ncase ${2:0}:\n    $0\n    break;\n\ndefault:\n    break;\n}"},
        {"if", "if (${1:condition})\n{\n    $0\n}"},
        {"else", "else\n{\n    $0\n}"},
        {"for", "for (uint ${1:i} = 0; ${1:i} < ${2:count}; ${1:i}++)\n{\n    $0\n}"},
        {"while", "while (${1:condition})\n{\n    $0\n}"},
        {"do", "do\n{\n    $0\n}\nwhile (${1:condition});"},
        {"try", "try\n{\n    $0\n}\ncatch\n{\n}"},
    };

    const std::pair<const char*, const char*> snippetDetails[] = {
        {"class", "declaration, with a constructor and a destructor"},
        {"interface", "declaration, with one method"},
        {"mixin", "mixin class declaration"},
        {"function", "anonymous function, for a funcdef parameter or handle"},
        {"enum", "declaration, with one member"},
        {"funcdef", "function-pointer type declaration"},
        {"switch", "block, with a case and a default"},
        {"if", "block"},
        {"else", "block"},
        {"for", "loop over a counter"},
        {"while", "loop"},
        {"do", "loop, with the test at the end"},
        {"try", "block, with its catch"},
    };

    for (size_t i = 0; i < std::size(declarationSnippets); ++i)
    {
        lsp::CompletionItem item;
        item.label = declarationSnippets[i].first;
        item.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::Snippet);
        item.insertText = declarationSnippets[i].second;
        item.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
        item.sortText = FormatProximitySortText(SymbolProximity::Snippet, declarationSnippets[i].first);
        item.detail = snippetDetails[i].second;
        collector.items.push_back(std::move(item));
    }

    lsp::CompletionItem include;
    include.label = "#include";
    include.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::Snippet);
    include.insertText = "#include \"$1\"";
    include.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
    include.sortText = FormatProximitySortText(SymbolProximity::Snippet, "#include");
    include.detail = "directive, with the path left open";
    collector.items.push_back(std::move(include));
}

/**
 * @brief Inserts keyword completions into collector.
 * @param[in,out] collector Completion collector context.
 */
void CollectKeywords(CompletionCollector& collector)
{
    for (const auto& kw : GetKeywords())
    {
        AddItemIfNew(collector, {kw, lsp::CompletionItemKind::Keyword, "", "", "", "",
                                 FormatProximitySortText(SymbolProximity::Keyword, kw)});
    }
}

/**
 * @brief Attempts to extract the active call and parameter index before the cursor.
 * @param[in] prefix Text before cursor on current line.
 * @param[out] outCallee Extracted callee expression text.
 * @param[out] outArgIndex Argument index (0-based).
 * @return True if cursor is inside a call argument list.
 */
static size_t FindUnclosedCallParen(std::string_view prefix)
{
    int depth = 0;
    for (size_t i = prefix.size(); i > 0; --i)
    {
        char c = prefix[i - 1];
        if (c == ')')
        {
            ++depth;
        }
        else if (c == '(')
        {
            if (depth == 0)
            {
                return i - 1;
            }
            --depth;
        }
    }
    return std::string_view::npos;
}

static size_t CountCallArguments(std::string_view prefix, size_t openParenPos)
{
    size_t argIndex = 0;
    int commaDepth = 0;
    for (size_t i = openParenPos + 1; i < prefix.size(); ++i)
    {
        char c = prefix[i];
        if (c == '(' || c == '[' || c == '{')
        {
            ++commaDepth;
        }
        else if (c == ')' || c == ']' || c == '}')
        {
            if (commaDepth > 0)
            {
                --commaDepth;
            }
        }
        else if (c == ',' && commaDepth == 0)
        {
            ++argIndex;
        }
    }
    return argIndex;
}

static std::string ExtractCalleeName(std::string_view prefix, size_t openParenPos)
{
    size_t endCallee = openParenPos;
    while (endCallee > 0 && isspace(static_cast<unsigned char>(prefix[endCallee - 1])))
    {
        --endCallee;
    }
    size_t startCallee = endCallee;
    while (startCallee > 0)
    {
        char ch = prefix[startCallee - 1];
        if (isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == ':' || ch == '.')
        {
            --startCallee;
        }
        else
        {
            break;
        }
    }
    if (startCallee >= endCallee)
    {
        return "";
    }
    return std::string(prefix.substr(startCallee, endCallee - startCallee));
}

bool ExtractCallContext(std::string_view prefix, std::string& outCallee, size_t& outArgIndex)
{
    if (prefix.empty())
    {
        return false;
    }
    size_t openParenPos = FindUnclosedCallParen(prefix);
    if (openParenPos == std::string_view::npos)
    {
        return false;
    }

    outCallee = ExtractCalleeName(prefix, openParenPos);
    outArgIndex = CountCallArguments(prefix, openParenPos);
    return !outCallee.empty();
}

/**
 * @brief Resolves the expected parameter type for a callee at the given argument index.
 * @param[in] callee Callee identifier text.
 * @param[in] argIndex Argument index.
 * @param[in] request Completion request context.
 * @param[in] scope Innermost scope at cursor.
 * @return Resolved parameter type name, or empty string.
 */
static std::vector<analysis::Symbol>
ResolveMemberCalleeCandidates(const std::string& callee, const CompletionRequest& request, const analysis::Scope* scope)
{
    std::vector<analysis::Symbol> candidates;
    size_t dotPos = callee.rfind('.');
    std::string objText = callee.substr(0, dotPos);
    std::string memText = callee.substr(dotPos + 1);
    std::string rType = ResolveNamedReceiverType(objText, request, scope);
    if (!rType.empty())
    {
        std::string cleanType = analysis::StripTypeDecorations(rType);
        auto hier = GetInheritedTypeHierarchy(request.symbolTable, cleanType);
        for (const auto& cls : hier)
        {
            auto syms = request.symbolTable.FindSymbols(cls + "::" + memText);
            candidates.insert(candidates.end(), syms.begin(), syms.end());
        }
    }
    return candidates;
}

static std::vector<analysis::Symbol> ResolveUnqualifiedCalleeCandidates(const std::string& callee,
                                                                        const CompletionRequest& request,
                                                                        const analysis::Scope* scope)
{
    if (scope && request.tree)
    {
        TSNode root = ts_tree_root_node(request.tree);
        auto candidates = analysis::FindSymbolsInScope(callee, root, request.sourceCode, request.symbolTable);
        if (!candidates.empty())
        {
            return candidates;
        }
    }
    return request.symbolTable.FindSymbols(callee);
}

/**
 * @brief Finds the expected parameter type of a callee function at the given argument index.
 * @param[in] callee Callee name or member expression string.
 * @param[in] argIndex Index of the target argument.
 * @param[in] request Completion request.
 * @param[in] scope Lexical scope at cursor.
 * @return Resolved parameter type name, or empty string.
 */
std::string FindCalleeParameterType(const std::string& callee, size_t argIndex, const CompletionRequest& request,
                                    const analysis::Scope* scope)
{
    auto candidates = (callee.find('.') != std::string::npos)
                          ? ResolveMemberCalleeCandidates(callee, request, scope)
                          : ResolveUnqualifiedCalleeCandidates(callee, request, scope);

    for (const auto& sym : candidates)
    {
        if (sym.type == analysis::SymbolType::Function &&
            std::holds_alternative<analysis::FunctionSignature>(sym.signature))
        {
            const auto& params = sym.GetFunction().parameters;
            if (argIndex < params.size())
            {
                return params[argIndex].typeName;
            }
        }
    }
    return "";
}

static bool IsCompoundOrComparisonEquals(char prev, char next)
{
    if (next == '=')
    {
        return true;
    }
    constexpr std::string_view kOps = "=!<>+-*/%&|^";
    return kOps.find(prev) != std::string_view::npos;
}

static bool IsTypeChar(char ch)
{
    return isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '@' || ch == '&' || ch == '<' || ch == '>' ||
           ch == ':';
}

static std::string_view TrimTrailingWhitespace(std::string_view s)
{
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
    {
        s.remove_suffix(1);
    }
    return s;
}

static size_t FindIdentifierStart(std::string_view s, size_t end)
{
    size_t start = end;
    while (start > 0)
    {
        char ch = s[start - 1];
        if (!isalnum(static_cast<unsigned char>(ch)) && ch != '_')
        {
            break;
        }
        --start;
    }
    return start;
}

static size_t FindTypeStart(std::string_view s, size_t end)
{
    size_t start = end;
    while (start > 0 && IsTypeChar(s[start - 1]))
    {
        --start;
    }
    return start;
}

/**
 * @brief Extracts expected target type from declaration assignment before cursor.
 * @param[in] prefix Text before cursor on current line.
 * @return Extracted target type name, or empty string.
 */
std::string ExtractAssignmentTargetType(std::string_view prefix)
{
    size_t eqPos = prefix.rfind('=');
    if (eqPos == std::string_view::npos || eqPos == 0)
    {
        return "";
    }
    char prev = prefix[eqPos - 1];
    char next = (eqPos + 1 < prefix.size()) ? prefix[eqPos + 1] : ' ';
    if (IsCompoundOrComparisonEquals(prev, next))
    {
        return "";
    }

    std::string_view lhs = TrimTrailingWhitespace(prefix.substr(0, eqPos));
    size_t idEnd = lhs.size();
    size_t idStart = FindIdentifierStart(lhs, idEnd);
    if (idStart == idEnd || idStart == 0)
    {
        return "";
    }
    std::string_view typePart = TrimTrailingWhitespace(lhs.substr(0, idStart));
    size_t typeEnd = typePart.size();
    size_t typeStart = FindTypeStart(typePart, typeEnd);
    if (typeStart >= typeEnd)
    {
        return "";
    }
    return std::string(typePart.substr(typeStart, typeEnd - typeStart));
}

/**
 * @brief Checks whether a type name represents a numeric primitive.
 * @param[in] t Type name to inspect.
 * @return True if type is a numeric primitive.
 */
bool IsNumericTypeName(std::string_view t)
{
    return t == "int" || t == "int8" || t == "int16" || t == "int32" || t == "int64" || t == "uint" || t == "uint8" ||
           t == "uint16" || t == "uint32" || t == "uint64" || t == "float" || t == "double";
}

/**
 * @brief Categorization rank for contextual type-aware completion sorting.
 */
enum class TypeMatchRank : uint8_t
{
    Exact = 0,
    Convertible = 1,
    Other = 2
};

/**
 * @brief Formats sort text prefix for a given type match rank.
 * @param[in] rank Computed type match rank.
 * @param[in] baseSort Base sort key or label.
 * @return Formatted sortText string with bucket prefix.
 */
inline std::string FormatRankedSortText(TypeMatchRank rank, std::string_view baseSort)
{
    static constexpr std::array<std::string_view, 3> k_rankPrefixes = {"0000_", "0001_", "0002_"};
    const size_t idx = static_cast<size_t>(rank);
    if (idx < k_rankPrefixes.size())
    {
        return std::string(k_rankPrefixes[idx]) + std::string(baseSort);
    }
    return std::string("9999_") + std::string(baseSort);
}

/**
 * @brief Computes type ranking score (Exact match, Convertible, or Other).
 * @param[in] item Completion item to rank.
 * @param[in] expectedType Target expected type.
 * @param[in] symbolTable Global symbol table.
 * @return TypeMatchRank tier.
 */
TypeMatchRank ComputeTypeRank(const lsp::CompletionItem& item, const std::string& expectedType,
                              const analysis::SymbolTable& symbolTable)
{
    std::string cleanExpected = analysis::StripTypeDecorations(expectedType);
    std::string itemType = item.detail.has_value() ? *item.detail : "";
    std::string cleanItem = analysis::StripTypeDecorations(itemType);

    if (!cleanExpected.empty())
    {
        if (item.kind.has_value() && static_cast<int>(*item.kind) == static_cast<int>(lsp::CompletionItemKind::Class) &&
            item.label == cleanExpected)
        {
            return TypeMatchRank::Exact;
        }
        if (!cleanItem.empty() && cleanItem == cleanExpected)
        {
            return TypeMatchRank::Exact;
        }
    }

    if (IsNumericTypeName(cleanExpected) && IsNumericTypeName(cleanItem))
    {
        return TypeMatchRank::Convertible;
    }
    if (!cleanExpected.empty() && !cleanItem.empty())
    {
        auto hier = GetInheritedTypeHierarchy(symbolTable, cleanItem);
        for (const auto& base : hier)
        {
            if (base == cleanExpected)
            {
                return TypeMatchRank::Convertible;
            }
        }
    }

    return TypeMatchRank::Other;
}

/**
 * @brief Applies smart type-aware ranking to completion items based on active context.
 * @param[in,out] items Completion item list to rank and sort.
 * @param[in] prefix Text before cursor on current line.
 * @param[in] scope Innermost scope at cursor.
 * @param[in] request Completion request context.
 */
void ApplySmartTypeRanking(std::vector<lsp::CompletionItem>& items, std::string_view prefix,
                           const analysis::Scope* scope, const CompletionRequest& request)
{
    std::string callee;
    size_t argIndex = 0;
    std::string expectedType;
    if (ExtractCallContext(prefix, callee, argIndex))
    {
        expectedType = FindCalleeParameterType(callee, argIndex, request, scope);
    }
    if (expectedType.empty())
    {
        expectedType = ExtractAssignmentTargetType(prefix);
    }
    if (expectedType.empty())
    {
        return;
    }

    for (auto& item : items)
    {
        const TypeMatchRank rank = ComputeTypeRank(item, expectedType, request.symbolTable);
        const std::string_view baseSort = item.sortText.has_value() ? *item.sortText : item.label;
        item.sortText = FormatRankedSortText(rank, baseSort);
    }
}

/**
 * @brief Sorts completion items deterministically according to their sortText and label.
 * @param[in,out] items Vector of completion items to sort.
 */
inline void SortCompletionItemsByProximity(std::vector<lsp::CompletionItem>& items)
{
    std::stable_sort(items.begin(), items.end(),
                     [](const lsp::CompletionItem& a, const lsp::CompletionItem& b)
                     {
                         const std::string& sa = a.sortText.has_value() ? *a.sortText : a.label;
                         const std::string& sb = b.sortText.has_value() ? *b.sortText : b.label;
                         return sa < sb;
                     });
}

} // namespace

std::vector<lsp::CompletionItem> GetCompletion(const CompletionRequest& request)
{
    std::string prefix = GetLinePrefix(request.sourceCode, request.position.line, request.position.character);
    size_t trailingColons = 0;
    while (trailingColons < prefix.size() && prefix[prefix.size() - 1 - trailingColons] == ':')
    {
        ++trailingColons;
    }
    if (trailingColons > 0 && trailingColons != 2)
    {
        return {};
    }

    if (auto earlyItems = HandleIncludeOrLexicalSuppression(request, prefix))
    {
        return *earlyItems;
    }

    std::vector<lsp::CompletionItem> items;
    std::unordered_set<std::string> seenLabels;
    CompletionCollector collector{items, seenLabels, request};

    if (TryCompleteScopeResolution(prefix, collector))
    {
        SortCompletionItemsByProximity(items);
        return items;
    }

    if (TryCompleteTemplateArguments(prefix, collector))
    {
        SortCompletionItemsByProximity(items);
        return items;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* innermostScope =
        rootScope ? FindInnermostScope(rootScope.get(), request.position.line, request.position.character) : nullptr;

    if (TryCompleteMemberAccess(prefix, innermostScope, collector))
    {
        SortCompletionItemsByProximity(items);
        return items;
    }

    const int accessorMode = request.config ? request.config->engine.propertyAccessorMode : 2;
    const bool accessorsAreProperties = accessorMode >= 2;
    const bool accessorKeywordRequired = accessorMode == 3;

    CollectScopeDefinitions(innermostScope, collector);
    CollectEnclosingClassMembers(collector);
    const std::string_view queryPrefix = ExtractQueryPrefix(prefix);
    CollectVisibleNamespaceSymbols(innermostScope, queryPrefix, collector);
    CollectGlobalSymbols(accessorsAreProperties, accessorKeywordRequired, queryPrefix, collector);
    CollectDeclarationSnippets(collector);
    CollectKeywords(collector);

    const bool enableSmartRanking = !request.config || request.config->features.completionSmartTypeRanking;
    if (enableSmartRanking)
    {
        ApplySmartTypeRanking(items, prefix, innermostScope, request);
    }
    SortCompletionItemsByProximity(items);

    return items;
}

lsp::CompletionItem ResolveCompletionItem(const CompletionResolveRequest& request)
{
    lsp::CompletionItem resolved = request.item;

    if (resolved.documentation.has_value() || !resolved.data.has_value() || !resolved.data->isString())
    {
        return resolved;
    }

    const std::string qualifiedName = resolved.data->string();
    if (qualifiedName.empty() || !request.readDocument)
    {
        return resolved;
    }

    auto syms = request.symbolTable.FindSymbolsPtr(qualifiedName);
    if (syms)
    {
        for (const auto& symbol : *syms)
        {
            const std::string* text = request.readDocument(symbol.fileUri);
            if (!text)
            {
                continue;
            }

            const std::string documentation = analysis::ExtractDocComment(*text, symbol.startLine);
            if (documentation.empty())
            {
                continue;
            }

            resolved.documentation = lsp::MarkupContent{
                .kind = lsp::MarkupKind::Markdown,
                .value = documentation,
            };
            return resolved;
        }
    }

    return resolved;
}

} // namespace angel_lsp::features
