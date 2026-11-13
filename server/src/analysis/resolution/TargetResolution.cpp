#include "analysis/resolution/TargetResolution.h"

#include "analysis/index/CallArgumentFacts.h"
#include "analysis/resolution/ReferenceReceiver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/LspLogger.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <map>
#include <set>
#include <spdlog/fmt/fmt.h>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsIdentChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static std::string_view ExtractLineFromSource(std::string_view source, uint32_t targetLine,
                                              uint32_t& lineStartByte) noexcept
{
    uint32_t curLine = 0;
    size_t start = 0;
    while (curLine < targetLine && start < source.size())
    {
        size_t next = source.find('\n', start);
        if (next == std::string_view::npos)
        {
            return {};
        }
        start = next + 1;
        ++curLine;
    }
    if (curLine != targetLine || start > source.size())
    {
        return {};
    }
    lineStartByte = static_cast<uint32_t>(start);
    size_t end = source.find('\n', start);
    if (end == std::string_view::npos)
    {
        end = source.size();
    }
    if (end > start && source[end - 1] == '\r')
    {
        --end;
    }
    return source.substr(start, end - start);
}

/**
 * @brief Extracts token text under cursor with trailing-edge tolerance.
 * @param[in] sourceCode Document source text.
 * @param[in] position Cursor position.
 * @param[out] outStartByte Optional output start byte.
 * @param[out] outEndByte Optional output end byte.
 * @return Token text or empty string if not an identifier.
 */
static std::string_view GetNodeTextAt(std::string_view sourceCode, TargetPosition position, uint32_t* outStartByte = nullptr,
                                      uint32_t* outEndByte = nullptr)
{
    if (sourceCode.empty())
    {
        return {};
    }

    uint32_t lineStartByte = 0;
    std::string_view line = ExtractLineFromSource(sourceCode, position.line, lineStartByte);
    if (line.empty())
    {
        return {};
    }

    size_t col = position.character;
    if (col > line.size())
    {
        col = line.size();
    }

    if (col >= line.size() || !IsIdentChar(line[col]))
    {
        if (col > 0 && IsIdentChar(line[col - 1]))
        {
            --col;
        }
        else
        {
            return {};
        }
    }

    size_t startCol = col;
    while (startCol > 0 && IsIdentChar(line[startCol - 1]))
    {
        --startCol;
    }
    size_t endCol = col;
    while (endCol < line.size() && IsIdentChar(line[endCol]))
    {
        ++endCol;
    }

    if (startCol >= endCol)
    {
        return {};
    }

    uint32_t startByte = lineStartByte + static_cast<uint32_t>(startCol);
    uint32_t endByte = lineStartByte + static_cast<uint32_t>(endCol);
    if (outStartByte)
    {
        *outStartByte = startByte;
    }
    if (outEndByte)
    {
        *outEndByte = endByte;
    }
    return line.substr(startCol, endCol - startCol);
}

/**
 * @brief Resolves the enclosing class name for a given source position.
 * @param[in] symbolTable Symbol table to look up class definitions.
 * @param[in] uri Document file URI.
 * @param[in] line 0-based line number.
 * @return Enclosing class name or empty string if not in a class.
 */
static std::string GetEnclosingClassName(const analysis::SymbolTable& symbolTable, std::string_view uri,
                                         uint32_t line)
{
    return symbolTable.FindEnclosingClassName(uri, line);
}

/**
 * @brief Collects all unique indexed file URIs from the symbol table plus the current document.
 * @param[in] symbolTable Global symbol table.
 * @param[in] currentUri Current document URI.
 * @return Unique list of file URIs.
 */
static std::vector<std::string> GetAllIndexedFileUris(const analysis::SymbolTable& symbolTable,
                                                      std::string_view currentUri)
{
    std::vector<std::string> uris = symbolTable.GetIndexedFileUris();
    if (!currentUri.empty() && std::find(uris.begin(), uris.end(), currentUri) == uris.end())
    {
        uris.emplace_back(currentUri);
    }
    return uris;
}

bool IsValidIdentifier(std::string_view name)
{
    if (name.empty())
    {
        return false;
    }

    if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
    {
        return false;
    }

    for (size_t i = 1; i < name.size(); ++i)
    {
        if (!std::isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_')
        {
            return false;
        }
    }

    if (analysis::IsReservedKeyword(name) || analysis::IsPrimitiveTypeName(name))
    {
        return false;
    }

    return true;
}

/**
 * @brief Searches for a local definition matching name and line spanning scope ancestors.
 * @param[in] innerScope Innermost scope.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] line Target line number.
 * @param[out] outDeclScope Scope where definition was found.
 * @return Pointer to definition if found, nullptr otherwise.
 */
static const analysis::LocalDefinition* FindLocalDefinitionAtLine(const analysis::Scope* innerScope,
                                                                  std::string_view nodeText, uint32_t line,
                                                                  const analysis::Scope*& outDeclScope)
{
    analysis::ScopeTraversalGuard guard;
    for (const analysis::Scope* cur = innerScope; guard.CheckAndInsert(cur); cur = cur->parent)
    {
        for (const auto& d : cur->definitions)
        {
            if (d.name == nodeText && line >= d.startLine && line <= d.endLine)
            {
                outDeclScope = cur;
                return &d;
            }
        }
    }
    return nullptr;
}

/**
 * @brief Resolves target as a local variable, parameter, or enclosing class member.
 * @param[in] rootScope Document root scope.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Resolution request.
 * @param[out] target Target descriptor to populate.
 */
static void PopulateLocalTargetDescriptor(const analysis::LocalDefinition& matchedDef, const analysis::Scope* declScope,
                                          const ResolveTargetRequest& request, TargetDescriptor& target)
{
    if (matchedDef.kind == analysis::LocalDefinitionKind::Parameter ||
        matchedDef.kind == analysis::LocalDefinitionKind::Variable)
    {
        if (IsDeclaredInFunctionScope(declScope))
        {
            target.kind = TargetKind::Local;
            target.localDef = matchedDef;
            target.localUri = request.uri;
        }
    }
    else if (matchedDef.kind == analysis::LocalDefinitionKind::Field ||
             matchedDef.kind == analysis::LocalDefinitionKind::Method)
    {
        std::string enclosingClass = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line);
        if (!enclosingClass.empty())
        {
            target.kind = TargetKind::ClassMember;
            target.declaringClass = enclosingClass;
        }
    }
}

static void ResolveLocalTarget(const std::shared_ptr<const analysis::Scope>& rootScope, std::string_view nodeText,
                               const ResolveTargetRequest& request, TargetDescriptor& target,
                               uint32_t identStartByte = UINT32_MAX)
{
    if (!rootScope)
    {
        return;
    }
    const analysis::Scope* innerScope =
        FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
    if (!innerScope)
    {
        return;
    }
    const analysis::Scope* declScope = nullptr;
    const analysis::LocalDefinition* matchedDef =
        FindLocalDefinitionAtLine(innerScope, nodeText, request.position.line, declScope);
    if (!matchedDef)
    {
        const uint32_t cursorByte = identStartByte;
        matchedDef = analysis::ResolveInScopeAt(innerScope, nodeText, cursorByte, &declScope, false);
    }
    if (!matchedDef || !declScope)
    {
        return;
    }
    PopulateLocalTargetDescriptor(*matchedDef, declScope, request, target);
    if (target.kind == TargetKind::Local)
    {
        target.definingScope = std::shared_ptr<const Scope>(rootScope, declScope);
    }
}

/**
 * @brief Checks if any symbol in candidates represents a class or interface member.
 * @param[in] syms Candidate symbols to inspect.
 * @return True if at least one candidate is not from a namespace or enum.
 */
