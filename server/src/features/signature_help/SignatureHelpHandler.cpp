#include "features/signature_help/SignatureHelpHandler.h"

#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Appends parameter declarations into comma-separated text in result.
 * @param[in,out] result Destination string.
 * @param[in] parameters List of parameter symbols.
 */
static void AppendParameters(std::string& result, const std::vector<analysis::ParameterInformation>& parameters)
{
    for (size_t i = 0; i < parameters.size(); ++i)
    {
        if (i > 0)
        {
            result += ", ";
        }
        const auto& param = parameters[i];
        if (!param.typeName.empty())
        {
            result += param.typeName;
        }
        if (!param.name.empty())
        {
            if (!result.empty() && result.back() != ' ')
            {
                result += " ";
            }
            result += param.name;
        }
        if (!param.defaultValue.empty())
        {
            result += " = ";
            result += param.defaultValue;
        }
    }
}

/**
 * @brief Formats human-readable signature label for a function or funcdef symbol.
 * @param[in] sym Symbol representing a function or funcdef.
 * @return Formatted signature string.
 */
static std::string FormatSignatureLabel(const analysis::Symbol& sym)
{
    if (sym.type != analysis::SymbolType::Function && sym.type != analysis::SymbolType::Funcdef)
    {
        return "";
    }

    std::string result;
    result.reserve(64);
    std::string_view returnType;
    const std::vector<analysis::ParameterInformation>* parameters = nullptr;

    if (sym.type == analysis::SymbolType::Funcdef)
    {
        result += "funcdef ";
        const auto& sig = sym.GetFuncdef();
        returnType = sig.returnType;
        parameters = &sig.parameters;
    }
    else
    {
        const auto& sig = sym.GetFunction();
        returnType = sig.returnType;
        parameters = &sig.parameters;
    }

    const bool isConstructor = (sym.type == analysis::SymbolType::Function && !sym.containerName.empty() &&
                                sym.name == analysis::LastScopeSegment(sym.containerName));

    if (!isConstructor)
    {
        if (returnType.empty())
        {
            result += "void ";
        }
        else
        {
            result += returnType;
            result += " ";
        }

        if (!sym.containerName.empty())
        {
            result += sym.containerName;
            result += "::";
        }
    }
    result += sym.name;
    result += "(";

    if (parameters)
    {
        AppendParameters(result, *parameters);
    }

    result += ")";
    return result;
}

/**
 * @brief Builds LSP parameter information objects for a symbol's parameter list.
 * @param[in] parameters List of symbol parameters.
 * @return Vector of LSP ParameterInformation.
 */
static std::vector<lsp::ParameterInformation>
BuildParameterList(const std::vector<analysis::ParameterInformation>& parameters)
{
    std::vector<lsp::ParameterInformation> params;
    params.reserve(parameters.size());

    for (const auto& param : parameters)
    {
        lsp::ParameterInformation pInfo;
        std::string pLabel;
        pLabel.reserve(param.typeName.size() + param.name.size() + param.defaultValue.size() + 4);
        if (!param.typeName.empty())
        {
            pLabel += param.typeName;
        }
        if (!param.name.empty())
        {
            if (!pLabel.empty())
            {
                pLabel += " ";
            }
            pLabel += param.name;
        }
        if (!param.defaultValue.empty())
        {
            pLabel += " = ";
            pLabel += param.defaultValue;
        }
        pInfo.label = std::move(pLabel);
        params.push_back(std::move(pInfo));
    }
    return params;
}

/**
 * @brief Constructs LSP signature information objects for candidate symbols.
 * @param[in] candidateSymbols Candidate function or funcdef symbols.
 * @return List of LSP signature entries.
 */
static std::vector<lsp::SignatureInformation> BuildSignatureList(const std::vector<analysis::Symbol>& candidateSymbols)
{
    std::vector<lsp::SignatureInformation> signatures;
    signatures.reserve(candidateSymbols.size());

    for (const auto& sym : candidateSymbols)
    {
        if (sym.type != analysis::SymbolType::Function && sym.type != analysis::SymbolType::Funcdef)
        {
            continue;
        }
        lsp::SignatureInformation sigInfo;
        sigInfo.label = FormatSignatureLabel(sym);
        const auto* parameters =
            (sym.type == analysis::SymbolType::Function) ? &sym.GetFunction().parameters : &sym.GetFuncdef().parameters;
        if (parameters && !parameters->empty())
        {
            sigInfo.parameters = BuildParameterList(*parameters);
        }
        signatures.push_back(std::move(sigInfo));
    }
    return signatures;
}

