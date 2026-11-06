#include "features/definition/DefinitionHandler.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TargetResolution.h"
#include "analysis/types/VirtualMixinContext.h"
#include "config/EngineRuleConfig.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/IncludeResolver.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Formats an LSP Location from a symbol definition.
 * @param[in] sym Target symbol.
 * @param[in] request Definition context.
 * @return Formatted LSP Location with appropriate URI and mapped line range.
 */
static lsp::Location MakeLocation(const analysis::Symbol& sym, const DefinitionRequest& request)
{
    if (request.uri.starts_with("angelscript-virtual:") || request.uri.starts_with("angelscript-virtual://"))
    {
        std::string mixinPart = analysis::SymbolTable::ExtractVirtualMixinName(request.uri);
        const analysis::Symbol* mSym = nullptr;
        auto cand = request.symbolTable.FindSymbols(mixinPart);
        for (const auto& c : cand)
        {
            if (c.type == analysis::SymbolType::Class)
            {
                mSym = &c;
                break;
            }
        }
        if (!mSym)
        {
            auto sCand = request.symbolTable.FindTypeSymbolsByShortName(analysis::LastScopeSegment(mixinPart));
            for (const auto& c : sCand)
            {
                if (c.type == analysis::SymbolType::Class)
                {
                    mSym = &c;
                    break;
                }
            }
        }

        if (mSym && sym.fileUri == mSym->fileUri)
        {
            uint32_t mappedStartLine = analysis::SymbolTable::PhysicalToVirtualLine(sym.startLine, mSym->startLine);
            uint32_t lineDiff = sym.endLine >= sym.startLine ? (sym.endLine - sym.startLine) : 0;
            uint32_t mappedEndLine = mappedStartLine + lineDiff;
            return lsp::Location{lsp::DocumentUri::parse(request.uri),
                                 lsp::Range{lsp::Position{mappedStartLine, sym.startCharacter},
                                            lsp::Position{mappedEndLine, sym.endCharacter}}};
        }
    }

    std::string_view targetUri = sym.fileUri.empty() ? request.uri : std::string_view(sym.fileUri);
    const bool hasFullRange = sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0;
    return lsp::Location{lsp::DocumentUri::parse(targetUri),
                         hasFullRange ? common::ToLspRange(sym.fullRange)
                                      : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                                   lsp::Position{sym.endLine, sym.endCharacter}}};
}

/**
 * @brief Converts symbol candidates to deduplicated LSP locations.
 * @param[in] symbols Candidate symbols.
 * @param[in] request Definition request context.
 * @return Formatted and deduplicated LSP Location vector.
 */
static std::vector<lsp::Location> ConvertSymbolsToLocations(const std::vector<analysis::Symbol>& symbols,
                                                            const DefinitionRequest& request)
{
    std::vector<lsp::Location> locations;
    bool hasWorkspaceSymbol = false;
    for (const auto& sym : symbols)
    {
        if (sym.type != analysis::SymbolType::CallReference &&
            !utils::IsPredefinedFile(sym.fileUri, request.predefinedExtension))
        {
            hasWorkspaceSymbol = true;
            break;
        }
    }

    for (const auto& sym : symbols)
    {
        if (sym.type != analysis::SymbolType::CallReference)
        {
            if (hasWorkspaceSymbol && utils::IsPredefinedFile(sym.fileUri, request.predefinedExtension))
            {
                continue;
            }
            auto loc = MakeLocation(sym, request);
            bool duplicate = false;
            for (const auto& existing : locations)
            {
                if (existing.uri.toString() == loc.uri.toString() && common::SameRange(existing.range, loc.range))
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                locations.push_back(std::move(loc));
            }
        }
    }
    return locations;
}