static bool HasClassMemberSymbol(const std::vector<Symbol>& syms)
{
    for (const auto& s : syms)
    {
        if (s.isSynthesized)
        {
            continue;
        }
        if (s.containerKind != analysis::ContainerKind::Namespace && s.containerKind != analysis::ContainerKind::Enum)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Assign target from container kind.
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in,out] target Target node or symbol descriptor.
 * @return True if the condition is satisfied; false otherwise.
 */
static bool AssignTargetFromContainerKind(const analysis::Symbol& sym, TargetDescriptor& target)
{
    if (sym.containerKind == analysis::ContainerKind::Class || sym.containerKind == analysis::ContainerKind::Interface)
    {
        target.kind = TargetKind::ClassMember;
        target.declaringClass = sym.containerName;
        return true;
    }
    if (sym.containerKind == analysis::ContainerKind::Namespace)
    {
        target.kind = TargetKind::NamespaceSymbol;
        target.declaringNamespace = sym.containerName;
        target.qualifiedName = sym.qualifiedName;
        return true;
    }
    if (sym.containerKind == analysis::ContainerKind::Enum)
    {
        target.kind = TargetKind::EnumMember;
        target.declaringEnum = sym.containerName;
        target.qualifiedName = sym.qualifiedName;
        return true;
    }
    return false;
}

/**
 * @brief Assign target from container lookup.
 * @param[in] sym Tree-sitter grammar symbol.
 * @param[in] table Symbol table containing resolved symbols.
 * @param[in,out] target Target node or symbol descriptor.
 * @return True if the condition is satisfied; false otherwise.
 */
static bool AssignTargetFromContainerLookup(const analysis::Symbol& sym, const SymbolTable& table,
                                            TargetDescriptor& target)
{
    auto containerSyms = table.FindSymbols(sym.containerName);
    for (const auto& csym : containerSyms)
    {
        if (csym.type == analysis::SymbolType::Class || csym.type == analysis::SymbolType::Interface)
        {
            target.kind = TargetKind::ClassMember;
            target.declaringClass = sym.containerName;
            return true;
        }
        if (csym.type == analysis::SymbolType::Namespace)
        {
            target.kind = TargetKind::NamespaceSymbol;
            target.declaringNamespace = sym.containerName;
            target.qualifiedName = sym.qualifiedName;
            return true;
        }
        if (csym.type == analysis::SymbolType::Enum)
        {
            target.kind = TargetKind::EnumMember;
            target.declaringEnum = sym.containerName;
            target.qualifiedName = sym.qualifiedName;
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks global symbols for class or namespace containers matching the symbol.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Resolution request.
 * @param[out] target Target descriptor to populate.
 */
static void ResolveGlobalFallbackTarget(std::string_view nodeText, const ResolveTargetRequest& request,
                                        TargetDescriptor& target)
{
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.isSynthesized)
                {
                    continue;
                }
                if (sym.fileUri == request.uri && request.position.line >= sym.startLine &&
                    request.position.line <= sym.endLine && sym.name == nodeText)
                {
                    if (sym.containerName.empty())
                    {
                        continue;
                    }
                    if (AssignTargetFromContainerKind(sym, target) ||
                        AssignTargetFromContainerLookup(sym, request.symbolTable, target))
                    {
                        return;
                    }
                }
            }
        });
}

/**
 * @brief Matches an appropriate function signature from candidate symbols based on call constraints.
 * @param[in] symbols Candidate symbol bucket.
 * @param[in] uri Target document URI.
 * @param[in] line Target line number.
 * @param[in] callInfo Call arguments info.
 * @return Optional matching FunctionSignature.
 */
static std::optional<analysis::FunctionSignature> MatchFunctionSignature(const std::vector<analysis::Symbol>& symbols,
                                                                         std::string_view uri, uint32_t line,
                                                                         const FunctionArgConstraints& callInfo)
{
    for (const auto& s : symbols)
    {
        if (s.fileUri == uri && line >= s.startLine && line <= s.endLine &&
            std::holds_alternative<analysis::FunctionSignature>(s.signature))
        {
            return s.GetFunction();
        }
    }
    std::optional<analysis::FunctionSignature> fallback;
    for (const auto& s : symbols)
    {
        if (!std::holds_alternative<analysis::FunctionSignature>(s.signature))
        {
            continue;
        }
        const auto& fn = s.GetFunction();
        size_t minP = 0;
        for (const auto& p : fn.parameters)
        {
            if (p.defaultValue.empty())
            {
                minP++;
            }
        }
        size_t maxP = fn.parameters.size();
        if (callInfo.cursorOnCall)
        {
            if (callInfo.callArgCount >= minP && callInfo.callArgCount <= maxP)
            {
                return fn;
            }
        }
        else if (!fallback)
        {
            fallback = fn;
        }
    }
    return fallback;
}

/**
 * @brief Configures target descriptor function properties from signature.
 * @param[out] target Target descriptor to update.
 * @param[in] fn Matched function signature.
 */
static void ApplyFunctionSignature(TargetDescriptor& target, const analysis::FunctionSignature& fn)
{
    target.isFunction = true;
    target.maxArgs = fn.parameters.size();
    target.minArgs = 0;
    for (const auto& p : fn.parameters)
    {
        if (p.defaultValue.empty())
        {
            target.minArgs++;
        }
    }
}

/**
 * @brief Looks up a member directly declared in the class or file.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[out] resolvedAccess Access modifier if found.
 * @return True if direct member symbol was found.
 */