/**
 * @brief Selects active signature matching the active parameter index.
 * @param[in] signatures Available signature list.
 * @param[in] activeParameter Active parameter index.
 * @return Zero-based index of the matching signature.
 */
static uint32_t SelectActiveSignature(const std::vector<lsp::SignatureInformation>& signatures,
                                      uint32_t activeParameter)
{
    for (size_t i = 0; i < signatures.size(); ++i)
    {
        if (signatures[i].parameters.has_value() && activeParameter < signatures[i].parameters->size())
        {
            return static_cast<uint32_t>(i);
        }
    }
    return 0;
}

/**
 * @brief Finds a local definition in the given scope or enclosing scopes.
 * @param[in] scope Starting lexical scope.
 * @param[in] name Identifier name.
 * @return Pointer to local definition if found; nullptr otherwise.
 */
static const analysis::LocalDefinition* FindLocalDefinition(const analysis::Scope* scope, std::string_view name)
{
    while (scope)
    {
        for (const auto& def : scope->definitions)
        {
            if (def.name == name)
            {
                return &def;
            }
        }
        scope = scope->parent;
    }
    return nullptr;
}

/**
 * @brief Advances cursor past comments or string literals.
 * @param[in] source Source code.
 * @param[in] i Current byte offset.
 * @return New byte offset after skipped comment or string.
 */
static size_t SkipComment(std::string_view source, size_t i)
{
    if (i + 1 >= source.size() || source[i] != '/')
    {
        return i;
    }
    if (source[i + 1] == '/')
    {
        i += 2;
        while (i < source.size() && source[i] != '\n')
        {
            ++i;
        }
        return i;
    }
    if (source[i + 1] == '*')
    {
        i += 2;
        while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
        {
            ++i;
        }
        return i + (i + 1 < source.size() ? 2 : 0);
    }
    return i;
}

static size_t SkipStringLiteral(std::string_view source, size_t i)
{
    char c = source[i];
    if (c == '"' && i + 2 < source.size() && source[i + 1] == '"' && source[i + 2] == '"')
    {
        i += 3;
        while (i + 2 < source.size() && !(source[i] == '"' && source[i + 1] == '"' && source[i + 2] == '"'))
        {
            ++i;
        }
        return i + (i + 2 < source.size() ? 3 : 0);
    }
    if (c == '"' || c == '\'')
    {
        char quote = c;
        ++i;
        while (i < source.size() && source[i] != quote && source[i] != '\n')
        {
            i += (source[i] == '\\' && i + 1 < source.size()) ? 2 : 1;
        }
        return i + (i < source.size() && source[i] == quote ? 1 : 0);
    }
    return i;
}

static size_t SkipCommentOrString(std::string_view source, size_t i)
{
    size_t nextI = SkipComment(source, i);
    return (nextI > i) ? nextI : SkipStringLiteral(source, i);
}

using CallFrame = std::tuple<size_t, uint32_t, int, int>;

static void StepCallDelimiter(char c, std::vector<CallFrame>& stack, size_t offset)
{
    if (c == '(')
    {
        stack.push_back({offset, 0, 0, 0});
        return;
    }
    if (stack.empty())
    {
        return;
    }
    auto& top = stack.back();
    switch (c)
    {
    case ')':
        stack.pop_back();
        break;
    case '[':
        ++std::get<2>(top);
        break;
    case ']':
        if (std::get<2>(top) > 0)
            --std::get<2>(top);
        break;
    case '{':
        ++std::get<3>(top);
        break;
    case '}':
        if (std::get<3>(top) > 0)
            --std::get<3>(top);
        break;
    case ',':
        if (std::get<2>(top) == 0 && std::get<3>(top) == 0)
        {
            ++std::get<1>(top);
        }
        break;
    default:
        break;
    }
}