/**
 * @brief Attempts to resolve definition when cursor is within an `#include` directive.
 * @param[in] request Definition request context.
 * @return Location vector pointing to the resolved include file, or nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveIncludeDirective(const DefinitionRequest& request)
{
    if (!request.resolveInclude)
    {
        return std::nullopt;
    }

    std::string_view line = common::ExtractLineAt(request.sourceCode, request.position.line);
    if (line.empty())
    {
        return std::nullopt;
    }

    const auto info = angel_lsp::utils::IncludeResolver::ParseSingleLineInclude(
        line, static_cast<size_t>(request.position.character));
    if (!info || info->rawPath.empty())
    {
        return std::nullopt;
    }

    std::string resolved = request.resolveInclude(info->rawPath);
    if (resolved.empty())
    {
        return std::nullopt;
    }

    lsp::DocumentUri targetUri =
        resolved.rfind("file://", 0) == 0 ? lsp::DocumentUri::parse(resolved) : lsp::Uri::fileUriFromPath(resolved);
    return std::vector<lsp::Location>{
        lsp::Location{std::move(targetUri), lsp::Range{lsp::Position{0, 0}, lsp::Position{0, 0}}}};
}

/**
 * @brief Checks if a symbol type represents a user-defined type definition target.
 * @param[in] type Symbol type enum.
 * @return True if Class, Interface, Enum, Typedef, or Funcdef.
 */
static inline bool IsTypeDefType(analysis::SymbolType type) noexcept
{
    return type == analysis::SymbolType::Class || type == analysis::SymbolType::Interface ||
           type == analysis::SymbolType::Enum || type == analysis::SymbolType::Typedef ||
           type == analysis::SymbolType::Funcdef;
}

/**
 * @brief Finds innermost enclosing namespace for a source position.
 * @param[in] uri Document URI.
 * @param[in] line Document line number.
 * @param[in] table Symbol table.
 * @return Qualified enclosing namespace name, or empty string.
 */
static std::string FindEnclosingNamespace(std::string_view uri, uint32_t line, const analysis::SymbolTable& table)
{
    std::string enclosingNs;
    uint32_t bestSpan = UINT32_MAX;
    const std::string fileUri(uri);
    table.ForEachSymbolInFile(fileUri,
                              [&]([[maybe_unused]] const std::string& key, const std::vector<analysis::Symbol>& symbols)
                              {
                                  for (const auto& sym : symbols)
                                  {
                                      if (sym.type != analysis::SymbolType::Namespace ||
                                          (sym.fileUri != uri && utils::PathToUri(sym.fileUri) != uri))
                                      {
                                          continue;
                                      }
                                      const auto range =
                                          sym.fullRange.endLine != 0 ? sym.fullRange : sym.selectionRange;
                                      if (line >= range.startLine && line <= range.endLine)
                                      {
                                          const uint32_t span = range.endLine - range.startLine;
                                          if (span <= bestSpan)
                                          {
                                              bestSpan = span;
                                              enclosingNs = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                                          }
                                      }
                                  }
                              });
    return enclosingNs;
}

/**
 * @brief Resolves type name for a given identifier using local scopes, variables, or accessors.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] request Definition request context.
 * @return Clean target type name or empty string.
 */