static bool ResolveDirectClassMember(TargetDescriptor& target, const ResolveTargetRequest& request,
                                     analysis::AccessModifier& resolvedAccess)
{
    std::string qMember = utils::JoinScopedName(target.declaringClass, target.name);
    auto localSyms = request.symbolTable.FindSymbols(qMember);
    if (localSyms.empty() && !target.declaringClass.empty())
    {
        localSyms = request.symbolTable.FindSymbols(target.name);
    }
    for (const auto& s : localSyms)
    {
        if (s.fileUri == request.uri && request.position.line >= s.startLine && request.position.line <= s.endLine)
        {
            if (std::holds_alternative<analysis::FunctionSignature>(s.signature))
            {
                resolvedAccess = s.GetFunction().modifiers.access;
                ApplyFunctionSignature(target, s.GetFunction());
                return true;
            }
            if (std::holds_alternative<analysis::VariableSignature>(s.signature))
            {
                resolvedAccess = s.GetVariable().modifiers.access;
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Looks up an inherited member in ancestor classes.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[in] callInfo Call argument constraints.
 * @param[out] resolvedAccess Access modifier if found.
 */
static void ResolveInheritedClassMember(TargetDescriptor& target, const ResolveTargetRequest& request,
                                        const FunctionArgConstraints& callInfo,
                                        analysis::AccessModifier& resolvedAccess)
{
    auto hier = analysis::GetInheritedTypeHierarchy(target.declaringClass, request.symbolTable);
    for (const auto& owner : hier)
    {
        auto symsPtr = request.symbolTable.FindMemberSymbolPtr(owner, target.name);
        if (!symsPtr)
        {
            continue;
        }
        if (auto fn = MatchFunctionSignature(*symsPtr, request.uri, request.position.line, callInfo))
        {
            resolvedAccess = fn->modifiers.access;
            ApplyFunctionSignature(target, *fn);
            return;
        }
        for (const auto& s : *symsPtr)
        {
            if (std::holds_alternative<analysis::VariableSignature>(s.signature))
            {
                resolvedAccess = s.GetVariable().modifiers.access;
                return;
            }
        }
    }
}

/**
 * @brief Resolves access modifier and overload signature for a class member target.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[in] callInfo Call argument constraints.
 */
static void ResolveClassMemberDetails(TargetDescriptor& target, const ResolveTargetRequest& request,
                                      const FunctionArgConstraints& callInfo)
{
    analysis::AccessModifier resolvedAccess = analysis::AccessModifier::Public;
    if (!ResolveDirectClassMember(target, request, resolvedAccess))
    {
        ResolveInheritedClassMember(target, request, callInfo, resolvedAccess);
    }
    target.access = resolvedAccess;
    target.relatedClasses =
        analysis::GetCompatibleMemberClasses(target.declaringClass, target.name, target.access, request.symbolTable);
}

/**
 * @brief Resolves overload function signature for class member, namespace, or global symbol.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[in] callInfo Call argument constraints.
 */
static void ResolveTargetSignature(TargetDescriptor& target, const ResolveTargetRequest& request,
                                   const FunctionArgConstraints& callInfo)
{
    if (target.kind == TargetKind::ClassMember)
    {
        ResolveClassMemberDetails(target, request, callInfo);
        return;
    }
    if (target.kind != TargetKind::NamespaceSymbol && target.kind != TargetKind::GlobalSymbol)
    {
        return;
    }
    const std::string_view query = (target.kind == TargetKind::NamespaceSymbol) ? target.qualifiedName : target.name;
    if (auto symsPtr = request.symbolTable.FindSymbolsPtr(query))
    {
        if (auto fn = MatchFunctionSignature(*symsPtr, request.uri, request.position.line, callInfo))
        {
            ApplyFunctionSignature(target, *fn);
        }
    }
}

static bool ResolveGlobalScopedTarget(const ResolveTargetRequest& request, const GlobalScopeQualifier& gsq,
                                      TargetDescriptor& target)
{
    if (gsq.isDirectRoot)
    {
        if (auto symsPtr = request.symbolTable.FindSymbolsPtr(gsq.rootPath))
        {
            for (const auto& sym : *symsPtr)
            {
                if (sym.containerName.empty() && sym.type != analysis::SymbolType::CallReference)
                {
                    target.kind = TargetKind::GlobalSymbol;
                    target.name = std::string(gsq.rootPath);
                    FunctionArgConstraints callInfo{};
                    ResolveTargetSignature(target, request, callInfo);
                    return true;
                }
            }
        }
        return false;
    }

    if (auto symsPtr = request.symbolTable.FindSymbolsPtr(gsq.rootPath))
    {
        const bool hasRealSymbol = std::any_of(symsPtr->begin(), symsPtr->end(), [](const Symbol& sym)
                                               { return sym.type != analysis::SymbolType::CallReference; });
        if (hasRealSymbol)
        {
            const auto [nsPrefix, leafName] = utils::SplitScopedName(gsq.rootPath);
            if (!nsPrefix.empty())
            {
                target.kind = TargetKind::NamespaceSymbol;
                target.qualifiedName = std::string(gsq.rootPath);
                target.declaringNamespace = std::string(nsPrefix);
                target.name = std::string(leafName);
            }
            else
            {
                target.kind = TargetKind::GlobalSymbol;
                target.name = std::string(gsq.rootPath);
                target.qualifiedName = std::string(gsq.rootPath);
            }
            FunctionArgConstraints callInfo{};
            ResolveTargetSignature(target, request, callInfo);
            return true;
        }
    }
    return false;
}

std::shared_ptr<const Symbol> ResolveIndexedCall(const SymbolTable& symbols, const Scope* root, uint32_t nameByte,
                                                 const std::optional<CallBindingRevision>& revision)
{
    const auto* bindings = root ? root->GetCallBindings() : nullptr;
    if (!bindings || !revision || !bindings->IsFrom(revision))
    {
        return {};
    }
    const auto* binding = bindings->Find(nameByte);
    if (!binding)
    {
        return {};
    }
    const auto candidates = symbols.FindSymbolsPtr(binding->qualifiedName);
    if (!candidates)
    {
        return {};
    }
    for (const auto& symbol : *candidates)
    {
        if (symbol.type == SymbolType::Function && !symbol.isSynthesized && binding->Matches(symbol))
        {
            return {candidates, &symbol};
        }
    }
    return {};
}

static bool ResolveTargetFromCall(const ResolveTargetRequest& request, const Scope* root, TargetDescriptor& target,
                                  uint32_t identStartByte = UINT32_MAX)
{
    if (!request.useCallBindings)
    {
        return false;
    }
    const uint32_t callByte = identStartByte;
    if (callByte == UINT32_MAX)
    {
        return false;
    }
    const auto symbol = ResolveIndexedCall(request.symbolTable, root, callByte, request.callBindingRevision);
    if (!symbol || symbol->name != target.name)
    {
        return false;
    }
    AssignTargetFromContainerKind(*symbol, target);
    ApplyFunctionSignature(target, symbol->GetFunction());
    target.callDeclaration.emplace(*symbol);
    if (target.kind == TargetKind::ClassMember)
    {
        target.access = symbol->GetFunction().modifiers.access;
        target.relatedClasses =
            GetCompatibleMemberClasses(target.declaringClass, target.name, target.access, request.symbolTable);
    }
    return true;
}

static void CaptureCallDeclaration(TargetDescriptor& target, const ResolveTargetRequest& request,
                                   uint32_t identStartByte = UINT32_MAX, uint32_t identEndByte = UINT32_MAX)
{
    if (!request.useCallBindings || !target.isFunction)
    {
        return;
    }
    if (identStartByte == UINT32_MAX || identEndByte == UINT32_MAX)
    {
        return;
    }
    uint32_t lineStartByte = 0;
    ExtractLineFromSource(request.sourceCode, request.position.line, lineStartByte);
    uint32_t sCol = identStartByte >= lineStartByte ? (identStartByte - lineStartByte) : request.position.character;
    uint32_t eCol = identEndByte >= lineStartByte ? (identEndByte - lineStartByte)
                                                  : sCol + static_cast<uint32_t>(target.name.size());
    SourceRange selection{request.position.line, sCol, request.position.line, eCol};
    const auto query = target.kind == TargetKind::ClassMember
                           ? utils::JoinScopedName(target.declaringClass, target.name)
                           : (target.qualifiedName.empty() ? target.name : target.qualifiedName);
    if (const auto candidates = request.symbolTable.FindSymbolsPtr(query))
    {
        for (const auto& symbol : *candidates)
        {
            if (symbol.type == SymbolType::Function && symbol.fileUri == request.uri &&
                symbol.selectionRange == selection && !symbol.isSynthesized)
            {
                target.callDeclaration.emplace(symbol);
                ApplyFunctionSignature(target, symbol.GetFunction());
                return;
            }
        }
    }
}

/**
 * @brief Computes global scope qualification without an AST.
 * @param[in] sourceCode Document source text.
 * @param[in] identStartByte Start byte of identifier.
 * @param[in] identEndByte End byte of identifier.
 * @return GlobalScopeQualifier structure.
 */
static GlobalScopeQualifier GetGlobalScopeQualifierNoAst(std::string_view sourceCode, uint32_t identStartByte,
                                                         uint32_t identEndByte) noexcept
{
    GlobalScopeQualifier gsq{};
    size_t chainStart = identStartByte;
    while (chainStart >= 2 && sourceCode[chainStart - 1] == ':' && sourceCode[chainStart - 2] == ':')
    {
        size_t segEnd = chainStart - 2;
        size_t segStart = segEnd;
        while (segStart > 0 && IsIdentChar(sourceCode[segStart - 1]))
        {
            --segStart;
        }
        if (segStart == segEnd)
        {
            gsq.isGlobal = true;
            break;
        }
        chainStart = segStart;
    }
    if (!gsq.isGlobal)
    {
        return gsq;
    }
    size_t chainEnd = identEndByte;
    while (chainEnd + 2 <= sourceCode.size() && sourceCode[chainEnd] == ':' && sourceCode[chainEnd + 1] == ':')
    {
        size_t nextStart = chainEnd + 2;
        size_t nextEnd = nextStart;
        while (nextEnd < sourceCode.size() && IsIdentChar(sourceCode[nextEnd]))
        {
            ++nextEnd;
        }
        if (nextEnd == nextStart)
        {
            break;
        }
        chainEnd = nextEnd;
    }
    if (chainEnd > chainStart && chainEnd <= sourceCode.size())
    {
        gsq.rootPath = sourceCode.substr(chainStart, chainEnd - chainStart);
        gsq.isDirectRoot = !utils::ContainsScopeSeparator(gsq.rootPath);
    }
    return gsq;
}

/**
 * @brief Finds the declaring enum container for an enum member symbol.
 * @param[in] s Enum member symbol.
 * @param[in] qualifierName Extracted qualifier text.
 * @param[in] symbolTable Symbol table.
 * @return Declaring enum container name.
 */
static std::string FindRealDeclaringEnum(const analysis::Symbol& s, std::string_view qualifierName,
                                         const analysis::SymbolTable& symbolTable)
{
    std::string declEnum = s.containerName.empty() ? std::string(qualifierName) : s.containerName;
    auto enumSyms = symbolTable.FindSymbols(declEnum);
    bool isRealEnum = std::any_of(enumSyms.begin(), enumSyms.end(),
                                  [](const auto& es) { return es.type == analysis::SymbolType::Enum; });
    if (isRealEnum)
    {
        return declEnum;
    }
    auto allMemberSyms = symbolTable.FindSymbols(s.name);
    for (const auto& ms : allMemberSyms)
    {
        if (ms.type == analysis::SymbolType::Variable && ms.containerKind == analysis::ContainerKind::Enum &&
            ms.containerName != declEnum && (declEnum.empty() || ms.containerName.starts_with(declEnum)))
        {
            auto candEnums = symbolTable.FindSymbols(ms.containerName);
            if (std::any_of(candEnums.begin(), candEnums.end(),
                            [](const auto& es) { return es.type == analysis::SymbolType::Enum; }))
            {
                return ms.containerName;
            }
        }
    }
    return declEnum;
}

/**
 * @brief Extracts the preceding qualifier chain before a scoped member.
 * @param[in] sourceCode Document source text.
 * @param[in] identStartByte Start byte of identifier.
 * @return Extracted qualifier string or empty string.
 */
static std::string_view ExtractPrecedingQualifier(std::string_view sourceCode, uint32_t identStartByte)
{
    if (identStartByte < 2 || sourceCode[identStartByte - 1] != ':' || sourceCode[identStartByte - 2] != ':')
    {
        return {};
    }

    size_t pEnd = identStartByte - 2;
    size_t pStart = pEnd;
    while (pStart > 0 && IsIdentChar(sourceCode[pStart - 1]))
    {
        --pStart;
    }
    while (pStart >= 2 && sourceCode[pStart - 1] == ':' && sourceCode[pStart - 2] == ':')
    {
        size_t prevEnd = pStart - 2;
        size_t prevStart = prevEnd;
        while (prevStart > 0 && IsIdentChar(sourceCode[prevStart - 1]))
        {
            --prevStart;
        }
        if (prevStart == prevEnd)
        {
            break;
        }
        pStart = prevStart;
    }
    if (pEnd <= pStart)
    {
        return {};
    }
    return sourceCode.substr(pStart, pEnd - pStart);
}

/**
 * @brief Finds scoped symbols searching directly and across enclosing namespaces.
 * @param[in] request Resolution request context.
 * @param[in] scopedText Fully qualified scoped text to look up.
 * @return Matched symbols vector.
 */
static std::vector<analysis::Symbol> FindScopedSymbolsWithEnclosing(const ResolveTargetRequest& request,
                                                                    std::string_view scopedText)
{
    auto syms = request.symbolTable.FindSymbols(scopedText);
    if (!syms.empty())
    {
        return syms;
    }

    std::string encNs;
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] std::string_view qn, const std::vector<analysis::Symbol>& sList)
        {
            for (const auto& s : sList)
            {
                if (s.type == analysis::SymbolType::Namespace && request.position.line >= s.startLine &&
                    request.position.line <= s.endLine && s.qualifiedName.size() > encNs.size())
                {
                    encNs = s.qualifiedName;
                }
            }
        });
    if (!encNs.empty())
    {
        utils::ForEachEnclosingScope(encNs,
                                     [&](std::string_view scope)
                                     {
                                         if (!syms.empty())
                                         {
                                             return;
                                         }
                                         std::string candidateScoped = utils::JoinScopedName(scope, scopedText);
                                         syms = request.symbolTable.FindSymbols(candidateScoped);
                                     });
    }
    return syms;
}

/**
 * @brief Populates target descriptor from a candidate scoped symbol.
 * @param[in] s Matched symbol.
 * @param[in] qualifierName Extracted qualifier name.
 * @param[in] scopedText Scoped symbol text.
 * @param[in] symbolTable Global symbol table.
 * @param[out] target Target descriptor to populate.
 * @return True if target was successfully populated.
 */
static bool AssignScopedTargetFromSymbol(const analysis::Symbol& s, std::string_view qualifierName,
                                         std::string_view scopedText, const analysis::SymbolTable& symbolTable,
                                         TargetDescriptor& target)
{
    if (s.containerKind == analysis::ContainerKind::Enum ||
        (s.type == analysis::SymbolType::Variable && s.containerKind == analysis::ContainerKind::Enum))
    {
        target.kind = TargetKind::EnumMember;
        target.declaringEnum = FindRealDeclaringEnum(s, qualifierName, symbolTable);
        target.qualifiedName = s.qualifiedName.empty() ? std::string(scopedText) : s.qualifiedName;
        return true;
    }
    if (s.containerKind == analysis::ContainerKind::Namespace || s.type == analysis::SymbolType::Class ||
        s.type == analysis::SymbolType::Interface || s.type == analysis::SymbolType::Enum ||
        s.type == analysis::SymbolType::Typedef)
    {
        target.kind = TargetKind::NamespaceSymbol;
        target.declaringNamespace = s.containerName.empty() ? std::string(qualifierName) : s.containerName;
        target.qualifiedName = s.qualifiedName.empty() ? std::string(scopedText) : s.qualifiedName;
        if (s.type == analysis::SymbolType::Function)
        {
            target.isFunction = true;
            ApplyFunctionSignature(target, s.GetFunction());
        }
        return true;
    }
    if (s.containerKind == analysis::ContainerKind::Class || s.containerKind == analysis::ContainerKind::Interface)
    {
        target.kind = TargetKind::ClassMember;
        target.declaringClass = s.containerName;
        target.qualifiedName = s.qualifiedName.empty() ? std::string(scopedText) : s.qualifiedName;
        target.relatedClasses =
            analysis::GetCompatibleMemberClasses(target.declaringClass, target.name, target.access, symbolTable);
        if (s.type == analysis::SymbolType::Function)
        {
            target.isFunction = true;
            ApplyFunctionSignature(target, s.GetFunction());
        }
        return true;
    }
    return false;
}

/**
 * @brief Resolves scoped identifier target without an AST.
 * @param[in] request Resolution request context.
 * @param[in] identStartByte Byte offset of target identifier start.
 * @param[in] nodeText Symbol identifier name.
 * @param[out] target Target descriptor to populate.
 * @return True if target was resolved.
 */
static bool TryResolveScopedTargetNoAst(const ResolveTargetRequest& request, uint32_t identStartByte,
                                        std::string_view nodeText, TargetDescriptor& target)
{
    std::string_view qualifierName = ExtractPrecedingQualifier(request.sourceCode, identStartByte);
    if (qualifierName.empty())
    {
        return false;
    }

    std::string scopedText = utils::JoinScopedName(qualifierName, nodeText);
    auto syms = FindScopedSymbolsWithEnclosing(request, scopedText);
    for (const auto& s : syms)
    {
        if (AssignScopedTargetFromSymbol(s, qualifierName, scopedText, request.symbolTable, target))
        {
            return true;
        }
    }

    auto enumSyms = FindScopedSymbolsWithEnclosing(request, qualifierName);
    for (const auto& es : enumSyms)
    {
        if (es.type == analysis::SymbolType::Enum)
        {
            target.kind = TargetKind::EnumMember;
            target.declaringEnum = es.qualifiedName.empty() ? es.name : es.qualifiedName;
            target.qualifiedName = utils::JoinScopedName(target.declaringEnum, nodeText);
            return true;
        }
    }
    return false;
}

/**
 * @brief Resolves enum or scoped targets with or without an AST.
 * @param[in] request Resolution request context.
 * @param[in] nodeText Symbol identifier name.
 * @param[in] identStartByte Byte offset of target identifier start.
 * @param[out] target Target descriptor to populate.
 * @return True if target was resolved.
 */
static bool TryResolveEnumOrScopedTarget(const ResolveTargetRequest& request, std::string_view nodeText,
                                         uint32_t identStartByte, TargetDescriptor& target)
{
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] std::string_view qn, const std::vector<analysis::Symbol>& syms)
        {
            for (const auto& s : syms)
            {
                if (s.name == nodeText && s.fileUri == request.uri && s.startLine == request.position.line &&
                    s.type == analysis::SymbolType::Variable && s.containerKind == analysis::ContainerKind::Enum)
                {
                    auto enumSyms = request.symbolTable.FindSymbols(s.containerName);
                    bool isRealEnum = std::any_of(enumSyms.begin(), enumSyms.end(),
                                                  [](const auto& es) { return es.type == analysis::SymbolType::Enum; });
                    if (isRealEnum || target.declaringEnum.empty())
                    {
                        target.kind = TargetKind::EnumMember;
                        target.declaringEnum = s.containerName;
                        target.qualifiedName = s.qualifiedName.empty()
                                                   ? utils::JoinScopedName(s.containerName, nodeText)
                                                   : s.qualifiedName;
                        if (isRealEnum)
                        {
                            return;
                        }
                    }
                }
            }
        });
    if (target.kind == TargetKind::EnumMember)
    {
        return true;
    }
    return TryResolveScopedTargetNoAst(request, identStartByte, nodeText, target);
}