static bool IsValidPostTemplateChar(char c)
{
    return c == '(' || c == ')' || c == ',' || c == '@' || c == '[' || c == '.' || c == '>' || c == ':';
}

static bool IsValidTemplateChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == ',' || c == '@' || c == '&' ||
           std::isspace(static_cast<unsigned char>(c));
}

static bool HasPrecedingIdentifier(std::string_view source, size_t i)
{
    if (i == 0)
    {
        return false;
    }
    size_t k = i;
    while (k > 0 && (source[k - 1] == ' ' || source[k - 1] == '\t'))
    {
        --k;
    }
    return k > 0 && (std::isalnum(static_cast<unsigned char>(source[k - 1])) || source[k - 1] == '_');
}

static bool CheckFirstTypeChar(std::string_view source, size_t i)
{
    size_t k = i + 1;
    while (k < source.size() && (source[k] == ' ' || source[k] == '\t'))
    {
        ++k;
    }
    return k < source.size() &&
           (std::isalpha(static_cast<unsigned char>(source[k])) || source[k] == '_' || source[k] == '?');
}

static bool CheckPostTemplateValid(std::string_view source, size_t pos)
{
    while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t'))
    {
        ++pos;
    }
    return pos >= source.size() || IsValidPostTemplateChar(source[pos]);
}

static size_t TrySkipTemplate(std::string_view source, size_t i)
{
    if (i >= source.size() || source[i] != '<' || !HasPrecedingIdentifier(source, i) || !CheckFirstTypeChar(source, i))
    {
        return i;
    }

    int depth = 0;
    size_t j = i;
    while (j < source.size())
    {
        char c = source[j];
        if (c == '<')
        {
            ++depth;
            ++j;
        }
        else if (c == '>')
        {
            --depth;
            ++j;
            if (depth == 0)
            {
                return CheckPostTemplateValid(source, j) ? j : i;
            }
        }
        else if (IsValidTemplateChar(c))
        {
            ++j;
        }
        else
        {
            return i;
        }
    }
    return i;
}

/**
 * @brief Scans source text forwards to determine active call parenthesis and parameter offset.
 * @param[in] source Document text buffer.
 * @param[in] cursorByte Target cursor byte offset.
 * @return CallFrame if cursor is inside call parenthesis; nullopt otherwise.
 */
static std::optional<CallFrame> ScanEnclosingCall(std::string_view source, size_t cursorByte)
{
    std::vector<CallFrame> stack;
    size_t i = 0;

    while (i < cursorByte && i < source.size())
    {
        size_t nextI = SkipCommentOrString(source, i);
        if (nextI > i)
        {
            i = nextI;
            continue;
        }
        if (source[i] == '<')
        {
            size_t tmplI = TrySkipTemplate(source, i);
            if (tmplI > i)
            {
                if (tmplI <= cursorByte)
                {
                    i = tmplI;
                    continue;
                }
                break;
            }
        }
        StepCallDelimiter(source[i], stack, i);
        ++i;
    }
    if (stack.empty())
    {
        return std::nullopt;
    }
    return stack.back();
}

static size_t SkipWhitespaceBackwards(std::string_view s, size_t pos)
{
    while (pos > 0 && std::isspace(static_cast<unsigned char>(s[pos - 1])))
    {
        --pos;
    }
    return pos;
}

static size_t SkipTemplateArgsBackwards(std::string_view s, size_t pos)
{
    if (pos == 0 || s[pos - 1] != '>')
    {
        return pos;
    }
    int depth = 1;
    --pos;
    while (pos > 0 && depth > 0)
    {
        if (s[pos - 1] == '>')
        {
            depth++;
        }
        else if (s[pos - 1] == '<')
        {
            depth--;
        }
        --pos;
    }
    return SkipWhitespaceBackwards(s, pos);
}

static std::string_view ExtractIdentifierBackwards(std::string_view s, size_t& pos)
{
    pos = SkipWhitespaceBackwards(s, pos);
    size_t end = pos;
    while (pos > 0 && (std::isalnum(static_cast<unsigned char>(s[pos - 1])) || s[pos - 1] == '_'))
    {
        --pos;
    }
    return (end > pos) ? s.substr(pos, end - pos) : std::string_view{};
}