static std::string ResolveTargetTypeName(std::string_view nodeText, const DefinitionRequest& request)
{
    const std::string enclosingNs = FindEnclosingNamespace(request.uri, request.position.line, request.symbolTable);

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    if (rootScope)
    {
        const analysis::Scope* scope =
            FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
        if (scope)
        {
            const analysis::LocalDefinition* def = analysis::ResolveInScopeAt(scope, nodeText, UINT32_MAX);
            if (def && !def->typeName.empty())
            {
                return analysis::CleanBaseType(def->typeName);
            }
        }
    }

    if (!enclosingNs.empty())
    {
        const std::string scoped = utils::JoinScopedName(enclosingNs, nodeText);
        for (const auto& sym : request.symbolTable.FindSymbols(scoped))
        {
            if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().typeName.empty())
            {
                return analysis::CleanBaseType(sym.GetVariable().typeName);
            }
            if (sym.type == analysis::SymbolType::Function && !sym.GetFunction().returnType.empty())
            {
                return analysis::CleanBaseType(sym.GetFunction().returnType);
            }
        }
        for (const auto prefix : {"get_", "set_"})
        {
            const std::string acc = utils::JoinScopedName(enclosingNs, std::string(prefix) + std::string(nodeText));
            const auto accSyms = request.symbolTable.FindSymbols(acc);
            const std::string pType = analysis::PropertyTypeFromAccessors(accSyms);
            if (!pType.empty())
            {
                return analysis::CleanBaseType(pType);
            }
        }
    }

    auto symbols = request.symbolTable.FindSymbols(nodeText);
    for (const auto& sym : symbols)
    {
        if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(sym.GetVariable().typeName);
        }
        if (sym.type == analysis::SymbolType::Function && !sym.GetFunction().returnType.empty())
        {
            return analysis::CleanBaseType(sym.GetFunction().returnType);
        }
        if (IsTypeDefType(sym.type))
        {
            return sym.name;
        }
    }

    auto accessors = analysis::FindGlobalPropertyAccessors(nodeText, request.symbolTable);
    std::string propType = analysis::PropertyTypeFromAccessors(accessors);
    if (!propType.empty())
    {
        return analysis::CleanBaseType(propType);
    }

    return analysis::CleanBaseType(nodeText);
}

/**
 * @brief Attempts to resolve cursor to overridden base method symbols.
 * @param[in] nodeText Method name under cursor.
 * @param[in] innerScope Innermost lexical scope at cursor position.
 * @param[in] request Definition request.
 * @return Base method locations or std::nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveOverriddenMethod(std::string_view nodeText,
                                                                            const analysis::Scope* innerScope,
                                                                            const DefinitionRequest& request)
{
    std::string enclosingClass;
    if (innerScope && !innerScope->GetEnclosingClass().empty())
    {
        enclosingClass = std::string(innerScope->GetEnclosingClass());
    }
    else
    {
        enclosingClass = request.symbolTable.FindEnclosingClassName(request.uri, request.position.line);
    }
    if (enclosingClass.empty())
    {
        return std::nullopt;
    }

    auto localMethods = request.symbolTable.FindMemberSymbols(enclosingClass, nodeText);
    bool isDecl = false;
    for (const auto& m : localMethods)
    {
        if (m.startLine == request.position.line)
        {
            isDecl = true;
            break;
        }
    }
    if (!isDecl)
    {
        return std::nullopt;
    }

    analysis::LinkedBaseOptions linkedOpts;
    if (request.engineRules)
    {
        linkedOpts.proxyMap = &request.engineRules->unusedRules.baseClassProxyMap;
        linkedOpts.delegateProps = request.engineRules->unusedRules.baseClassDelegateProperties;
    }
    auto overriddenSyms =
        analysis::FindOverriddenBaseMethodSymbols(enclosingClass, nodeText, request.symbolTable, linkedOpts);
    if (overriddenSyms.empty())
    {
        return std::nullopt;
    }

    auto locs = ConvertSymbolsToLocations(overriddenSyms, request);
    return locs.empty() ? std::nullopt : std::make_optional(std::move(locs));
}

/**
 * @brief Attempts to resolve cursor on a local variable or parameter declaration.
 * @param[in] nodeText Identifier text.
 * @param[in] innerScope Innermost lexical scope.
 * @param[in] request Definition request.
 * @return Declaration location or std::nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveLocalDeclaration(std::string_view nodeText,
                                                                            const analysis::Scope* innerScope,
                                                                            const DefinitionRequest& request)
{
    if (!innerScope)
    {
        return std::nullopt;
    }

    for (const auto& def : innerScope->definitions)
    {
        if (def.name == nodeText && def.startLine == request.position.line &&
            def.startCharacter <= request.position.character && request.position.character <= def.endCharacter)
        {
            auto range = common::FindIdentifierRangeAtPosition(request.sourceCode, request.position);
            if (range)
            {
                return std::vector<lsp::Location>{lsp::Location{lsp::DocumentUri::parse(request.uri), *range}};
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Skips consecutive whitespace characters.
 * @param[in] s Source string view.
 * @param[in] pos Starting offset.
 * @return Offset after whitespace.
 */