/**
 * @brief Resolves explicit member access expression without an AST using scope references.
 * @param[in] request Resolution request context.
 * @param[in] rootScope Document root scope.
 * @param[in] nodeText Symbol identifier name.
 * @param[out] target Target descriptor to populate.
 * @return True if cursor is on an explicit member access expression.
 */
static bool ResolveExplicitMemberAccessNoAst(const ResolveTargetRequest& request, const Scope* rootScope,
                                             std::string_view nodeText, TargetDescriptor& target)
{
    if (!rootScope)
    {
        return false;
    }
    const analysis::Scope* innerScope =
        FindInnermostScope(rootScope, request.position.line, request.position.character);
    if (!innerScope)
    {
        return false;
    }
    for (const auto& ref : innerScope->references)
    {
        if (ref.isMemberAccess && ref.name == nodeText && ref.startLine == request.position.line &&
            ref.startCharacter <= request.position.character && request.position.character <= ref.endCharacter)
        {
            std::string receiverTypeName = ResolveIndexedReferenceReceiver(ref, *innerScope, request.symbolTable);
            if (!receiverTypeName.empty())
            {
                target.kind = TargetKind::ClassMember;
                target.declaringClass = receiverTypeName;
            }
            return true;
        }
    }
    return false;
}

/**
 * @brief Resolves enclosing class member without an AST.
 * @param[in] request Resolution request context.
 * @param[in] nodeText Symbol identifier name.
 * @param[out] target Target descriptor to populate.
 */