static std::string CleanReceiverTypeName(std::string_view rawType)
{
    analysis::TrimTypeWhitespace(rawType);
    rawType = analysis::StripLeadingConst(rawType);
    analysis::TrimTypeWhitespace(rawType);
    rawType = analysis::StripTrailingDecorationsView(rawType, false);
    analysis::TrimTypeWhitespace(rawType);

    if (rawType.ends_with("[]") || rawType.starts_with("array<"))
    {
        return "array";
    }
    if (auto pos = rawType.find('<'); pos != std::string::npos)
    {
        return std::string(rawType.substr(0, pos));
    }
    return std::string(rawType);
}

static std::string ResolveReceiverType(std::string_view receiverName, const analysis::Scope* scope,
                                       const analysis::SymbolTable& symbolTable)
{
    if (receiverName == "this")
    {
        return scope ? std::string(scope->GetEnclosingClass()) : std::string{};
    }
    if (const auto* def = FindLocalDefinition(scope, receiverName))
    {
        return def->typeName;
    }
    if (scope && !scope->GetEnclosingClass().empty())
    {
        auto mems = symbolTable.FindMemberSymbols(scope->GetEnclosingClass(), receiverName);
        if (!mems.empty() && mems[0].type == analysis::SymbolType::Variable)
        {
            return mems[0].GetVariable().typeName;
        }
    }
    auto globals = symbolTable.FindSymbols(receiverName);
    for (const auto& sym : globals)
    {
        if (sym.type == analysis::SymbolType::Variable)
        {
            return sym.GetVariable().typeName;
        }
    }
    return "";
}

/**
 * @brief Resolves candidate method symbols for a member expression callee.
 * @param[in] receiver Receiver expression name.
 * @param[in] callee Callee method identifier.
 * @param[in] scope Current lexical scope.
 * @param[in] symbolTable Symbol table.
 * @return Matching symbol candidates from the receiver hierarchy.
 */
static std::vector<analysis::Symbol> ResolveMemberCandidates(std::string_view receiver, std::string_view callee,
                                                             const analysis::Scope* scope,
                                                             const analysis::SymbolTable& symbolTable)
{
    std::string rawType = ResolveReceiverType(receiver, scope, symbolTable);
    if (rawType.empty())
    {
        return {};
    }
    std::string cleanType = CleanReceiverTypeName(rawType);
    auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanType, symbolTable);
    std::vector<analysis::Symbol> candidates;

    for (const auto& typeName : hierarchy)
    {
        auto mems = symbolTable.FindMemberSymbols(typeName, callee);
        for (const auto& sym : mems)
        {
            if (sym.type == analysis::SymbolType::Function || sym.type == analysis::SymbolType::Funcdef)
            {
                candidates.push_back(sym);
            }
        }
        if (!candidates.empty())
        {
            return candidates;
        }
    }
    return {};
}

/**
 * @brief Resolves signature information for direct (non-member) callee expressions.
 * @param[in] callee Callee identifier.
 * @param[in] prevToken Token preceding callee if declaring variable.
 * @param[in] scope Current lexical scope.
 * @param[in] symbolTable Symbol table.
 * @return Resolved list of candidate symbols.
 */
