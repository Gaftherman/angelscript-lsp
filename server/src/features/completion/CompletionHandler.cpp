#include "features/completion/CompletionHandler.h"
#include "analysis/DocComment.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SignatureFormatter.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
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

    const size_t at = typeName.rfind("::");
    const std::string_view shortName =
        at == std::string::npos ? std::string_view(typeName) : std::string_view(typeName).substr(at + 2);

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
 * @brief Segment in a parsed member access chain (e.g., `obj.prop[0].method()`).
 */
struct AccessSegment
{
    std::string name;
    bool isCall = false;
    size_t indexCount = 0;
};

/**
 * @brief Advances index past whitespace characters.
 * @param[in] chain Access chain view.
 * @param[in,out] i Cursor index.
 */
inline void SkipWhitespace(std::string_view chain, size_t& i)
{
    while (i < chain.size() && (chain[i] == ' ' || chain[i] == '\t'))
    {
        ++i;
    }
}

/**
 * @brief Consumes an alphanumeric identifier from the chain.
 * @param[in] chain Access chain view.
 * @param[in,out] i Cursor index.
 * @return Extracted identifier, or empty string.
 */
std::string ConsumeIdentifier(std::string_view chain, size_t& i)
{
    if (i >= chain.size() || (!isalpha(static_cast<unsigned char>(chain[i])) && chain[i] != '_'))
    {
        return "";
    }
    size_t start = i;
    while (i < chain.size() && (isalnum(static_cast<unsigned char>(chain[i])) || chain[i] == '_'))
    {
        ++i;
    }
    return std::string(chain.substr(start, i - start));
}

/**
 * @brief Consumes subsequent call arguments `(...)` or indexing brackets `[...]`.
 * @param[in] chain Access chain view.
 * @param[in,out] i Cursor index.
 * @param[in,out] seg Current access segment being populated.
 */
void ConsumeParenthesesOrBrackets(std::string_view chain, size_t& i, AccessSegment& seg)
{
    while (i < chain.size() && (chain[i] == '(' || chain[i] == '['))
    {
        char open = chain[i];
        char close = (open == '(') ? ')' : ']';
        if (open == '(')
        {
            seg.isCall = true;
        }
        else
        {
            ++seg.indexCount;
        }
        ++i;
        int depth = 1;
        while (i < chain.size() && depth > 0)
        {
            if (chain[i] == open)
            {
                ++depth;
            }
            else if (chain[i] == close)
            {
                --depth;
            }
            ++i;
        }
    }
}

/**
 * @brief Consumes a member access delimiter (`.` or `->`).
 * @param[in] chain Access chain view.
 * @param[in,out] i Cursor index.
 * @return True if delimiter was matched and consumed.
 */
bool ConsumeDelimiter(std::string_view chain, size_t& i)
{
    if (i < chain.size() && chain[i] == '.')
    {
        ++i;
        return true;
    }
    if (i + 1 < chain.size() && chain[i] == '-' && chain[i + 1] == '>')
    {
        i += 2;
        return true;
    }
    return false;
}

/**
 * @brief Parses a chained member access expression into individual segments.
 * @param[in] chain Access chain string view.
 * @return Vector of parsed access segments.
 */