static void ResolveEnclosingClassMember(const ResolveTargetRequest& request, std::string_view nodeText,
                                        TargetDescriptor& target)
{
    std::string enclosingClass = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line);
    if (enclosingClass.empty())
    {
        return;
    }
    auto hierarchy = analysis::GetInheritedTypeHierarchy(enclosingClass, request.symbolTable);
    for (const auto& cls : hierarchy)
    {
        auto syms = request.symbolTable.FindMemberSymbols(cls, nodeText);
        if (HasClassMemberSymbol(syms))
        {
            target.kind = TargetKind::ClassMember;
            target.declaringClass = cls;
            break;
        }
    }
}

/**
 * @brief Checks if a symbol name exists in the table or has global property accessors.
 * @param[in] table Symbol table.
 * @param[in] name Symbol name.
 * @return True if symbols or accessors are found.
 */
static bool HasGlobalSymbolOrAccessor(const SymbolTable& table, std::string_view name)
{
    return table.FindSymbolsPtr(name) != nullptr || !analysis::FindGlobalPropertyAccessors(name, table).empty();
}

std::optional<TargetDescriptor> ResolveTargetSymbol(ResolveTargetRequest& request)
{
    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[TargetResolution] ResolveTargetSymbol at {}:{} in URI: {}",
                                             request.position.line, request.position.character, request.uri));
    }

    uint32_t identStartByte = UINT32_MAX;
    uint32_t identEndByte = UINT32_MAX;
    std::string_view nodeText = GetNodeTextAt(request.sourceCode, request.position, &identStartByte, &identEndByte);
    if (nodeText.empty() || identStartByte == UINT32_MAX || !IsValidIdentifier(nodeText))
    {
        return std::nullopt;
    }

    auto rootScope = request.useCallBindings ? request.scopeIndex.GetOccurrenceRoot(request.uri)
                                             : request.scopeIndex.GetRoot(request.uri);

    TargetDescriptor target;
    target.name = std::string(nodeText);
    if (ResolveTargetFromCall(request, rootScope.get(), target, identStartByte))
    {
        return target;
    }

    GlobalScopeQualifier gsq = GetGlobalScopeQualifierNoAst(request.sourceCode, identStartByte, identEndByte);

    if (gsq.isGlobal)
    {
        return ResolveGlobalScopedTarget(request, gsq, target) ? std::make_optional(target) : std::nullopt;
    }

    if (TryResolveEnumOrScopedTarget(request, nodeText, identStartByte, target))
    {
        return target;
    }

    bool isExplicitMemberAccess = ResolveExplicitMemberAccessNoAst(request, rootScope.get(), nodeText, target);

    if (!isExplicitMemberAccess)
    {
        ResolveLocalTarget(rootScope, nodeText, request, target, identStartByte);
    }
    if (target.kind == TargetKind::GlobalSymbol && !isExplicitMemberAccess)
    {
        ResolveEnclosingClassMember(request, nodeText, target);
    }
    if (target.kind == TargetKind::GlobalSymbol)
    {
        ResolveGlobalFallbackTarget(nodeText, request, target);
    }

    FunctionArgConstraints callInfo{};
    ResolveTargetSignature(target, request, callInfo);
    CaptureCallDeclaration(target, request, identStartByte, identEndByte);

    if (target.kind == TargetKind::GlobalSymbol && !HasGlobalSymbolOrAccessor(request.symbolTable, target.name))
    {
        return std::nullopt;
    }

    return target;
}

/**
 * @brief Extracts effective selection or full range span for a symbol.
 * @param[in] sym Target symbol.
 * @return Coordinate span.
 */
static SymbolSpan GetSymbolSpan(const analysis::Symbol& sym) noexcept
{
    bool hasSel = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0);
    return SymbolSpan{
        .sL = hasSel ? sym.selectionRange.startLine : sym.startLine,
        .sC = hasSel ? sym.selectionRange.startCharacter : sym.startCharacter,
        .eL = hasSel ? sym.selectionRange.endLine : sym.endLine,
        .eC = hasSel ? sym.selectionRange.endCharacter : sym.endCharacter,
    };
}

/**
 * @brief Checks if a function symbol's parameter count matches the target function requirements.
 * @param[in] sym Symbol to test.
 * @param[in] target Target descriptor.
 * @return True if function parameters are compatible.
 */
static bool FunctionMatchesTarget(const analysis::Symbol& sym, const TargetDescriptor& target)
{
    if (target.callDeclaration &&
        (target.kind != TargetKind::ClassMember || target.callDeclaration->qualifiedName == sym.qualifiedName))
    {
        return target.callDeclaration->Matches(sym);
    }
    if (!target.isFunction)
    {
        return true;
    }
    if (!std::holds_alternative<analysis::FunctionSignature>(sym.signature))
    {
        return false;
    }
    const auto& fn = sym.GetFunction();
    size_t minP = 0;
    for (const auto& p : fn.parameters)
    {
        if (p.defaultValue.empty())
        {
            minP++;
        }
    }
    size_t maxP = fn.parameters.size();
    return target.maxArgs >= minP && target.minArgs <= maxP;
}

/**
 * @brief Checks if a symbol name is shadowed by a definition in the specified scope.
 * @param[in] scope Scope to inspect.
 * @param[in] name Symbol name.
 * @return True if shadowed.
 */
static bool IsSymbolShadowedInScope(const analysis::Scope* scope, std::string_view name)
{
    return std::any_of(scope->definitions.begin(), scope->definitions.end(),
                       [name](const auto& def) { return def.name == name; });
}

/**
 * @brief Collects references matching target in a single scope.
 * @param[in] scope Scope to inspect.
 * @param[in] target Target descriptor.
 * @param[in] includeDeclaration True to include declaration site.
 * @param[in,out] collector Occurrence accumulator.
 */
static void CollectScopeReferences(const analysis::Scope* scope, const TargetDescriptor& target,
                                   bool includeDeclaration, OccurrenceCollector& collector)
{
    for (const auto& ref : scope->references)
    {
        if (ref.name != target.name || ref.isMemberAccess)
        {
            continue;
        }
        if (target.localDef.kind == LocalDefinitionKind::Variable && ref.startByte < target.localDef.visibleFromByte)
        {
            continue;
        }
        if (!includeDeclaration && collector.declRanges.contains({target.localUri, ref.startLine, ref.startCharacter}))
        {
            continue;
        }
        collector.Add(target.localUri, ref);
    }
}