static std::vector<analysis::Symbol> ResolveDirectCandidates(std::string_view callee, std::string_view prevToken,
                                                             const analysis::Scope* scope,
                                                             const analysis::SymbolTable& symbolTable)
{
    if (const auto* def = FindLocalDefinition(scope, callee))
    {
        std::string clean = analysis::CleanBaseType(def->typeName);
        if (auto fd = analysis::FindFuncdefSymbol(clean, symbolTable))
        {
            return {analysis::FuncdefToFunctionSymbol(*fd, std::string(callee))};
        }
    }

    const auto* varDef = FindLocalDefinition(scope, callee);
    std::string_view initType = (varDef && !varDef->typeName.empty()) ? std::string_view(varDef->typeName) : prevToken;
    if (!initType.empty())
    {
        std::string cleanInit = analysis::CleanBaseType(initType);
        auto ctors = symbolTable.FindMemberSymbols(cleanInit, cleanInit);
        if (!ctors.empty())
        {
            return ctors;
        }
    }

    if (auto found = symbolTable.FindSymbolsPtr(callee); found && !found->empty())
    {
        std::vector<analysis::Symbol> funcs;
        for (const auto& s : *found)
        {
            if (s.type == analysis::SymbolType::Function || s.type == analysis::SymbolType::Funcdef)
            {
                funcs.push_back(s);
            }
        }
        if (!funcs.empty())
        {
            return funcs;
        }
    }

    auto ctors = symbolTable.FindMemberSymbols(callee, callee);
    if (!ctors.empty())
    {
        return ctors;
    }
    auto typeSyms = symbolTable.FindTypeSymbolsByShortName(callee);
    if (!typeSyms.empty())
    {
        auto typeCtors = symbolTable.FindMemberSymbols(typeSyms[0].name, typeSyms[0].name);
        if (!typeCtors.empty())
        {
            return typeCtors;
        }
    }

    if (scope && !scope->GetEnclosingClass().empty())
    {
        auto mems = symbolTable.FindMemberSymbols(scope->GetEnclosingClass(), callee);
        std::vector<analysis::Symbol> funcs;
        for (const auto& s : mems)
        {
            if (s.type == analysis::SymbolType::Function || s.type == analysis::SymbolType::Funcdef)
            {
                funcs.push_back(s);
            }
        }
        if (!funcs.empty())
        {
            return funcs;
        }
    }

    if (auto callableSym = analysis::TryResolveCallableFuncdef(std::string(callee), scope, symbolTable))
    {
        return {*callableSym};
    }
    return {};
}

std::optional<lsp::SignatureHelp> GetSignatureHelp(const SignatureHelpRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    const size_t cursorByte = utils::PositionToOffset(request.sourceCode, request.position.line,
                                                      request.position.character, utils::PositionEncoding::Utf16);
    auto frame = ScanEnclosingCall(request.sourceCode, cursorByte);
    if (!frame)
    {
        return std::nullopt;
    }

    size_t parenPos = std::get<0>(*frame);
    uint32_t activeParam = std::get<1>(*frame);

    size_t pos = SkipWhitespaceBackwards(request.sourceCode, parenPos);
    pos = SkipTemplateArgsBackwards(request.sourceCode, pos);
    std::string_view callee = ExtractIdentifierBackwards(request.sourceCode, pos);
    if (callee.empty())
    {
        return std::nullopt;
    }

    pos = SkipWhitespaceBackwards(request.sourceCode, pos);
    std::string_view receiver;
    std::string_view prevToken;
    bool isMember = false;
    bool isQualified = false;

    if (pos > 0 && request.sourceCode[pos - 1] == '.')
    {
        isMember = true;
        --pos;
        receiver = ExtractIdentifierBackwards(request.sourceCode, pos);
    }
    else if (pos >= 2 && request.sourceCode[pos - 1] == ':' && request.sourceCode[pos - 2] == ':')
    {
        isQualified = true;
        pos -= 2;
        receiver = ExtractIdentifierBackwards(request.sourceCode, pos);
    }
    else
    {
        prevToken = ExtractIdentifierBackwards(request.sourceCode, pos);
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope =
        rootScope ? FindInnermostScope(rootScope.get(), request.position.line, request.position.character) : nullptr;

    std::vector<analysis::Symbol> candidates;
    if (isMember)
    {
        candidates = ResolveMemberCandidates(receiver, callee, scope, request.symbolTable);
    }
    else if (isQualified)
    {
        std::string full = utils::JoinScopedName(receiver, callee);
        if (auto found = request.symbolTable.FindSymbolsPtr(full))
        {
            candidates = *found;
        }
    }
    else
    {
        candidates = ResolveDirectCandidates(callee, prevToken, scope, request.symbolTable);
    }

    if (candidates.empty())
    {
        return std::nullopt;
    }

    auto signatures = BuildSignatureList(candidates);
    if (signatures.empty())
    {
        return std::nullopt;
    }

    uint32_t activeSig = SelectActiveSignature(signatures, activeParam);

    lsp::SignatureHelp result;
    result.signatures = std::move(signatures);
    result.activeSignature = activeSig;
    result.activeParameter = activeParam;
    return result;
}

} // namespace angel_lsp::features