std::vector<AccessSegment> ParseAccessChain(std::string_view chain)
{
    std::vector<AccessSegment> segments;
    size_t i = 0;
    while (i < chain.size())
    {
        SkipWhitespace(chain, i);
        if (i >= chain.size())
        {
            break;
        }
        std::string name = ConsumeIdentifier(chain, i);
        if (name.empty())
        {
            break;
        }
        AccessSegment seg;
        seg.name = std::move(name);
        ConsumeParenthesesOrBrackets(chain, i, seg);
        SkipWhitespace(chain, i);
        if (!ConsumeDelimiter(chain, i))
        {
            break;
        }
        segments.push_back(std::move(seg));
    }
    return segments;
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
 * @brief Adds a completion item to collector if its label has not yet been offered.
 * @param[in,out] collector Completion collector context.
 * @param[in] candidate Completion item candidate attributes.
 */
void AddItemIfNew(CompletionCollector& collector, CompletionCandidate candidate)
{
    if (candidate.label.empty() || collector.seenLabels.contains(candidate.label))
    {
        return;
    }
    collector.seenLabels.insert(candidate.label);

    lsp::CompletionItem item;
    item.label = std::move(candidate.label);
    item.kind = lsp::CompletionItemKindEnum(candidate.kind);
    if (!candidate.snippet.empty())
    {
        item.insertText = std::move(candidate.snippet);
        item.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
    }
    if (!candidate.sortText.empty())
    {
        item.sortText = std::move(candidate.sortText);
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
 * @brief Static array of primitive type names.
 * @return Constant reference to vector of primitive strings.
 */
const std::vector<std::string>& GetPrimitiveTypeNames()
{
    static const std::vector<std::string> primitives = []
    {
        std::vector<std::string> all;
        all.reserve(parser::primitives::k_all.size() + 3);
        for (const std::string_view name : parser::primitives::k_all)
        {
            all.emplace_back(name);
        }
        all.emplace_back("string");
        all.emplace_back("array");
        all.emplace_back("dictionary");
        return all;
    }();
    return primitives;
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
    auto enumMatches = collector.request.symbolTable.FindSymbolsPtr(qualifier);
    if (enumMatches && !enumMatches->empty())
    {
        for (const auto& sym : *enumMatches)
        {
            if (sym.type == analysis::SymbolType::Enum)
            {
                for (const auto& mem : sym.GetEnum().members)
                {
                    AddItemIfNew(collector,
                                 {mem.name, lsp::CompletionItemKind::EnumMember, qualifier + "::" + mem.name});
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
                for (const auto& mem : sym.GetEnum().members)
                {
                    AddItemIfNew(collector,
                                 {mem.name, lsp::CompletionItemKind::EnumMember, qualifier + "::" + mem.name});
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
void CollectContainerMembersUnderQualifier(const std::string& qualifier, CompletionCollector& collector)
{
    const auto ruleIndex = collector.request.symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    auto addMembersOf = [&](const std::string& container)
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
                    if (sym.type == analysis::SymbolType::Function)
                    {
                        kind = lsp::CompletionItemKind::Function;
                        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
                    }
                    else if (sym.type == analysis::SymbolType::Variable)
                    {
                        kind = lsp::CompletionItemKind::Variable;
                        detail = sym.GetVariable().typeName;
                    }
                    else if (sym.type == analysis::SymbolType::Class)
                    {
                        kind = lsp::CompletionItemKind::Class;
                    }
                    else if (sym.type == analysis::SymbolType::Enum)
                    {
                        kind = lsp::CompletionItemKind::Enum;
                    }
                    AddItemIfNew(collector, {sym.name, kind, std::move(detail), "", sym.qualifiedName});
                }
            }
        }
    };

    addMembersOf(qualifier);
    if (qualifier.find("::") == std::string::npos)
    {
        auto qit = ruleIndex->qualifiedTypesByShortName.find(qualifier);
        if (qit != ruleIndex->qualifiedTypesByShortName.end())
        {
            for (const auto& q : qit->second)
            {
                addMembersOf(q);
            }
        }
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
    static const std::regex scopeResolutionRegex(R"(([a-zA-Z_][a-zA-Z0-9_]*)::([a-zA-Z_][a-zA-Z0-9_]*)?$)");
    std::smatch scopeMatch;
    if (!std::regex_search(prefix, scopeMatch, scopeResolutionRegex))
    {
        return false;
    }
    std::string qualifier = scopeMatch[1].str();
    CollectEnumMembersUnderQualifier(qualifier, collector);
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
    collector.request.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (!sym.containerName.empty() || !IsTypeSymbol(sym))
                {
                    continue;
                }
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
                const std::string snippet =
                    collector.request.snippetSupport ? TemplateInsertSnippet(sym) : std::string{};
                AddItemIfNew(collector, {sym.name, kind, "", "", sym.qualifiedName, snippet});
            }
        });

    for (const auto& primitive : GetPrimitiveTypeNames())
    {
        AddItemIfNew(collector, {primitive, lsp::CompletionItemKind::Keyword});
    }
    return true;
}

/**
 * @brief Resolves the type of the `this` keyword at current position.
 * @param[in] request Completion request context.
 * @return Enclosing class or interface name.
 */
std::string ResolveThisSegmentType(const CompletionRequest& request)
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
                return c.name;
            }
        }
    }
    std::string rawTypeName;
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
                        rawTypeName = sym.name;
                        return;
                    }
                }
            }
        });
    return rawTypeName;
}

/**
 * @brief Resolves base type name from local scope or in-scope AST declarations.
 * @param[in] seg0 Initial access segment.
 * @param[in] request Completion request.
 * @param[in] innermostScope Enclosing lexical scope.
 * @return Resolved type name, or empty string.
 */