/**
 * @brief Collects all occurrences for a local variable or parameter target.
 * @param[in] target Target descriptor.
 * @param[in] includeDeclaration True to include declaration site.
 * @param[in,out] collector Occurrence accumulator.
 */
static void CollectLocalOccurrences(const TargetDescriptor& target, bool includeDeclaration,
                                    OccurrenceCollector& collector)
{
    if (includeDeclaration)
    {
        collector.Add(target.localUri, SourceRange{target.localDef.startLine, target.localDef.startCharacter,
                                                   target.localDef.endLine, target.localDef.endCharacter});
    }
    collector.declRanges.insert({target.localUri, target.localDef.startLine, target.localDef.startCharacter});

    if (!target.definingScope)
    {
        return;
    }

    std::vector<std::pair<const analysis::Scope*, bool>> stack{{target.definingScope.get(), true}};
    while (!stack.empty())
    {
        auto [scope, isRoot] = stack.back();
        stack.pop_back();

        if (!isRoot && IsSymbolShadowedInScope(scope, target.name))
        {
            continue;
        }

        CollectScopeReferences(scope, target, includeDeclaration, collector);

        for (const auto& child : scope->children)
        {
            if (child)
            {
                stack.push_back({child.get(), false});
            }
        }
    }
}

/**
 * @brief Resolves type name for an identifier from scope or global symbol table.
 * @param[in] name Identifier name.
 * @param[in] scope Lexical scope.
 * @param[in] request Collect occurrences request.
 * @param[in] line Source line number.
 * @return Cleaned base type name or empty string.
 */
static std::string ResolveNamedIdentifierType(std::string_view name, const analysis::Scope* scope,
                                              const CollectOccurrencesRequest& request, uint32_t line,
                                              std::string_view fileUri = {})
{
    if (name == "this")
    {
        return GetEnclosingClassName(request.symbolTable, fileUri.empty() ? request.currentUri : fileUri, line);
    }
    const analysis::LocalDefinition* oDef = analysis::ResolveInScope(scope, name);
    if (oDef && !oDef->typeName.empty())
    {
        return analysis::CleanBaseType(oDef->typeName);
    }
    auto gSyms = request.symbolTable.FindSymbols(name);
    for (const auto& gs : gSyms)
    {
        if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(gs.GetVariable().typeName);
        }
        if (gs.type == analysis::SymbolType::Function && !gs.GetFunction().returnType.empty())
        {
            return analysis::CleanBaseType(gs.GetFunction().returnType);
        }
        if (gs.type == analysis::SymbolType::Class || gs.type == analysis::SymbolType::Interface)
        {
            return gs.name;
        }
    }
    return "";
}

/**
 * @brief Resolves receiver type name by scanning preceding references in scope.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] ctx Occurrence scan context.
 * @return Cleaned receiver type or empty string.
 */
static std::string ResolveReceiverFromScope(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                            const OccurrenceScanContext& ctx)
{
    std::string currentReceiverType;
    for (const auto& candRef : scope->references)
    {
        if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
        {
            if (currentReceiverType.empty())
            {
                currentReceiverType =
                    ResolveNamedIdentifierType(candRef.name, scope, ctx.request, ref.startLine, ctx.fileUri);
            }
            else
            {
                std::string memberType =
                    ResolveReceiverMemberType(currentReceiverType, candRef.name, ctx.request.symbolTable);
                if (!memberType.empty())
                {
                    currentReceiverType = std::move(memberType);
                }
                else
                {
                    std::string t =
                        ResolveNamedIdentifierType(candRef.name, scope, ctx.request, ref.startLine, ctx.fileUri);
                    if (!t.empty())
                    {
                        currentReceiverType = t;
                    }
                }
            }
        }
    }
    return currentReceiverType;
}

/**
 * @brief Resolves receiver type name from AST or scope for member expression reference.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] ctx Occurrence scan context.
 * @return Cleaned receiver type or empty string.
 */
static std::string ResolveReceiverType(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                       const OccurrenceScanContext& ctx)
{
    if (auto indexed = ResolveIndexedReferenceReceiver(ref, *scope, ctx.request.symbolTable); !indexed.empty())
    {
        return indexed;
    }
    return ResolveReceiverFromScope(ref, scope, ctx);
}

/**
 * @brief Checks if an enclosing class is in the inheritance or mixin hierarchy of the declaring class.
 * @param[in] cleanEnc Cleaned enclosing class name.
 * @param[in] cleanDecl Cleaned declaring class name.
 * @param[in] symbolTable Global symbol table.
 * @return True if enclosing class is compatible with declaring class hierarchy.
 */