static size_t SkipWhitespace(std::string_view s, size_t pos) noexcept
{
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
    {
        pos++;
    }
    return pos;
}

/**
 * @brief Counts comma-separated argument tokens inside a call expression.
 * @param[in] sourceCode Document source text.
 * @param[in] i Starting offset inside argument list.
 * @return Number of arguments.
 */
static size_t CountArgumentTokens(std::string_view sourceCode, size_t i) noexcept
{
    size_t count = 1;
    int parenDepth = 0;
    bool inQuote = false;
    while (i < sourceCode.size())
    {
        char c = sourceCode[i];
        if (c == '"' && (i == 0 || sourceCode[i - 1] != '\\'))
        {
            inQuote = !inQuote;
        }
        else if (!inQuote)
        {
            if (c == '(')
            {
                parenDepth++;
            }
            else if (c == ')')
            {
                if (parenDepth == 0)
                {
                    break;
                }
                parenDepth--;
            }
            else if (c == ',' && parenDepth == 0)
            {
                count++;
            }
        }
        i++;
    }
    return count;
}

/**
 * @brief Counts the number of arguments in a function call at cursor.
 * @param[in] sourceCode Document source text.
 * @param[in] pos Cursor position.
 * @return Number of arguments, or 0 if no arguments or not a call.
 */
static size_t CountCallArguments(std::string_view sourceCode, lsp::Position pos) noexcept
{
    size_t lineStart = common::ExtractLineStartByte(sourceCode, pos.line);
    if (lineStart >= sourceCode.size())
    {
        return 0;
    }
    size_t i = lineStart + pos.character;
    while (i < sourceCode.size() && common::IsIdentifierChar(sourceCode[i]))
    {
        i++;
    }
    i = SkipWhitespace(sourceCode, i);
    if (i >= sourceCode.size() || sourceCode[i] != '(')
    {
        return 0;
    }
    i = SkipWhitespace(sourceCode, i + 1);
    if (i < sourceCode.size() && sourceCode[i] == ')')
    {
        return 0;
    }
    return CountArgumentTokens(sourceCode, i);
}

/**
 * @brief Resolves class member targets including related class hierarchies.
 * @param[in] target Target symbol descriptor.
 * @param[in] request Definition request context.
 * @return Resolved member locations.
 */
static std::vector<lsp::Location> ResolveClassMemberLocations(const analysis::TargetDescriptor& target,
                                                              const DefinitionRequest& request)
{
    if (target.callDeclaration.has_value())
    {
        return {lsp::Location{lsp::DocumentUri::parse(target.callDeclaration->uri),
                              common::ToLspRange(target.callDeclaration->range)}};
    }
    auto memberSyms = request.symbolTable.FindMemberSymbols(target.declaringClass, target.name);
    size_t argCount = CountCallArguments(request.sourceCode, request.position);
    if (argCount > 0)
    {
        std::vector<analysis::Symbol> arityMatched;
        for (const auto& s : memberSyms)
        {
            if (s.type == analysis::SymbolType::Function && s.GetFunction().parameters.size() == argCount)
            {
                arityMatched.push_back(s);
            }
        }
        if (!arityMatched.empty())
        {
            return ConvertSymbolsToLocations(arityMatched, request);
        }
        for (const auto& cls : target.relatedClasses)
        {
            auto relSyms = request.symbolTable.FindMemberSymbols(cls, target.name);
            for (const auto& s : relSyms)
            {
                if (s.type == analysis::SymbolType::Function && s.GetFunction().parameters.size() == argCount)
                {
                    arityMatched.push_back(s);
                }
            }
            if (!arityMatched.empty())
            {
                return ConvertSymbolsToLocations(arityMatched, request);
            }
        }
    }
    if (memberSyms.empty())
    {
        for (const auto& cls : target.relatedClasses)
        {
            auto relSyms = request.symbolTable.FindMemberSymbols(cls, target.name);
            if (!relSyms.empty())
            {
                memberSyms = std::move(relSyms);
                break;
            }
        }
    }
    return ConvertSymbolsToLocations(memberSyms, request);
}

