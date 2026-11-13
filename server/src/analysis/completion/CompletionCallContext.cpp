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

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::completion
{

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
ResolveMemberCalleeCandidates(std::string_view callee, const CompletionQuery& request, const analysis::Scope* scope)
{
    std::vector<analysis::Symbol> candidates;
    size_t dotPos = callee.rfind('.');
    const std::string_view objText = callee.substr(0, dotPos);
    const std::string_view memText = callee.substr(dotPos + 1);
    std::string rType = ResolveNamedReceiverType(objText, request, scope);
    if (!rType.empty())
    {
        std::string cleanType = analysis::StripTypeDecorations(rType);
        auto hier = GetInheritedTypeHierarchy(request.symbolTable, cleanType);
        for (const auto& cls : hier)
        {
            auto syms = request.symbolTable.FindMemberSymbols(cls, memText);
            candidates.insert(candidates.end(), syms.begin(), syms.end());
        }
    }
    return candidates;
}

static std::vector<analysis::Symbol> ResolveUnqualifiedCalleeCandidates(std::string_view callee,
                                                                        const CompletionQuery& request,
                                                                        const analysis::Scope* scope)
{
    if (scope && request.nodeIndex && !callee.empty())
    {
        auto candidates = analysis::FindSymbolsInScope(
            request.symbolTable, {}, callee, request.nodeIndex->CollectUsingNamespaces(request.sourceCode, 0u));
        if (!candidates.empty())
        {
            return candidates;
        }
    }
    return request.symbolTable.FindSymbols(callee);
}

std::string FindCalleeParameterType(std::string_view callee, size_t argIndex, const CompletionQuery& request,
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

} // namespace angel_lsp::analysis::completion