static bool IsInTargetHierarchy(std::string_view cleanEnc, std::string_view cleanDecl,
                                const analysis::SymbolTable& symbolTable)
{
    if (cleanDecl.empty() || analysis::MatchesDeclOrScope(cleanEnc, cleanDecl) ||
        analysis::IsMixinClass(cleanEnc, symbolTable) || analysis::IsMixinClass(cleanDecl, symbolTable))
    {
        return true;
    }
    auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanEnc, symbolTable);
    for (const auto& ancestor : hierarchy)
    {
        if (analysis::MatchesDeclOrScope(analysis::CleanBaseType(ancestor), cleanDecl))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Evaluates whether an implicit member access matches the target class member.
 * @param[in] ref Reference to test.
 * @param[in] scope Lexical scope.
 * @param[in] relatedSet Set of related class names.
 * @param[in] ctx Occurrence scan context.
 * @return True if implicit member access is a valid match.
 */
static bool CheckImplicitMemberAccess(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                      const ankerl::unordered_dense::set<std::string>& relatedSet,
                                      const OccurrenceScanContext& ctx)
{
    std::string encClass = GetEnclosingClassName(ctx.request.symbolTable, ctx.fileUri, ref.startLine);
    if (encClass.empty() ||
        (!relatedSet.contains(encClass) && !relatedSet.contains(std::string(analysis::LastScopeSegment(encClass)))))
    {
        return false;
    }

    std::string cleanEnc = analysis::CleanBaseType(encClass);
    std::string cleanDecl = analysis::CleanBaseType(ctx.request.target.declaringClass);
    if (!IsInTargetHierarchy(cleanEnc, cleanDecl, ctx.request.symbolTable))
    {
        return false;
    }

    const analysis::LocalDefinition* localShadow = analysis::ResolveInScope(scope, ctx.request.target.name);
    return !localShadow || localShadow->kind == analysis::LocalDefinitionKind::Field ||
           localShadow->kind == analysis::LocalDefinitionKind::Method;
}

/**
 * @brief Determines if a class member declaration should be included in occurrences.
 * @param[in] clsName Class name where symbol is declared.
 * @param[in] declaringClass Target's declaring class name.
 * @param[in] symbolTable Global symbol table.
 * @return True if declaration belongs to the target's hierarchy.
 */
static bool ShouldIncludeClassDecl(std::string_view clsName, std::string_view declaringClass,
                                   const analysis::SymbolTable& symbolTable)
{
    std::string_view cleanCls = analysis::CleanBaseTypeView(clsName);
    std::string_view cleanDecl = analysis::CleanBaseTypeView(declaringClass);
    if (cleanCls == cleanDecl || cleanDecl.empty())
    {
        return true;
    }
    auto hier = analysis::GetInheritedTypeHierarchy(cleanCls, symbolTable);
    for (const auto& anc : hier)
    {
        if (analysis::CleanBaseTypeView(anc) == cleanDecl)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Collects declarations of class members across related classes.
 * @param[in] relatedSet Set of related class names.
 * @param[in] request Occurrences request.
 * @param[out] allDeclRanges Set of all declaration coordinate tuples.
 * @param[in,out] collector Occurrence accumulator.
 */
static void CollectClassMemberDeclarations(const ankerl::unordered_dense::set<std::string>& relatedSet,
                                           const CollectOccurrencesRequest& request,
                                           std::set<std::tuple<std::string, uint32_t, uint32_t>>& allDeclRanges,
                                           OccurrenceCollector& collector)
{
    for (const auto& clsName : relatedSet)
    {
        std::string qualifiedName = utils::JoinScopedName(clsName, request.target.name);
        auto syms = request.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : syms)
        {
            if (sym.type == analysis::SymbolType::CallReference)
            {
                continue;
            }
            SymbolSpan span = GetSymbolSpan(sym);
            allDeclRanges.insert({sym.fileUri, span.sL, span.sC});

            if (!FunctionMatchesTarget(sym, request.target))
            {
                continue;
            }

            collector.declRanges.insert({sym.fileUri, span.sL, span.sC});
            if (request.includeDeclaration &&
                ShouldIncludeClassDecl(clsName, request.target.declaringClass, request.symbolTable))
            {
                collector.Add(sym.fileUri, SourceRange{span.sL, span.sC, span.eL, span.eC});
            }
        }
    }
}

/**
 * @brief Evaluates whether a class member reference matches the target symbol.
 * @param[in] ref Reference to test.
 * @param[in] scope Lexical scope.
 * @param[in] relatedSet Set of related class names.
 * @param[in] ctx Occurrence scan context.
 * @return True if reference is a match.
 */
static bool IsClassMemberReferenceMatch(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                        const ankerl::unordered_dense::set<std::string>& relatedSet,
                                        const OccurrenceScanContext& ctx)
{
    if (ref.isMemberAccess)
    {
        std::string rType = ResolveReceiverType(ref, scope, ctx);
        if (!rType.empty())
        {
            return relatedSet.contains(rType);
        }
        if (ctx.fileUri != ctx.request.currentUri)
        {
            std::string encClass = GetEnclosingClassName(ctx.request.symbolTable, ctx.fileUri, ref.startLine);
            return !encClass.empty() && (relatedSet.contains(encClass) ||
                                         relatedSet.contains(std::string(analysis::LastScopeSegment(encClass))));
        }
        return false;
    }
    return CheckImplicitMemberAccess(ref, scope, relatedSet, ctx);
}

static bool IsBoundCallOccurrence(const Scope& root, const LocalReference& ref, const TargetDescriptor& target)
{
    const auto* bindings = root.GetCallBindings();
    if (!target.callDeclaration || !ref.isCall || !bindings)
    {
        return false;
    }
    const auto* bound = bindings->Find(ref.startByte);
    return bound &&
           (target.kind != TargetKind::ClassMember || target.callDeclaration->qualifiedName == bound->qualifiedName);
}

static bool IsUnboundSearchCandidate(const Scope& root, const LocalReference& ref, const OccurrenceScanContext& ctx)
{
    return ref.name == ctx.request.target.name &&
           !ctx.declRanges.contains({ctx.fileUri, ref.startLine, ref.startCharacter}) &&
           !IsBoundCallOccurrence(root, ref, ctx.request.target) && MatchesCallArguments(ref, ctx.request.target);
}

/**
 * @brief Scans a document scope tree for class member occurrences.
 * @param[in] root Document root scope.
 * @param[in] relatedSet Set of related class names.
 * @param[in,out] ctx Occurrence scan context.
 */
static void ScanDocumentForClassMember(const analysis::Scope* root,
                                       const ankerl::unordered_dense::set<std::string>& relatedSet,
                                       OccurrenceScanContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (!IsUnboundSearchCandidate(*root, ref, ctx))
            {
                continue;
            }
            if (ctx.request.target.access == analysis::AccessModifier::Private ||
                ctx.request.target.access == analysis::AccessModifier::Protected)
            {
                std::string encClass = GetEnclosingClassName(ctx.request.symbolTable, ctx.fileUri, ref.startLine);
                if (encClass.empty() || (!relatedSet.contains(encClass) &&
                                         !relatedSet.contains(std::string(analysis::LastScopeSegment(encClass)))))
                {
                    continue;
                }
            }
            if (IsClassMemberReferenceMatch(ref, s, relatedSet, ctx))
            {
                ctx.collector.Add(ctx.fileUri, ref);
            }
        }

        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects all occurrences of a class member across all indexed documents.
 * @param[in] request Occurrences request.
 * @param[in,out] collector Occurrence accumulator.
 */
static void CollectClassMemberOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    ankerl::unordered_dense::set<std::string> relatedSet(request.target.relatedClasses.begin(),
                                                         request.target.relatedClasses.end());
    if (!request.target.declaringClass.empty())
    {
        relatedSet.insert(request.target.declaringClass);
    }

    std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
    CollectClassMemberDeclarations(relatedSet, request, allDeclRanges, collector);

    auto allUris = GetAllIndexedFileUris(request.symbolTable, request.currentUri);
    for (const auto& fileUri : allUris)
    {
        auto docScopeRoot = request.scopeIndex.GetOccurrenceRoot(fileUri, request.occurrenceModule);
        if (!docScopeRoot)
        {
            continue;
        }
        OccurrenceScanContext ctx{
            .fileUri = fileUri,
            .request = request,
            .declRanges = allDeclRanges,
            .collector = collector,
        };
        ScanDocumentForClassMember(docScopeRoot.get(), relatedSet, ctx);
    }
}

/**
 * @brief Collects declarations of a namespace or global symbol.
 * @param[in] symQuery Symbol name or qualified name to query.
 * @param[in] request Occurrences request.
 * @param[out] allDeclRanges Declaration coordinate tuples.
 * @param[in,out] collector Accumulator.
 */
static void CollectDeclarations(std::string_view symQuery, const CollectOccurrencesRequest& request,
                                std::set<std::tuple<std::string, uint32_t, uint32_t>>& allDeclRanges,
                                OccurrenceCollector& collector)
{
    auto syms = request.symbolTable.FindSymbols(symQuery);
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::CallReference)
        {
            continue;
        }
        SymbolSpan span = GetSymbolSpan(sym);
        allDeclRanges.insert({sym.fileUri, span.sL, span.sC});

        if (!FunctionMatchesTarget(sym, request.target))
        {
            continue;
        }

        collector.declRanges.insert({sym.fileUri, span.sL, span.sC});
        if (request.includeDeclaration)
        {
            collector.Add(sym.fileUri, SourceRange{span.sL, span.sC, span.eL, span.eC});
        }
    }
}

/**
 * @brief Checks whether a reference is enclosed within a namespace region or explicitly scoped.
 * @param[in] ref Reference to check.
 * @param[in] nsRanges Line ranges of declaring namespace.
 * @param[in] ctx Occurrence scan context.
 * @param[in] scope Lexical scope.
 * @return True if reference is inside declaring namespace.
 */
static bool IsRefInsideNamespace(const analysis::LocalReference& ref,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& nsRanges,
                                 const OccurrenceScanContext& ctx, const analysis::Scope* scope)
{
    const auto qualifier = GetIndexedReferenceQualifier(ref, *scope);
    if (ref.isMemberAccess || !qualifier.empty())
    {
        return MatchesReferenceQualifier(qualifier, ctx.request.target.declaringNamespace, *scope);
    }
    return std::ranges::any_of(nsRanges, [&](const auto& range)
                               { return ref.startLine >= range.first && ref.startLine <= range.second; });
}

/**
 * @brief Scans a document scope tree for namespace symbol references.
 * @param[in] root Document root scope.
 * @param[in] nsRanges Namespace line spans in fileUri.
 * @param[in,out] ctx Occurrence scan context.
 */
static void ScanDocumentForNamespace(const analysis::Scope* root,
                                     const std::vector<std::pair<uint32_t, uint32_t>>& nsRanges,
                                     OccurrenceScanContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (!IsUnboundSearchCandidate(*root, ref, ctx))
            {
                continue;
            }
            if (!IsRefInsideNamespace(ref, nsRanges, ctx, s))
            {
                continue;
            }
            if (analysis::IsShadowedByFunctionLocal(s, ctx.request.target.name))
            {
                continue;
            }
            ctx.collector.Add(ctx.fileUri, ref);
        }

        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects occurrences of a namespace-scoped symbol across documents.
 * @param[in] request Occurrences request.
 * @param[in,out] collector Accumulator.
 */