/**
 * @brief Checks if a container symbol type matches a member symbol container kind.
 * @param[in] containerType SymbolType of container (Class, Interface, Namespace).
 * @param[in] memberContainerKind ContainerKind of candidate member symbol.
 * @return True if kinds match.
 */
static bool ContainerKindMatches(analysis::SymbolType containerType,
                                 analysis::ContainerKind memberContainerKind) noexcept
{
    switch (containerType)
    {
    case analysis::SymbolType::Class:
        return memberContainerKind == analysis::ContainerKind::Class;
    case analysis::SymbolType::Interface:
        return memberContainerKind == analysis::ContainerKind::Interface;
    case analysis::SymbolType::Namespace:
        return memberContainerKind == analysis::ContainerKind::Namespace;
    default:
        return false;
    }
}

/**
 * @brief Disambiguates candidate container symbols by matching against following scoped member.
 * @param[in,out] symbols Candidate container symbols.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] pos Cursor position.
 * @param[in] table Symbol table.
 */
static void DisambiguateCandidateContainers(std::vector<analysis::Symbol>& symbols, std::string_view sourceCode,
                                            lsp::Position pos, const analysis::SymbolTable& table)
{
    if (symbols.size() <= 1)
    {
        return;
    }
    size_t lineStart = common::ExtractLineStartByte(sourceCode, pos.line);
    if (lineStart >= sourceCode.size())
    {
        return;
    }
    size_t col = pos.character;
    while (lineStart + col < sourceCode.size() && common::IsIdentifierChar(sourceCode[lineStart + col]))
    {
        col++;
    }
    if (lineStart + col + 2 > sourceCode.size() || sourceCode[lineStart + col] != ':' ||
        sourceCode[lineStart + col + 1] != ':')
    {
        return;
    }
    size_t mStart = col + 2;
    size_t mEnd = mStart;
    while (lineStart + mEnd < sourceCode.size() && common::IsIdentifierChar(sourceCode[lineStart + mEnd]))
    {
        mEnd++;
    }
    if (mEnd <= mStart)
    {
        return;
    }
    std::string memberName(sourceCode.substr(lineStart + mStart, mEnd - mStart));
    std::vector<analysis::Symbol> matched;
    for (const auto& sym : symbols)
    {
        if (sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface ||
            sym.type == analysis::SymbolType::Namespace)
        {
            const std::string& containerName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            auto memberSyms = table.FindMemberSymbols(containerName, memberName);
            for (const auto& m : memberSyms)
            {
                if (ContainerKindMatches(sym.type, m.containerKind))
                {
                    matched.push_back(sym);
                    break;
                }
            }
        }
    }
    if (matched.size() == 1)
    {
        symbols = std::move(matched);
    }
}

/**
 * @brief Resolves a target symbol descriptor to LSP locations across symbols and accessors.
 * @param[in] target Target symbol descriptor.
 * @param[in] request Definition request context.
 * @return Vector of resolved locations.
 */