std::string ResolveASTOrScopeBaseType(const AccessSegment& seg0, const CompletionRequest& request,
                                      const analysis::Scope* innermostScope)
{
    if (innermostScope)
    {
        const analysis::LocalDefinition* def = analysis::ResolveInScope(innermostScope, seg0.name);
        if (def && !def->typeName.empty())
        {
            return def->typeName;
        }
    }
    if (request.tree)
    {
        TSNode rootNode = ts_tree_root_node(request.tree);
        TSPoint pt{request.position.line, request.position.character};
        TSNode curNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
        auto inScopeSyms = analysis::FindSymbolsInScope(seg0.name, curNode, request.sourceCode, request.symbolTable);
        for (const auto& sym : inScopeSyms)
        {
            if ((sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property) &&
                !sym.GetVariable().typeName.empty())
            {
                return sym.GetVariable().typeName;
            }
            if (sym.type == analysis::SymbolType::Function && seg0.isCall)
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    return "";
}

/**
 * @brief Resolves base type name from global symbols or property accessors.
 * @param[in] seg0 Initial access segment.
 * @param[in] request Completion request.
 * @return Resolved global type name, or empty string.
 */
std::string ResolveGlobalBaseType(const AccessSegment& seg0, const CompletionRequest& request)
{
    if (auto globSyms = request.symbolTable.FindSymbolsPtr(seg0.name))
    {
        for (const auto& sym : *globSyms)
        {
            if (sym.type == analysis::SymbolType::Variable && sym.containerName.empty() &&
                !sym.GetVariable().typeName.empty())
            {
                return sym.GetVariable().typeName;
            }
        }
    }
    const int accessorMode = request.config ? request.config->engine.propertyAccessorMode : 2;
    if (accessorMode >= 2)
    {
        auto globalAccessors = analysis::FindGlobalPropertyAccessors(seg0.name, request.symbolTable, accessorMode == 3);
        if (!globalAccessors.empty())
        {
            return analysis::PropertyTypeFromAccessors(globalAccessors);
        }
    }
    if (seg0.isCall)
    {
        if (auto fnSyms = request.symbolTable.FindSymbolsPtr(seg0.name))
        {
            for (const auto& sym : *fnSyms)
            {
                if (sym.type == analysis::SymbolType::Function && sym.containerName.empty())
                {
                    return sym.GetFunction().returnType;
                }
            }
        }
    }
    return "";
}

/**
 * @brief Resolves type name for the base segment in a chained member access expression.
 * @param[in] seg0 Initial access segment.
 * @param[in] request Completion request.
 * @param[in] innermostScope Enclosing lexical scope.
 * @return Resolved raw type name string.
 */
std::string ResolveBaseSegmentType(const AccessSegment& seg0, const CompletionRequest& request,
                                   const analysis::Scope* innermostScope)
{
    if (seg0.name == "this")
    {
        return ResolveThisSegmentType(request);
    }
    std::string typeName = ResolveASTOrScopeBaseType(seg0, request, innermostScope);
    if (!typeName.empty())
    {
        return typeName;
    }
    return ResolveGlobalBaseType(seg0, request);
}

/**
 * @brief Resolves return or field type of a member in a chained member expression.
 * @param[in] typeName Container type name.
 * @param[in] seg Current access segment.
 * @param[in] symbolTable Global symbol table.
 * @param[in] accessorMode Engine property accessor mode.
 * @return Next type name string.
 */
std::string ResolveNextChainedType(const std::string& typeName, const AccessSegment& seg,
                                   const analysis::SymbolTable& symbolTable, int accessorMode)
{
    auto memberSyms = symbolTable.FindSymbolsPtr(typeName + "::" + seg.name);
    if (memberSyms)
    {
        for (const auto& sym : *memberSyms)
        {
            if (sym.type == analysis::SymbolType::Variable)
            {
                return sym.GetVariable().typeName;
            }
            if (sym.type == analysis::SymbolType::Function && seg.isCall)
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    if (accessorMode >= 2)
    {
        auto accessors = analysis::FindPropertyAccessors(typeName, seg.name, symbolTable, accessorMode == 3);
        if (!accessors.empty())
        {
            return analysis::PropertyTypeFromAccessors(accessors);
        }
    }
    if (memberSyms)
    {
        for (const auto& sym : *memberSyms)
        {
            if (sym.type == analysis::SymbolType::Function)
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    return "";
}

/**
 * @brief Evaluates subsequent segments in an access chain from left to right.
 * @param[in] segments Vector of parsed access segments.
 * @param[in] rawTypeName Initial type name from base segment.
 * @param[in] request Completion request.
 * @param[in] arrayContainer Standard array container identifier.
 * @return Final resolved type name string.
 */
std::string ResolveChainedSegments(const std::vector<AccessSegment>& segments, std::string rawTypeName,
                                   const CompletionRequest& request, const std::string& arrayContainer)
{
    const int accessorMode = request.config ? request.config->engine.propertyAccessorMode : 2;
    for (size_t s = 1; s < segments.size() && !rawTypeName.empty(); ++s)
    {
        const auto& seg = segments[s];
        std::string cleanType = analysis::CleanBaseType(rawTypeName);
        auto hierarchy = GetInheritedTypeHierarchy(request.symbolTable, cleanType);
        std::string nextTypeName;

        for (const auto& typeName : hierarchy)
        {
            nextTypeName = ResolveNextChainedType(typeName, seg, request.symbolTable, accessorMode);
            if (!nextTypeName.empty())
            {
                break;
            }
        }
        if (nextTypeName.empty())
        {
            return "";
        }
        rawTypeName = analysis::ResolveIndexedType(nextTypeName, seg.indexCount, request.symbolTable, arrayContainer);
    }
    return rawTypeName;
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
    std::string baseContainer = targetTemplate.containerName;
    if (baseContainer.find("::") == std::string::npos && !symbolTable.HasSymbol(baseContainer))
    {
        auto shortMatches = symbolTable.FindTypeSymbolsByShortName(baseContainer);
        for (const auto& sym : shortMatches)
        {
            if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface)
            {
                return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            }
        }
    }
    return baseContainer;
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
    if (sym.type == analysis::SymbolType::Function)
    {
        kind = lsp::CompletionItemKind::Method;
        detail = FormatMethodDetail(sym, tCtx.binding, tCtx.templateArgs);
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

    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName});

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
        AddItemIfNew(collector, {propertyName, lsp::CompletionItemKind::Property, propertyType, "", sym.qualifiedName});
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
 * @brief Attempts to complete member expression following a `.` or `->`.
 * @param[in] prefix Prefix text before cursor.
 * @param[in] innermostScope Lexical scope at cursor.
 * @param[in,out] collector Completion collector context.
 * @return True if member completion completed.
 */
bool TryCompleteMemberAccess(const std::string& prefix, const analysis::Scope* innermostScope,
                             CompletionCollector& collector)
{
    static const std::regex memberChainRegex(
        R"(((?:[a-zA-Z_][a-zA-Z0-9_]*(?:\([^\)]*\)|\[[^\]]*\])*\s*(?:\.|\->)\s*)+)([a-zA-Z_][a-zA-Z0-9_]*)?$)");
    std::smatch memberMatch;
    if (!std::regex_search(prefix, memberMatch, memberChainRegex))
    {
        return false;
    }
    auto segments = ParseAccessChain(memberMatch[1].str());
    if (segments.empty())
    {
        return false;
    }

    std::string arrayContainer = (collector.request.config && !collector.request.config->types.arrayTypeName.empty())
                                     ? collector.request.config->types.arrayTypeName
                                     : "array";

    std::string rawTypeName = ResolveBaseSegmentType(segments[0], collector.request, innermostScope);
    if (!rawTypeName.empty())
    {
        rawTypeName = analysis::ResolveIndexedType(rawTypeName, segments[0].indexCount, collector.request.symbolTable,
                                                   arrayContainer);
    }
    rawTypeName = ResolveChainedSegments(segments, rawTypeName, collector.request, arrayContainer);
    if (rawTypeName.empty())
    {
        return true;
    }

    std::string canonicalType = CanonicalizeArrayType(rawTypeName, arrayContainer);
    std::string baseContainer = FindCanonicalBaseContainer(canonicalType, collector.request.symbolTable);
    auto targetTemplate = analysis::ParseTemplateType(canonicalType);
    const auto binding = analysis::BindTemplateArguments(canonicalType, collector.request.symbolTable);

    auto hierarchy = GetInheritedTypeHierarchy(collector.request.symbolTable, baseContainer);
    for (const auto& typeName : hierarchy)
    {
        PopulateMembersForType(typeName, binding, targetTemplate.templateArgs, collector);
    }
    return true;
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
        for (const auto& def : cur->definitions)
        {
            lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
            bool isCallable = false;
            std::string snippet;
            if (def.kind == analysis::LocalDefinitionKind::Parameter)
            {
                kind = lsp::CompletionItemKind::Variable;
            }
            else if (def.kind == analysis::LocalDefinitionKind::Function ||
                     def.kind == analysis::LocalDefinitionKind::Method)
            {
                kind = lsp::CompletionItemKind::Function;
                isCallable = true;
                if (collector.request.snippetSupport)
                {
                    snippet = CallSnippetForName(def.name, collector.request.symbolTable);
                }
            }
            else if (def.kind == analysis::LocalDefinitionKind::Type)
            {
                kind = lsp::CompletionItemKind::Class;
                if (collector.request.snippetSupport)
                {
                    snippet = TemplateSnippetForName(def.name, collector.request.symbolTable);
                }
            }
            AddItemIfNew(collector, {def.name, kind, def.typeName, "", isCallable ? def.name : std::string{}, snippet});
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
                return c.name;
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
                        className = sym.name;
                        return;
                    }
                }
            }
        });
    return className;
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
            if (sym.containerName == enclosingClassName)
            {
                lsp::CompletionItemKind kind = (sym.type == analysis::SymbolType::Function)
                                                   ? lsp::CompletionItemKind::Method
                                                   : lsp::CompletionItemKind::Field;
                AddItemIfNew(collector, {sym.name, kind});
            }
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
        kind = lsp::CompletionItemKind::Function;
        detail = sym.GetFunction().returnType + " " + sym.name + "(...)";
        if (collector.request.snippetSupport)
        {
            snippet = CallSnippet(sym.name, sym.GetFunction().parameters);
        }
        if (accessorsAreProperties)
        {
            const std::string propName = analysis::PropertyNameFromAccessor(sym, accessorKeywordRequired);
            if (!propName.empty())
            {
                std::string propType = analysis::PropertyTypeFromAccessors(analysis::FindGlobalPropertyAccessors(
                    propName, collector.request.symbolTable, accessorKeywordRequired));
                AddItemIfNew(collector, {propName, lsp::CompletionItemKind::Property, propType, "", sym.qualifiedName});
            }
        }
        break;
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
        kind = lsp::CompletionItemKind::Variable;
        detail = sym.GetVariable().typeName;
        break;
    case analysis::SymbolType::Property:
        kind = lsp::CompletionItemKind::Property;
        break;
    default:
        break;
    }
    AddItemIfNew(collector, {sym.name, kind, detail, "", sym.qualifiedName, snippet});
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
    if (queryPrefix.empty() || qualifiedName.starts_with(queryPrefix))
    {
        return true;
    }
    if (accessorsAreProperties)
    {
        if (qualifiedName.starts_with("get_") && std::string_view(qualifiedName).substr(4).starts_with(queryPrefix))
        {
            return true;
        }
        if (qualifiedName.starts_with("set_") && std::string_view(qualifiedName).substr(4).starts_with(queryPrefix))
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
    collector.request.symbolTable.ForEachSymbol(
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
        item.sortText = std::string("0") + declarationSnippets[i].first;
        item.detail = snippetDetails[i].second;
        collector.items.push_back(std::move(item));
    }

    lsp::CompletionItem include;
    include.label = "#include";
    include.kind = lsp::CompletionItemKindEnum(lsp::CompletionItemKind::Snippet);
    include.insertText = "#include \"$1\"";
    include.insertTextFormat = lsp::InsertTextFormatEnum(lsp::InsertTextFormat::Snippet);
    include.sortText = "0#include";
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
        AddItemIfNew(collector, {kw, lsp::CompletionItemKind::Keyword, "", "", "", "", "1" + kw});
    }
}

} // namespace