static void CollectNamespaceOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
    CollectDeclarations(request.target.qualifiedName, request, allDeclRanges, collector);

    auto allUris = GetAllIndexedFileUris(request.symbolTable, request.currentUri);
    for (const auto& fileUri : allUris)
    {
        auto docScopeRoot = request.scopeIndex.GetOccurrenceRoot(fileUri, request.occurrenceModule);
        if (!docScopeRoot)
        {
            continue;
        }

        auto nsRanges =
            CollectNamespaceAndContainerLineRanges(request.symbolTable, fileUri, request.target.declaringNamespace);

        OccurrenceScanContext ctx{
            .fileUri = fileUri,
            .request = request,
            .declRanges = allDeclRanges,
            .collector = collector,
        };
        ScanDocumentForNamespace(docScopeRoot.get(), nsRanges, ctx);
    }
}

/**
 * @brief Scans a document scope tree for global symbol references.
 * @param[in] root Document root scope.
 * @param[in,out] ctx Occurrence scan context.
 */
static void ScanDocumentForGlobal(const analysis::Scope* root, OccurrenceScanContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.isMemberAccess || !IsUnboundSearchCandidate(*root, ref, ctx))
            {
                continue;
            }
            if (analysis::IsShadowedByFunctionLocal(s, ctx.request.target.name))
            {
                continue;
            }
            ctx.collector.Add(ctx.fileUri, ref);
        }

        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects occurrences of a global symbol across documents.
 * @param[in] request Occurrences request.
 * @param[in,out] collector Accumulator.
 */
static void CollectGlobalOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
    CollectDeclarations(request.target.name, request, allDeclRanges, collector);

    auto allUris = GetAllIndexedFileUris(request.symbolTable, request.currentUri);
    for (const auto& fileUri : allUris)
    {
        auto docScopeRoot = request.scopeIndex.GetOccurrenceRoot(fileUri, request.occurrenceModule);
        if (!docScopeRoot)
        {
            continue;
        }
        OccurrenceScanContext ctx{
            .fileUri = fileUri,
            .request = request,
            .declRanges = allDeclRanges,
            .collector = collector,
        };
        ScanDocumentForGlobal(docScopeRoot.get(), ctx);
    }
}

static bool MatchesDeclaringNamespace(const analysis::LocalReference& ref, std::string_view declaringNamespace,
                                      const OccurrenceScanContext& ctx)
{
    auto nsRanges = CollectNamespaceAndContainerLineRanges(ctx.request.symbolTable, ctx.fileUri, declaringNamespace);
    for (const auto& nr : nsRanges)
    {
        if (ref.startLine >= nr.first && ref.startLine <= nr.second)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if an unscoped reference matches the enum member target without collisions.
 * @param[in] ref Local reference.
 * @param[in] scope Scope of reference.
 * @param[in] ctx Occurrence scan context.
 * @return True if the unscoped reference safely resolves to the target enum member.
 */
static bool IsUnscopedEnumMemberMatch(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                      const OccurrenceScanContext& ctx)
{
    if (analysis::IsShadowedByFunctionLocal(scope, ctx.request.target.name))
    {
        return false;
    }
    const std::string_view declaringNamespace = utils::ParentScopeView(ctx.request.target.declaringEnum);
    if (!declaringNamespace.empty())
    {
        return MatchesDeclaringNamespace(ref, declaringNamespace, ctx);
    }

    size_t enumCount = 0;
    auto syms = ctx.request.symbolTable.FindSymbols(ctx.request.target.name);
    for (const auto& s : syms)
    {
        if (s.type == analysis::SymbolType::Variable &&
            std::holds_alternative<analysis::VariableSignature>(s.signature) && s.GetVariable().isEnumConstant)
        {
            enumCount++;
        }
    }
    if (enumCount <= 1)
    {
        return true;
    }
    for (const auto& decl : ctx.declRanges)
    {
        if (std::get<0>(decl) == ctx.fileUri)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a reference matches the target enum member.
 * @param[in] ref Reference to check.
 * @param[in] scope Scope containing reference.
 * @param[in] ctx Occurrence scan context.
 * @return True if reference belongs to the declaring enum.
 */
static bool IsEnumMemberReferenceMatch(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                       const OccurrenceScanContext& ctx)
{
    const auto qualifier = GetIndexedReferenceQualifier(ref, *scope);
    if (ref.isMemberAccess || !qualifier.empty())
    {
        return MatchesEnumReferenceQualifier(qualifier, ctx.request.target.declaringEnum, *scope);
    }
    return IsUnscopedEnumMemberMatch(ref, scope, ctx);
}

/**
 * @brief Scans a document scope tree for enum member occurrences.
 * @param[in] root Document root scope.
 * @param[in,out] ctx Occurrence scan context.
 */
static void ScanDocumentForEnumMember(const analysis::Scope* root, OccurrenceScanContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.name != ctx.request.target.name ||
                ctx.declRanges.contains({ctx.fileUri, ref.startLine, ref.startCharacter}))
            {
                continue;
            }
            if (IsEnumMemberReferenceMatch(ref, s, ctx))
            {
                ctx.collector.Add(ctx.fileUri, ref);
            }
        }

        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects all occurrences of an enum member across documents.
 * @param[in] request Occurrences request.
 * @param[in,out] collector Occurrence accumulator.
 */
static void CollectEnumMemberOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
    CollectDeclarations(request.target.qualifiedName, request, allDeclRanges, collector);

    if (allDeclRanges.empty())
    {
        auto syms = request.symbolTable.FindSymbols(request.target.name);
        for (const auto& sym : syms)
        {
            if (sym.containerName == request.target.declaringEnum || sym.qualifiedName == request.target.qualifiedName)
            {
                SymbolSpan span = GetSymbolSpan(sym);
                allDeclRanges.insert({sym.fileUri, span.sL, span.sC});
                collector.declRanges.insert({sym.fileUri, span.sL, span.sC});
                if (request.includeDeclaration)
                {
                    collector.Add(sym.fileUri, SourceRange{span.sL, span.sC, span.eL, span.eC});
                }
            }
        }
    }

    auto allUris = GetAllIndexedFileUris(request.symbolTable, request.currentUri);
    for (const auto& fileUri : allUris)
    {
        auto docScopeRoot = request.scopeIndex.GetOccurrenceRoot(fileUri, request.occurrenceModule);
        if (!docScopeRoot)
        {
            continue;
        }
        OccurrenceScanContext ctx{
            .fileUri = fileUri,
            .request = request,
            .declRanges = allDeclRanges,
            .collector = collector,
        };
        ScanDocumentForEnumMember(docScopeRoot.get(), ctx);
    }
}

static void CollectBoundCallOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    if (!request.target.callDeclaration)
    {
        return;
    }
    request.scopeIndex.ForEachBoundUse(*request.target.callDeclaration, request.occurrenceModule,
                                       [&](const std::string& uri, std::span<const SourceRange> ranges)
                                       {
                                           for (const auto& use : ranges)
                                           {
                                               collector.Add(uri, use);
                                           }
                                       });
}

std::vector<OccurrenceLocation> CollectOccurrences(const CollectOccurrencesRequest& request)
{
    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[TargetResolution] CollectOccurrences for target '{}' (kind={}) in {}",
                                             request.target.name, static_cast<int>(request.target.kind),
                                             request.currentUri));
    }

    OccurrenceCollector collector;
    CollectBoundCallOccurrences(request, collector);

    if (request.target.kind == TargetKind::Local)
    {
        CollectLocalOccurrences(request.target, request.includeDeclaration, collector);
    }
    else if (request.target.kind == TargetKind::ClassMember)
    {
        CollectClassMemberOccurrences(request, collector);
    }
    else if (request.target.kind == TargetKind::NamespaceSymbol)
    {
        CollectNamespaceOccurrences(request, collector);
    }
    else if (request.target.kind == TargetKind::EnumMember)
    {
        CollectEnumMemberOccurrences(request, collector);
    }
    else
    {
        CollectGlobalOccurrences(request, collector);
    }

    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace(fmt::format("[TargetResolution] Found {} occurrences for target '{}'",
                                             collector.results.size(), request.target.name));
    }

    return collector.results;
}

} // namespace angel_lsp::analysis
