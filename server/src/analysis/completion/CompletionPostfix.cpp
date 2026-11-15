#include "analysis/completion/CompletionInternal.h"
#include "parser/Keywords.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

static bool IsIdentifierChar(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

/**
 * @brief Byte offset of a line and character in the source.
 */
static size_t OffsetOf(std::string_view source, uint32_t line, uint32_t character)
{
    size_t offset = 0;
    for (uint32_t current = 0; current < line && offset < source.size(); ++offset)
    {
        current += source[offset] == '\n' ? 1 : 0;
    }
    return std::min(offset + character, source.size());
}

/**
 * @brief Tells whether the statement under the cursor is a declaration header and not a call in a body.
 * @param[in] innermostScope Scope at the cursor; a function scope opens at its header, so a body is told apart
 *            by a scope that starts before the statement does.
 * @param[in] request Completion query with the source text and cursor.
 * @param[in] statementSize Length of the statement text before the cursor.
 */
static bool IsDeclarationHeader(const analysis::Scope* innermostScope, const CompletionQuery& request,
                                size_t statementSize)
{
    if (innermostScope == nullptr || innermostScope->kind == analysis::ScopeKind::Class ||
        innermostScope->kind == analysis::ScopeKind::Namespace || innermostScope->kind == analysis::ScopeKind::Global)
    {
        return true;
    }
    const size_t cursor = OffsetOf(request.sourceCode, request.position.line, request.position.character);
    const size_t scopeStart = OffsetOf(request.sourceCode, innermostScope->startLine, innermostScope->startCharacter);
    return scopeStart + statementSize >= cursor;
}

/**
 * @brief Reads `name(params) [qualifiers] <partial>` from the statement text before the cursor.
 * @param[in] statement Text since the last `{`, `}` or `;`.
 * @param[out] header Receives the function name, whether it is a constructor, and the qualifiers typed so far.
 * @return False when the text does not end in a function parameter list followed by whitespace or a partial word.
 */
static bool ReadFunctionHeader(std::string_view statement, std::string_view className, PostfixHeader& header)
{
    size_t end = statement.size();
    while (end > 0 && IsIdentifierChar(statement[end - 1]))
    {
        --end;
    }
    // Qualifiers already written between ')' and the cursor, read from the end backwards.
    std::vector<std::string_view> written;
    for (;;)
    {
        size_t cursor = end;
        while (cursor > 0 && std::isspace(static_cast<unsigned char>(statement[cursor - 1])) != 0)
        {
            --cursor;
        }
        // The cursor must follow whitespace, so `T()ov` and a bare `T()` are not signature suffixes.
        if (cursor == end && written.empty())
        {
            return false;
        }
        size_t wordStart = cursor;
        while (wordStart > 0 && IsIdentifierChar(statement[wordStart - 1]))
        {
            --wordStart;
        }
        if (wordStart == cursor)
        {
            end = cursor;
            break;
        }
        written.push_back(statement.substr(wordStart, cursor - wordStart));
        end = wordStart;
    }
    if (end == 0 || statement[end - 1] != ')')
    {
        return false;
    }
    int depth = 0;
    size_t open = end;
    while (open > 0)
    {
        --open;
        if (statement[open] == ')')
        {
            ++depth;
        }
        else if (statement[open] == '(' && --depth == 0)
        {
            break;
        }
    }
    if (depth != 0 || statement[open] != '(')
    {
        return false;
    }
    size_t nameEnd = open;
    while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(statement[nameEnd - 1])) != 0)
    {
        --nameEnd;
    }
    size_t nameStart = nameEnd;
    while (nameStart > 0 && IsIdentifierChar(statement[nameStart - 1]))
    {
        --nameStart;
    }
    if (nameStart == nameEnd)
    {
        return false;
    }
    const bool tilde = nameStart > 0 && statement[nameStart - 1] == '~';
    header.name = std::string(statement.substr(nameStart, nameEnd - nameStart));
    header.written = std::move(written);
    const std::string_view before = statement.substr(0, tilde ? nameStart - 1 : nameStart);
    const bool nothingBefore = before.find_first_not_of(" \t\r\n") == std::string_view::npos;
    if (nothingBefore && header.name == className)
    {
        header.owner = tilde ? PostfixOwner::Destructor : PostfixOwner::Constructor;
    }
    return true;
}

/**
 * @brief Tells whether a base type of the enclosing class declares a member named like the function.
 * @param[in] request Completion query with the symbol table.
 * @param[in] className Qualified name of the enclosing class.
 * @param[in] memberName Method name found in the header.
 */
static bool BaseDeclaresMember(const CompletionQuery& request, std::string_view className, std::string_view memberName)
{
    for (const std::string& base : GetInheritedTypeHierarchy(request.symbolTable, className))
    {
        const auto members = request.symbolTable.FindMemberSymbolPtr(base, memberName);
        if (members && !members->empty() && base != className)
        {
            return true;
        }
    }
    return false;
}

bool TryCompletePostfixQualifiers(const analysis::Scope* innermostScope, CompletionCollector& collector)
{
    const CompletionQuery& request = collector.request;
    const std::string statement =
        GetBackwardDeclarationText(request.sourceCode, request.position.line, request.position.character);
    if (!IsDeclarationHeader(innermostScope, request, statement.size()))
    {
        return false;
    }
    const std::string className = FindEnclosingClassName(request);
    PostfixHeader header;
    if (!ReadFunctionHeader(statement, className, header))
    {
        return false;
    }
    if (!className.empty())
    {
        for (const auto& sym : request.symbolTable.FindSymbols(className))
        {
            if (sym.type == analysis::SymbolType::Interface)
            {
                header.owner = PostfixOwner::InterfaceMethod;
            }
        }
    }

    std::vector<std::string_view> offered;
    if (!className.empty())
    {
        const bool isMethod = header.owner == PostfixOwner::ClassMethod;
        if (isMethod || header.owner == PostfixOwner::InterfaceMethod)
        {
            offered.push_back(parser::keywords::Const);
        }
        if (isMethod)
        {
            offered.push_back(parser::keywords::Final);
            if (BaseDeclaresMember(request, className, header.name))
            {
                offered.push_back(parser::keywords::Override);
            }
            if (header.name.rfind("get_", 0) == 0 || header.name.rfind("set_", 0) == 0)
            {
                offered.push_back(parser::keywords::Property);
            }
        }
        if (header.owner == PostfixOwner::Constructor)
        {
            offered.push_back(parser::keywords::Explicit);
        }
    }
    for (const std::string_view keyword : offered)
    {
        if (std::find(header.written.begin(), header.written.end(), keyword) == header.written.end())
        {
            AddItemIfNew(collector, {std::string(keyword), CompletionEntryKind::Keyword, "", "", "", "",
                                     FormatProximitySortText(SymbolProximity::Keyword, keyword)});
        }
    }
    return true;
}

} // namespace angel_lsp::analysis::completion