static std::vector<lsp::Location> ResolveTargetLocations(const analysis::TargetDescriptor& target,
                                                         const DefinitionRequest& request)
{
    if (target.kind == analysis::TargetKind::Local)
    {
        const auto& def = target.localDef;
        const bool hasFull = (def.fullEndLine > 0 || def.fullEndCharacter > 0);
        uint32_t sLine = hasFull ? def.fullStartLine : def.startLine;
        uint32_t sChar = hasFull ? def.fullStartCharacter : def.startCharacter;
        uint32_t eLine = hasFull ? def.fullEndLine : def.endLine;
        uint32_t eChar = hasFull ? def.fullEndCharacter : def.endCharacter;
        return {lsp::Location{lsp::DocumentUri::parse(target.localUri),
                              lsp::Range{lsp::Position{sLine, sChar}, lsp::Position{eLine, eChar}}}};
    }
    if (target.kind == analysis::TargetKind::ClassMember)
    {
        return ResolveClassMemberLocations(target, request);
    }
    if (target.kind == analysis::TargetKind::NamespaceSymbol || target.kind == analysis::TargetKind::EnumMember)
    {
        auto syms = request.symbolTable.FindSymbols(target.qualifiedName);
        if (syms.empty())
        {
            syms = request.symbolTable.FindSymbols(target.name);
        }
        return ConvertSymbolsToLocations(syms, request);
    }
    if (target.callDeclaration.has_value())
    {
        return {lsp::Location{lsp::DocumentUri::parse(target.callDeclaration->uri),
                              common::ToLspRange(target.callDeclaration->range)}};
    }
    auto syms = request.symbolTable.FindSymbols(target.name);
    if (syms.empty())
    {
        syms = analysis::FindGlobalPropertyAccessors(target.name, request.symbolTable);
    }
    DisambiguateCandidateContainers(syms, request.sourceCode, request.position, request.symbolTable);
    return ConvertSymbolsToLocations(syms, request);
}

/**
 * @brief Attempts to resolve scoped identifier references (e.g. Foo::Bar).
 * @param[in] request Definition request context.
 * @return Resolved scoped locations or std::nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveScopedIdentifier(const DefinitionRequest& request)
{
    std::string_view scopedName = common::FindScopedIdentifierAtPosition(request.sourceCode, request.position);
    if (scopedName.empty() || !utils::ContainsScopeSeparator(scopedName))
    {
        return std::nullopt;
    }

    auto [qualifier, memberName] = utils::SplitScopedName(scopedName);
    if (qualifier.empty() || memberName.empty())
    {
        return std::nullopt;
    }

    auto memberSyms = request.symbolTable.FindMemberSymbols(qualifier, memberName);
    if (memberSyms.empty())
    {
        memberSyms = request.symbolTable.FindSymbols(scopedName);
    }
    if (memberSyms.empty())
    {
        const std::string enclosingNs = FindEnclosingNamespace(request.uri, request.position.line, request.symbolTable);
        if (!enclosingNs.empty())
        {
            utils::ForEachEnclosingScope(enclosingNs,
                                         [&](std::string_view scope)
                                         {
                                             if (!memberSyms.empty())
                                             {
                                                 return;
                                             }
                                             std::string fullQual = utils::JoinScopedName(scope, qualifier);
                                             memberSyms = request.symbolTable.FindMemberSymbols(fullQual, memberName);
                                             if (memberSyms.empty())
                                             {
                                                 memberSyms = request.symbolTable.FindSymbols(
                                                     utils::JoinScopedName(scope, scopedName));
                                             }
                                         });
        }
    }
    auto locs = ConvertSymbolsToLocations(memberSyms, request);
    return locs.empty() ? std::nullopt : std::make_optional(std::move(locs));
}

/**
 * @brief Attempts to resolve 'this' keyword to its enclosing class declaration.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] request Definition request context.
 * @return Enclosing class location or std::nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveThisKeyword(std::string_view nodeText,
                                                                       const DefinitionRequest& request)
{
    if (nodeText != "this")
    {
        return std::nullopt;
    }

    std::string enclosingClass = request.symbolTable.FindEnclosingClassName(request.uri, request.position.line);
    if (enclosingClass.empty())
    {
        return std::nullopt;
    }

    auto syms = request.symbolTable.FindSymbols(enclosingClass);
    auto locs = ConvertSymbolsToLocations(syms, request);
    return locs.empty() ? std::nullopt : std::make_optional(std::move(locs));
}

/**
 * @brief Resolves base class delegate property (e.g. BaseClass) to base class definition.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] request Definition request context.
 * @return Resolved base class location or std::nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveDelegateProperty(std::string_view nodeText,
                                                                            const DefinitionRequest& request)
{
    if (!analysis::IsBaseClassDelegateProperty(nodeText, request.symbolTable, request.engineRules))
    {
        return std::nullopt;
    }
    std::string enclosingClass = request.symbolTable.FindEnclosingClassName(request.uri, request.position.line);
    if (enclosingClass.empty())
    {
        return std::nullopt;
    }
    std::string baseClass = analysis::ResolveBaseClass(enclosingClass, request.symbolTable);
    if (!baseClass.empty())
    {
        auto baseSyms = request.symbolTable.FindSymbols(baseClass);
        auto locs = ConvertSymbolsToLocations(baseSyms, request);
        if (!locs.empty())
        {
            return locs;
        }
    }
    return std::nullopt;
}

/**
 * @brief Resolves virtual mixin document identifiers to host class symbols.
 * @param[in] nodeText Symbol name under cursor.
 * @param[in] request Definition request context.
 * @return Resolved host symbol locations or std::nullopt.
 */