std::vector<lsp::CompletionItem> GetCompletion(const CompletionRequest& request)
{
    std::string prefix = GetLinePrefix(request.sourceCode, request.position.line, request.position.character);
    if (auto earlyItems = HandleIncludeOrLexicalSuppression(request, prefix))
    {
        return *earlyItems;
    }

    std::vector<lsp::CompletionItem> items;
    std::unordered_set<std::string> seenLabels;
    CompletionCollector collector{items, seenLabels, request};

    if (TryCompleteScopeResolution(prefix, collector))
    {
        return items;
    }

    if (TryCompleteTemplateArguments(prefix, collector))
    {
        return items;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* innermostScope =
        rootScope ? FindInnermostScope(rootScope.get(), request.position.line, request.position.character) : nullptr;

    if (TryCompleteMemberAccess(prefix, innermostScope, collector))
    {
        return items;
    }

    const int accessorMode = request.config ? request.config->engine.propertyAccessorMode : 2;
    const bool accessorsAreProperties = accessorMode >= 2;
    const bool accessorKeywordRequired = accessorMode == 3;

    CollectScopeDefinitions(innermostScope, collector);
    CollectEnclosingClassMembers(collector);
    const std::string_view queryPrefix = ExtractQueryPrefix(prefix);
    CollectGlobalSymbols(accessorsAreProperties, accessorKeywordRequired, queryPrefix, collector);
    CollectDeclarationSnippets(collector);
    CollectKeywords(collector);

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