static std::optional<std::vector<lsp::Location>> TryResolveVirtualMixin(std::string_view nodeText,
                                                                        const DefinitionRequest& request)
{
    auto vCtx = analysis::ResolveVirtualMixinContext(request.uri, request.symbolTable);
    if (!vCtx.isVirtual)
    {
        return std::nullopt;
    }
    auto memberSyms = request.symbolTable.FindMemberSymbols(vCtx.hostClass, nodeText);
    if (memberSyms.empty())
    {
        auto hierarchy = analysis::GetInheritedTypeHierarchy(vCtx.hostClass, request.symbolTable);
        for (const auto& base : hierarchy)
        {
            memberSyms = request.symbolTable.FindMemberSymbols(base, nodeText);
            if (!memberSyms.empty())
            {
                break;
            }
        }
    }
    if (!memberSyms.empty())
    {
        auto locs = ConvertSymbolsToLocations(memberSyms, request);
        if (!locs.empty())
        {
            return locs;
        }
    }
    if (nodeText == "this" || analysis::IsBaseClassDelegateProperty(nodeText, request.symbolTable, request.engineRules))
    {
        auto hostSyms = request.symbolTable.FindSymbols(vCtx.hostClass);
        auto locs = ConvertSymbolsToLocations(hostSyms, request);
        if (!locs.empty())
        {
            return locs;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    if (auto includeLoc = TryResolveIncludeDirective(request))
    {
        return includeLoc;
    }

    std::string_view nodeText = common::FindIdentifierAtPosition(request.sourceCode, request.position);
    if (nodeText.empty() || !analysis::IsValidIdentifier(nodeText))
    {
        return std::nullopt;
    }

    if (auto vmLocs = TryResolveVirtualMixin(nodeText, request))
    {
        return vmLocs;
    }

    if (auto thisLocs = TryResolveThisKeyword(nodeText, request))
    {
        return thisLocs;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* innerScope =
        rootScope ? FindInnermostScope(rootScope.get(), request.position.line, request.position.character) : nullptr;

    if (auto overLocs = TryResolveOverriddenMethod(nodeText, innerScope, request))
    {
        return overLocs;
    }

    if (auto declLocs = TryResolveLocalDeclaration(nodeText, innerScope, request))
    {
        return declLocs;
    }

    auto resolveReq = analysis::MakeTargetRequest(
        request.uri, request.sourceCode, analysis::TargetPosition{request.position.line, request.position.character},
        request.symbolTable, request.scopeIndex);
    resolveReq.useCallBindings = true;
    resolveReq.callBindingRevision = request.callBindingRevision;
    if (auto target = analysis::ResolveTargetSymbol(resolveReq))
    {
        auto locs = ResolveTargetLocations(*target, request);
        if (!locs.empty())
        {
            return locs;
        }
    }

    if (auto delLocs = TryResolveDelegateProperty(nodeText, request))
    {
        return delLocs;
    }

    if (auto scopedLocs = TryResolveScopedIdentifier(request))
    {
        return scopedLocs;
    }

    const std::string enclosingNs = FindEnclosingNamespace(request.uri, request.position.line, request.symbolTable);
    if (!enclosingNs.empty())
    {
        std::vector<analysis::Symbol> nsSyms;
        utils::ForEachEnclosingScope(enclosingNs,
                                     [&](std::string_view scope)
                                     {
                                         if (!nsSyms.empty())
                                         {
                                             return;
                                         }
                                         std::string scoped = utils::JoinScopedName(scope, nodeText);
                                         nsSyms = request.symbolTable.FindSymbols(scoped);
                                     });
        if (!nsSyms.empty())
        {
            DisambiguateCandidateContainers(nsSyms, request.sourceCode, request.position, request.symbolTable);
            auto locs = ConvertSymbolsToLocations(nsSyms, request);
            if (!locs.empty())
            {
                return locs;
            }
        }
    }

    auto fallbackSyms = request.symbolTable.FindSymbols(nodeText);
    DisambiguateCandidateContainers(fallbackSyms, request.sourceCode, request.position, request.symbolTable);
    auto locs = ConvertSymbolsToLocations(fallbackSyms, request);
    return locs.empty() ? std::nullopt : std::make_optional(std::move(locs));
}

std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest& request)
{
    if (request.sourceCode.empty())
    {
        return std::nullopt;
    }

    std::string_view nodeText = common::FindIdentifierAtPosition(request.sourceCode, request.position);
    if (nodeText.empty() || !analysis::IsValidIdentifier(nodeText))
    {
        return std::nullopt;
    }

    std::string typeNameToFind = ResolveTargetTypeName(nodeText, request);
    if (typeNameToFind.empty() || analysis::IsPrimitiveTypeName(typeNameToFind))
    {
        return std::nullopt;
    }

    const std::string enclosingNs = FindEnclosingNamespace(request.uri, request.position.line, request.symbolTable);
    std::vector<analysis::Symbol> typeSymbols;
    if (!enclosingNs.empty())
    {
        std::string scopedType = utils::JoinScopedName(enclosingNs, typeNameToFind);
        typeSymbols = request.symbolTable.FindSymbols(scopedType);
    }
    if (typeSymbols.empty())
    {
        typeSymbols = request.symbolTable.FindSymbols(typeNameToFind);
    }
    if (typeSymbols.empty())
    {
        typeSymbols = request.symbolTable.FindTypeSymbolsByShortName(typeNameToFind);
    }

    if (typeSymbols.size() > 1 && !enclosingNs.empty())
    {
        std::vector<analysis::Symbol> filtered;
        for (const auto& sym : typeSymbols)
        {
            if (analysis::IsSameOrNestedNamespace(sym.containerName, enclosingNs))
            {
                filtered.push_back(sym);
            }
        }
        if (!filtered.empty())
        {
            typeSymbols = std::move(filtered);
        }
    }

    std::vector<analysis::Symbol> targetSymbols;
    for (const auto& sym : typeSymbols)
    {
        if (IsTypeDefType(sym.type))
        {
            targetSymbols.push_back(sym);
        }
    }

    auto locs = ConvertSymbolsToLocations(targetSymbols, request);
    if (!locs.empty())
    {
        return locs;
    }

    return std::nullopt;
}

} // namespace angel_lsp::features
