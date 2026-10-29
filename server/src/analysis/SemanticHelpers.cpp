#include "analysis/SemanticHelpers.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticContext.h"
#include "analysis/NodeIndex.h"
#include "analysis/OverloadResolver.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "analysis/rules/RuleIndex.h"
#include "parser/Keywords.h"
#include "utils/Constants.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include "parser/GrammarNames.h"
#include "parser/QueryRegistry.h"
#include "utils/LspLogger.h"
#include <optional>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::analysis
{
std::string CleanExpressionType(std::string_view typeName)
{
    while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
    {
        typeName.remove_prefix(1);
    }
    while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t'))
    {
        typeName.remove_suffix(1);
    }
    if (typeName.starts_with("const "))
    {
        typeName.remove_prefix(6);
    }
    while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
    {
        typeName.remove_prefix(1);
    }
    while (!typeName.empty() && (typeName.back() == '&' || typeName.back() == ' ' || typeName.back() == '\t'))
    {
        typeName.remove_suffix(1);
    }
    return std::string(typeName);
}

std::string_view GetNodeTextView(TSNode node, std::string_view sourceCode) noexcept
{
    if (ts_node_is_null(node) || sourceCode.empty())
    {
        return {};
    }
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start < sourceCode.size() && end <= sourceCode.size() && start <= end)
    {
        return sourceCode.substr(start, end - start);
    }
    return {};
}

bool IsReservedKeyword(const std::string& name)
{
    // The list this used to hold was measured against the compiler word by word and came back
    // exactly right - 53, no additions, no removals. It moved to parser/Keywords.h so the
    // formatter and completion could stop keeping their own, differing, copies.
    return parser::keywords::IsReserved(name);
}

bool IsKeyword(std::string_view word) noexcept
{
    return parser::keywords::IsKeyword(word);
}

bool IsPrimitiveTypeName(std::string_view name)
{
    return IsCorePrimitive(name);
}

InitializerItemKind ClassifyInitializerItem(std::string_view item)
{
    if (item.empty())
    {
        return InitializerItemKind::NumericOrExpression;
    }

    if (item.starts_with("\"") || item.starts_with("'"))
    {
        return InitializerItemKind::StringLiteral;
    }

    if (item == "true" || item == "false")
    {
        return InitializerItemKind::BooleanLiteral;
    }

    if (item == "null")
    {
        return InitializerItemKind::NullLiteral;
    }

    if (item.starts_with("{"))
    {
        return InitializerItemKind::NestedInitializer;
    }

    return InitializerItemKind::NumericOrExpression;
}

bool IsFromPredefinedStub(const Symbol& sym, const DiagnosticContext& ctx)
{
    return utils::IsPredefinedFile(sym.fileUri, ctx.request.predefinedFileExtension);
}

bool IsDestructorSymbol(const Symbol& sym)
{
    if (sym.type != SymbolType::Function)
    {
        return false;
    }
    return sym.name.starts_with("~");
}

bool IsDestructorDeclaration(const Symbol& sym, std::string_view source)
{
    if (sym.type != SymbolType::Function)
    {
        return false;
    }
    if (sym.name.starts_with('~'))
    {
        return true;
    }
    if (source.empty())
    {
        return false;
    }

    // Walk to the start of the declaration's line, then to the identifier.
    size_t offset = 0;
    for (uint32_t line = 0; line < sym.selectionRange.startLine; ++line)
    {
        offset = source.find('\n', offset);
        if (offset == std::string_view::npos)
        {
            return false;
        }
        ++offset;
    }

    offset += sym.selectionRange.startCharacter;
    if (offset == 0 || offset > source.size())
    {
        return false;
    }

    size_t back = offset;
    while (back > 0 && (source[back - 1] == ' ' || source[back - 1] == '\t'))
    {
        --back;
    }
    return back > 0 && source[back - 1] == '~';
}

bool IsDestructorDeclaration(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.fileUri != ctx.request.fileUri)
    {
        return false;
    }
    return IsDestructorDeclaration(sym, ctx.request.sourceCode);
}

namespace
{
bool HasListConstructorParam(const ParameterInformation& p)
{
    const std::string norm = StripTypeDecorations(p.baseTypeName.empty() ? p.typeName : p.baseTypeName);
    const bool isInt = (norm == "int" || norm == "uint" || norm == "int64" || norm == "uint64");
    return isInt && (p.isReference || p.modifier == ParameterModifier::In || p.modifier == ParameterModifier::InOut ||
                     p.typeName.find("&in") != std::string::npos || p.rawText.find("&in") != std::string::npos);
}
} // namespace

bool IsListConstructorSignature(const FunctionSignature& fn)
{
    if (fn.parameters.size() == 1)
    {
        return HasListConstructorParam(fn.parameters[0]);
    }
    if (fn.parameters.size() == 2)
    {
        return HasListConstructorParam(fn.parameters[0]) && HasListConstructorParam(fn.parameters[1]);
    }
    return false;
}

bool IsListConstructor(const Symbol& sym)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return false;
    }
    return IsListConstructorSignature(sym.GetFunction());
}

std::string_view FirstAttributeName(const SymbolModifiers& modifiers)
{
    // Grammar order: choice("override", "final", "explicit", "property", "delete").
    if (modifiers.isOverride)
    {
        return "override";
    }
    if (modifiers.isFinal)
    {
        return "final";
    }
    if (modifiers.isExplicit)
    {
        return "explicit";
    }
    if (modifiers.isProperty)
    {
        return "property";
    }
    if (modifiers.isDelete)
    {
        return "delete";
    }
    return {};
}

bool NamesAFunctionNotAType(std::string_view name, const SymbolTable& table)
{
    if (name.empty())
    {
        return false;
    }

    const auto symsPtr = table.FindSymbolsPtr(name);
    if (!symsPtr)
    {
        // Resolves to nothing. That is an unresolved type, assumed engine-registered, and not
        // this rule's business - see the header.
        return false;
    }

    bool isFunction = false;
    for (const auto& sym : *symsPtr)
    {
        switch (sym.type)
        {
        case SymbolType::Function:
            // A method is reached through its container and cannot be written bare in a type
            // position, so it says nothing about what the user meant here.
            if (sym.containerName.empty())
            {
                isFunction = true;
            }
            break;

        // Anything that CAN stand in a type position settles it: the name is a type, whatever
        // else it also is. Funcdef in particular - a funcdef and a function may share a name,
        // and then `Foo@` is already valid.
        case SymbolType::Class:
        case SymbolType::Interface:
        case SymbolType::Enum:
        case SymbolType::Typedef:
        case SymbolType::Funcdef:
            return false;

        default:
            break;
        }
    }

    return isFunction;
}

std::vector<Symbol> LookupTypeCandidateSymbols(const std::string& name, const SymbolTable& symbolTable)
{
    auto syms = symbolTable.FindSymbols(name);
    if (syms.empty())
    {
        const std::string shortName = std::string(LastScopeSegment(name));
        if (shortName != name)
        {
            syms = symbolTable.FindSymbols(shortName);
        }
    }
    if (syms.empty())
    {
        auto typeSyms = symbolTable.FindTypeSymbolsByShortName(LastScopeSegment(name));
        if (!typeSyms.empty())
        {
            syms = std::move(typeSyms);
        }
    }
    return syms;
}

bool AreAllSymbolsPureTypes(const std::vector<Symbol>& syms)
{
    if (syms.empty())
    {
        return false;
    }
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Variable || s.type == SymbolType::Function || s.type == SymbolType::Property)
        {
            return false;
        }
    }
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Class || s.type == SymbolType::Interface || s.type == SymbolType::Enum ||
            s.type == SymbolType::Typedef || s.type == SymbolType::Funcdef)
        {
            return true;
        }
    }
    return false;
}

static TSNode UnwrapParens(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression &&
           ts_node_named_child_count(node) > 0)
    {
        node = ts_node_named_child(node, 0);
    }
    return node;
}

static std::string ExtractTrimmedIdentifier(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return {};
    }
    std::string name = GetNodeText(node, sourceCode);
    while (!name.empty() && isspace(static_cast<unsigned char>(name.front())))
    {
        name.erase(name.begin());
    }
    while (!name.empty() && isspace(static_cast<unsigned char>(name.back())))
    {
        name.pop_back();
    }
    return name;
}

std::optional<std::string> IsBareDataType(TSNode node, const Scope* scope, const SymbolTable& symbolTable,
                                          std::string_view sourceCode)
{
    node = UnwrapParens(node);
    std::string name = ExtractTrimmedIdentifier(node, sourceCode);
    if (name.empty())
    {
        return std::nullopt;
    }

    if (IsPrimitiveTypeName(name))
    {
        return name;
    }

    if (scope)
    {
        if (const LocalDefinition* def = ResolveInScopeAt(scope, name, ts_node_start_byte(node)))
        {
            return def->kind == LocalDefinitionKind::Type ? std::optional<std::string>(name) : std::nullopt;
        }
    }

    const auto syms = LookupTypeCandidateSymbols(name, symbolTable);
    if (AreAllSymbolsPureTypes(syms))
    {
        return name;
    }

    return std::nullopt;
}

static bool IsMemberMethodSymbol(TSNode node, const Scope* scope, const SymbolTable& symbolTable,
                                 std::string_view sourceCode)
{
    TSNode objNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return false;
    }
    const std::string memName = NodeText(memNode, sourceCode);
    const std::string objType = ResolveExpressionType(objNode, ExpressionTypeContext(scope, symbolTable, sourceCode));
    const std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty() || memName.empty())
    {
        return false;
    }
    for (const auto& cls : GetInheritedTypeHierarchy(cleanObj, symbolTable))
    {
        if (const auto syms = symbolTable.FindMemberSymbolPtr(cls, memName))
        {
            bool hasFunc = false;
            bool hasVarOrProp = false;
            for (const auto& s : *syms)
            {
                if (s.type == SymbolType::Function)
                {
                    hasFunc = true;
                }
                else if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
                {
                    hasVarOrProp = true;
                }
            }
            if (hasFunc && !hasVarOrProp)
            {
                return true;
            }
        }
    }
    return false;
}

static bool HasFunctionSymbolOnly(const std::vector<Symbol>& syms)
{
    bool hasFunc = false;
    bool hasVarOrProp = false;
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Function)
        {
            hasFunc = true;
        }
        else if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
        {
            hasVarOrProp = true;
        }
    }
    return hasFunc && !hasVarOrProp;
}

static bool IsLocalVariableOrFieldInScope(const Scope* scope, std::string_view nodeType, const std::string& name,
                                          uint32_t refByte = UINT32_MAX)
{
    const bool isUnqualified = (nodeType == parser::nodes::Identifier ||
                                (nodeType == parser::nodes::ScopedIdentifier && !utils::ContainsScopeSeparator(name)));
    if (isUnqualified && scope)
    {
        if (const auto def = ResolveInScopeAt(scope, name, refByte))
        {
            return def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter ||
                   def->kind == LocalDefinitionKind::Field;
        }
    }
    return false;
}

static bool HasGlobalFunctionSymbol(std::string_view name, const SymbolTable& symbolTable)
{
    if (const auto found = symbolTable.FindSymbolsPtr(name))
    {
        return HasFunctionSymbolOnly(*found);
    }
    const std::string_view lastSeg = LastScopeSegment(name);
    if (lastSeg != name)
    {
        if (const auto found = symbolTable.FindSymbolsPtr(lastSeg))
        {
            return HasFunctionSymbolOnly(*found);
        }
    }
    return false;
}

static bool IsIdentifierFunctionSymbol(TSNode node, const Scope* scope, const SymbolTable& symbolTable,
                                       std::string_view sourceCode)
{
    std::string name = GetNodeText(node, sourceCode);
    if (name.empty())
    {
        return false;
    }
    if (IsLocalVariableOrFieldInScope(scope, parser::GetNodeType(node), name, ts_node_start_byte(node)))
    {
        return false;
    }
    return HasGlobalFunctionSymbol(name, symbolTable);
}

bool IsFunctionReference(TSNode node, const Scope* scope, const SymbolTable& symbolTable, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression &&
           ts_node_named_child_count(node) > 0)
    {
        node = ts_node_named_child(node, 0);
    }
    if (ts_node_is_null(node))
    {
        return false;
    }
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression)
    {
        return false;
    }
    if (sym == gs.symMemberExpression)
    {
        return IsMemberMethodSymbol(node, scope, symbolTable, sourceCode);
    }
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return IsIdentifierFunctionSymbol(node, scope, symbolTable, sourceCode);
    }
    return false;
}

bool IsMixinClass(std::string_view baseTypeName, const SymbolTable& table)
{
    if (baseTypeName.empty())
    {
        return false;
    }

    std::string searchName(baseTypeName);
    auto symsPtr = table.FindSymbolsPtr(searchName);
    if (!symsPtr)
    {
        return false;
    }

    for (const auto& sym : *symsPtr)
    {
        if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
        {
            return true;
        }
    }
    return false;
}

NonInstantiableKind ClassifyNonInstantiable(std::string_view baseTypeName, const SymbolTable& table)
{
    if (baseTypeName.empty())
    {
        return NonInstantiableKind::None;
    }

    auto symsPtr = table.FindSymbolsPtr(baseTypeName);
    if (!symsPtr)
    {
        return NonInstantiableKind::None;
    }

    for (const auto& sym : *symsPtr)
    {
        if (sym.type == SymbolType::Interface)
        {
            return NonInstantiableKind::Interface;
        }
        if (sym.type == SymbolType::Class)
        {
            if (sym.GetClass().modifiers.isMixin)
            {
                return NonInstantiableKind::Mixin;
            }
            if (sym.GetClass().modifiers.isAbstract)
            {
                return NonInstantiableKind::Abstract;
            }
        }
    }
    return NonInstantiableKind::None;
}

/**
 * @brief Matches qualified types by short name suffix against the rule index.
 * @param[in] clean Type name without root qualifier.
 * @param[in] index Precomputed rule index.
 * @return True if clean matches a known qualified type or suffix.
 */
static bool MatchesQualifiedTypeSuffix(std::string_view clean, const rules::RuleIndex* index)
{
    if (!index)
    {
        return false;
    }
    const std::string shortName = std::string(LastScopeSegment(clean));
    auto it = index->qualifiedTypesByShortName.find(shortName);
    if (it == index->qualifiedTypesByShortName.end())
    {
        return false;
    }
    const bool isPartiallyQualified = utils::ContainsScopeSeparator(clean);
    for (const auto& qType : it->second)
    {
        if (qType == clean || (isPartiallyQualified && utils::EndsWithScopedMember(qType, clean)))
        {
            return true;
        }
    }
    return false;
}

bool MatchesParentNamespaceType(std::string_view clean, const SymbolTable& table, const rules::RuleIndex* ruleIndex)
{
    const auto [parent, member] = utils::SplitScopedName(clean);
    if (parent.empty())
    {
        return false;
    }

    std::string_view prefix = parent;
    while (!prefix.empty())
    {
        prefix = utils::ParentScopeView(prefix);
        const std::string candidate = utils::JoinScopedName(prefix, member);
        if (table.HasSymbol(candidate) || table.HasSymbolAnywhere(candidate) ||
            MatchesQualifiedTypeSuffix(candidate, ruleIndex))
        {
            return true;
        }
    }
    return false;
}

bool IsReachableViaUsingDirectives(std::string_view clean, const DiagnosticContext& ctx)
{
    const auto& usings = GetUsingNamespaces(ctx.request);
    for (const auto& ns : usings)
    {
        std::string q = utils::JoinScopedName(ns, clean);
        if (ctx.request.symbolTable.HasSymbol(q))
        {
            return true;
        }
    }
    return false;
}

bool IsReachableInContainerScope(std::string_view clean, std::string_view containerScope, const SymbolTable& table)
{
    return table.IsReachableInContainerScope(clean, containerScope);
}

std::string EnclosingNamespaceForSymbol(const Symbol& sym, const SymbolTable& table)
{
    if (sym.containerKind == ContainerKind::Namespace)
    {
        return sym.containerName;
    }
    if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
    {
        if (auto classSyms = table.FindSymbolsPtr(sym.containerName))
        {
            for (const auto& cs : *classSyms)
            {
                if (cs.type == SymbolType::Class || cs.type == SymbolType::Interface)
                {
                    if (cs.containerKind == ContainerKind::Namespace)
                    {
                        return cs.containerName;
                    }
                    break;
                }
            }
        }
    }
    return "";
}

bool IsTemplateParameterOfContainer(std::string_view typeName, const Symbol& sym, const SymbolTable& table)
{
    if (typeName.empty() || sym.containerName.empty())
    {
        return false;
    }
    const std::string_view cleanContainer = CleanBaseTypeView(sym.containerName);
    const auto containerSyms = table.FindSymbolsPtr(cleanContainer.empty() ? sym.containerName : cleanContainer);
    if (!containerSyms)
    {
        return false;
    }
    for (const auto& owner : *containerSyms)
    {
        if (owner.type == SymbolType::Class && std::holds_alternative<ClassSignature>(owner.signature))
        {
            const auto& sig = owner.GetClass();
            if (std::find(sig.templateParams.begin(), sig.templateParams.end(), typeName) != sig.templateParams.end())
            {
                return true;
            }
        }
    }
    return false;
}

bool IsKnownType(std::string_view baseName, const DiagnosticContext& ctx, std::string_view containerScope)
{
    if (ctx.logger && ctx.logger->IsTraceEnabled())
    {
        ctx.logger->LogTrace(fmt::format("[SemanticHelpers] IsKnownType: checking '{}'", baseName));
    }
    if (baseName.empty() || IsCorePrimitive(baseName))
        return true;
    if (!ctx.request.GetStringTypeName().empty() && baseName == ctx.request.GetStringTypeName())
        return true;
    if (!ctx.request.GetArrayTypeName().empty() && baseName == ctx.request.GetArrayTypeName())
        return true;
    if (ctx.request.IsRegisteredSymbol(baseName))
        return true;

    std::string_view clean = utils::StripGlobalPrefix(baseName);
    if (ctx.request.symbolTable.HasSymbol(clean))
        return true;

    if (IsReachableInContainerScope(clean, containerScope, ctx.request.symbolTable))
        return true;

    const auto* ruleIndex = ctx.request.symbolTable.GetRuleIndex().get();
    if (MatchesQualifiedTypeSuffix(clean, ruleIndex))
        return true;

    if (MatchesParentNamespaceType(clean, ctx.request.symbolTable, ruleIndex))
        return true;

    return IsReachableViaUsingDirectives(clean, ctx);
}

std::string_view LastScopeSegment(std::string_view name) noexcept
{
    const size_t bracketPos = name.find('<');
    const std::string_view prefix = (bracketPos != std::string_view::npos) ? name.substr(0, bracketPos) : name;
    return utils::LastScopeSegment(prefix);
}

std::string_view ParentScope(std::string_view name) noexcept
{
    return utils::ParentScopeView(name);
}

bool ResolvesToEnum(std::string_view typeName, const SymbolTable& table)
{
    if (typeName.empty())
    {
        return false;
    }
    std::string bare = std::string(LastScopeSegment(typeName));
    for (const auto& candidate : {std::string(typeName), bare})
    {
        const auto bucket = table.FindSymbolsPtr(candidate);
        if (bucket)
        {
            for (const auto& sym : *bucket)
            {
                if (sym.type == SymbolType::Enum)
                {
                    return true;
                }
            }
        }
        if (bare == typeName)
        {
            break;
        }
    }
    return false;
}

std::string SubstituteTypeParam(std::string_view typeStr, std::string_view paramName, std::string_view concreteType)
{
    if (typeStr.empty() || paramName.empty())
    {
        return std::string(typeStr);
    }
    std::string result;
    size_t i = 0;
    while (i < typeStr.size())
    {
        if (typeStr.substr(i).starts_with(paramName))
        {
            bool leftBoundary =
                (i == 0) || (!isalnum(static_cast<unsigned char>(typeStr[i - 1])) && typeStr[i - 1] != '_');
            size_t nextIdx = i + paramName.size();
            bool rightBoundary = (nextIdx >= typeStr.size()) ||
                                 (!isalnum(static_cast<unsigned char>(typeStr[nextIdx])) && typeStr[nextIdx] != '_');
            if (leftBoundary && rightBoundary)
            {
                result.append(concreteType);
                i = nextIdx;
                continue;
            }
        }
        result.push_back(typeStr[i]);
        ++i;
    }
    return result;
}

bool IsTypeSymbolList(const std::vector<Symbol>& syms) noexcept
{
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Class || s.type == SymbolType::Interface || s.type == SymbolType::Enum ||
            s.type == SymbolType::Typedef || s.type == SymbolType::Funcdef)
        {
            return true;
        }
    }
    return false;
}

std::optional<std::string> TryQualifyIdentifier(std::string_view ident, std::string_view containerScope,
                                                const SymbolTable& table)
{
    if (parser::primitives::IsPrimitive(ident))
    {
        return std::nullopt;
    }
    std::string candidate;
    candidate.reserve(containerScope.size() + 2 + ident.size());
    candidate.append(containerScope);
    candidate.append("::");
    candidate.append(ident);
    if (const auto syms = table.FindSymbolsPtr(candidate))
    {
        if (IsTypeSymbolList(*syms))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

bool IsColonQualified(std::string_view s, size_t start, size_t end) noexcept
{
    const bool preceded = (start >= 2 && s[start - 1] == ':' && s[start - 2] == ':');
    const bool followed = (end + 1 < s.size() && s[end] == ':' && s[end + 1] == ':');
    return preceded || followed;
}

std::string QualifyTypeInContainer(std::string_view typeStr, std::string_view containerScope, const SymbolTable& table)
{
    if (typeStr.empty() || containerScope.empty())
    {
        return std::string(typeStr);
    }
    std::string result;
    result.reserve(typeStr.size() + containerScope.size() + 2);
    size_t i = 0;
    while (i < typeStr.size())
    {
        const char c = typeStr[i];
        if (!isalpha(static_cast<unsigned char>(c)) && c != '_')
        {
            result.push_back(c);
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < typeStr.size() && (isalnum(static_cast<unsigned char>(typeStr[i])) || typeStr[i] == '_'))
        {
            ++i;
        }
        const std::string_view ident = typeStr.substr(start, i - start);
        if (!IsColonQualified(typeStr, start, i))
        {
            if (auto qualified = TryQualifyIdentifier(ident, containerScope, table))
            {
                result.append(*qualified);
                continue;
            }
        }
        result.append(ident);
    }
    return result;
}

TemplateBinding BindTemplateArguments(const std::string& writtenType, const SymbolTable& table)
{
    TemplateBinding binding;
    const auto decomp = DecomposeTemplateType(writtenType);

    const auto declarations = table.FindSymbolsPtr(LastScopeSegment(decomp.containerName));
    if (!declarations)
    {
        return binding;
    }

    for (const auto& declaration : *declarations)
    {
        if (declaration.type != SymbolType::Class || !std::holds_alternative<ClassSignature>(declaration.signature))
        {
            continue;
        }
        const auto& cls = std::get<ClassSignature>(declaration.signature);
        if (!cls.isTemplate || cls.templateParams.empty())
        {
            continue;
        }

        binding.isTemplate = true;
        binding.parameters = cls.templateParams;
        break;
    }

    if (!binding.isTemplate || !decomp.isTemplate)
    {
        return binding;
    }

    binding.arguments = SplitTemplateArguments(decomp.innerArguments);

    if (!binding.arguments.empty() && binding.arguments.back().empty())
    {
        binding.arguments.pop_back();
    }

    binding.usable = binding.arguments.size() == binding.parameters.size();
    return binding;
}

std::string SubstituteTemplateParameters(std::string_view typeStr, const TemplateBinding& binding)
{
    std::string result(typeStr);
    if (binding.usable)
    {
        for (size_t i = 0; i < binding.parameters.size(); ++i)
        {
            result = SubstituteTypeParam(result, binding.parameters[i], binding.arguments[i]);
        }
    }
    return result;
}

static std::vector<Symbol> CollectOpIndexCandidates(const std::vector<std::string>& hierarchy,
                                                    const SymbolTable& symbolTable)
{
    std::vector<Symbol> candidates;
    for (const auto& typeInHierarchy : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbolPtr(typeInHierarchy, "opIndex");
        if (!found)
        {
            continue;
        }
        for (const auto& sym : *found)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                if (sym.GetFunction().returnType != "void" && sym.GetFunction().parameters.size() == 1)
                {
                    candidates.push_back(sym);
                }
            }
        }
    }
    return candidates;
}

static std::string ApplyTemplateBindingToReturn(std::string ret, const std::string& canonical,
                                                const SymbolTable& symbolTable)
{
    auto binding = BindTemplateArguments(canonical, symbolTable);
    if (binding.usable)
    {
        return CleanExpressionType(SubstituteTemplateParameters(ret, binding));
    }
    auto tmpl = ParseTemplateType(canonical);
    if (tmpl.templateArgs.size() == 1)
    {
        ret = SubstituteTypeParam(ret, "T", tmpl.templateArgs[0]);
    }
    else if (tmpl.templateArgs.size() >= 2)
    {
        ret = SubstituteTypeParam(ret, "T", tmpl.templateArgs.back());
        ret = SubstituteTypeParam(ret, "V", tmpl.templateArgs[1]);
        ret = SubstituteTypeParam(ret, "K", tmpl.templateArgs[0]);
    }
    return CleanExpressionType(ret);
}

std::string FindOpIndexInHierarchy(const std::string& owner, const std::string& canonical,
                                   const SymbolTable& symbolTable, std::string_view indexArgType = "")
{
    auto hierarchy = GetInheritedTypeHierarchy(owner, symbolTable);
    auto candidates = CollectOpIndexCandidates(hierarchy, symbolTable);
    const Symbol* bestSym = nullptr;
    if (!candidates.empty())
    {
        if (!indexArgType.empty())
        {
            auto match = ResolveBestOverload(candidates, {std::string(indexArgType)}, symbolTable);
            bestSym = match.bestCandidate;
        }
        if (!bestSym)
        {
            bestSym = &candidates.front();
        }
    }
    if (bestSym)
    {
        return ApplyTemplateBindingToReturn(bestSym->GetFunction().returnType, canonical, symbolTable);
    }
    return "";
}

std::string ResolveIndexFallback(const std::string& canonical)
{
    auto tmpl = ParseTemplateType(canonical);
    if (!tmpl.templateArgs.empty())
    {
        return CleanExpressionType(tmpl.templateArgs.size() >= 2 ? tmpl.templateArgs[1] : tmpl.templateArgs[0]);
    }
    if (canonical.ends_with("[]"))
    {
        return CleanExpressionType(canonical.substr(0, canonical.size() - 2));
    }
    return "";
}

std::string ResolveSingleIndexStep(std::string_view current, const SymbolTable& symbolTable,
                                   std::string_view arrayTypeName, std::string_view indexArgType = "")
{
    const std::string canonical = CanonicalizeArrayType(current, arrayTypeName);
    std::string owner = MemberOwnerType(canonical, arrayTypeName);
    if (owner.empty())
    {
        owner = CleanExpressionType(canonical);
    }

    std::string indexedType = FindOpIndexInHierarchy(owner, canonical, symbolTable, indexArgType);
    if (!indexedType.empty())
    {
        return indexedType;
    }
    return ResolveIndexFallback(canonical);
}

std::string ResolveIndexedTypeWithArg(std::string_view typeName, const SymbolTable& symbolTable,
                                      std::string_view arrayTypeName, std::string_view indexArgType)
{
    return ResolveSingleIndexStep(typeName, symbolTable, arrayTypeName, indexArgType);
}

std::string ResolveIndexedType(std::string_view typeName, size_t indexCount, const SymbolTable& symbolTable,
                               std::string_view arrayTypeName)
{
    std::string current(typeName);
    for (size_t idx = 0; idx < indexCount && !current.empty(); ++idx)
    {
        std::string next = ResolveSingleIndexStep(current, symbolTable, arrayTypeName, "");
        if (next.empty())
        {
            break;
        }
        current = std::move(next);
    }
    return current;
}

void StripTemplateDecorations(std::string_view& typeName)
{
    TrimTypeWhitespace(typeName);
    while (!typeName.empty() &&
           (typeName.back() == ' ' || typeName.back() == '\t' || typeName.back() == '@' || typeName.back() == '&'))
    {
        typeName.remove_suffix(1);
    }
    StripLeadingConst(typeName);
    TrimTypeWhitespace(typeName);
}

TemplateTypeInfo ParseTemplateType(std::string_view typeName)
{
    TemplateTypeInfo info;
    StripTemplateDecorations(typeName);

    const size_t openBracket = typeName.find('<');
    if (openBracket == std::string_view::npos || !typeName.ends_with('>'))
    {
        info.containerName = std::string(typeName);
        return info;
    }

    info.containerName = std::string(typeName.substr(0, openBracket));
    const std::string_view inner = typeName.substr(openBracket + 1, typeName.size() - openBracket - 2);

    for (std::string& argument : SplitTemplateArguments(inner))
    {
        if (!argument.empty())
        {
            info.templateArgs.push_back(std::move(argument));
        }
    }

    return info;
}

/**
 * @brief Checks if a type name should be ignored when comparing template compatibility.
 * @param[in] s Type name to check.
 * @return True if the type name is empty, wildcard, or untyped.
 */
bool IsIgnoredTemplateType(std::string_view s)
{
    return s.empty() || s == "null" || s == "auto" || s == "void";
}

bool AreCompatibleTemplateContainers(std::string_view fromContainer, std::string_view toContainer) noexcept
{
    if (fromContainer == toContainer)
    {
        return true;
    }
    if (utils::ContainsScopeSeparator(fromContainer) && utils::ContainsScopeSeparator(toContainer))
    {
        return false;
    }
    return LastScopeSegment(fromContainer) == LastScopeSegment(toContainer);
}

bool AreCompatibleTemplateArgPair(std::string_view va, std::string_view vb)
{
    TrimTypeWhitespace(va);
    TrimTypeWhitespace(vb);
    if (va == vb)
    {
        return true;
    }
    const bool vaIsTmpl = (va.find('<') != std::string_view::npos && va.ends_with('>'));
    const bool vbIsTmpl = (vb.find('<') != std::string_view::npos && vb.ends_with('>'));
    if (vaIsTmpl && vbIsTmpl)
    {
        return !AreIncompatibleTemplateTypes(va, vb);
    }
    if (utils::ContainsScopeSeparator(va) && utils::ContainsScopeSeparator(vb))
    {
        return false;
    }
    return LastScopeSegment(va) == LastScopeSegment(vb);
}

bool AreCompatibleTemplateArguments(const std::vector<std::string>& fromArgs, const std::vector<std::string>& toArgs)
{
    if (fromArgs.size() != toArgs.size())
    {
        return false;
    }
    for (size_t i = 0; i < fromArgs.size(); ++i)
    {
        if (!AreCompatibleTemplateArgPair(fromArgs[i], toArgs[i]))
        {
            return false;
        }
    }
    return true;
}

bool AreIncompatibleTemplateTypes(std::string_view from, std::string_view to)
{
    StripTemplateDecorations(from);
    StripTemplateDecorations(to);
    if (IsIgnoredTemplateType(from) || IsIgnoredTemplateType(to))
    {
        return false;
    }
    const bool fromIsTmpl = (from.find('<') != std::string_view::npos && from.ends_with('>'));
    const bool toIsTmpl = (to.find('<') != std::string_view::npos && to.ends_with('>'));
    if (fromIsTmpl != toIsTmpl)
    {
        return true;
    }
    if (!fromIsTmpl)
    {
        return false;
    }
    const size_t fromOpen = from.find('<');
    const size_t toOpen = to.find('<');
    if (!AreCompatibleTemplateContainers(from.substr(0, fromOpen), to.substr(0, toOpen)))
    {
        return true;
    }
    const auto fromInner = from.substr(fromOpen + 1, from.size() - fromOpen - 2);
    const auto toInner = to.substr(toOpen + 1, to.size() - toOpen - 2);
    return !AreCompatibleTemplateArguments(SplitTemplateArguments(fromInner), SplitTemplateArguments(toInner));
}

std::string PropertyNameFromAccessor(const Symbol& sym, bool keywordRequired)
{
    if (!std::holds_alternative<FunctionSignature>(sym.signature))
        return {};

    if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
        return {};

    for (const std::string_view prefix : {std::string_view("get_"), std::string_view("set_")})
    {
        if (sym.name.size() > prefix.size() && sym.name.compare(0, prefix.size(), prefix) == 0)
            return sym.name.substr(prefix.size());
    }

    return {};
}

std::vector<Symbol> FindPropertyAccessors(const std::string& typeName, const std::string& propertyName,
                                          const SymbolTable& symbolTable, bool keywordRequired)
{
    std::vector<Symbol> accessors;
    if (typeName.empty() || propertyName.empty())
        return accessors;

    const auto hierarchy = GetInheritedTypeHierarchy(typeName, symbolTable);
    for (const std::string_view prefix : {std::string_view("get_"), std::string_view("set_")})
    {
        const std::string accessorName = std::string(prefix) + propertyName;
        for (const auto& owner : hierarchy)
        {
            bool foundForOwner = false;
            if (const auto syms = symbolTable.FindMemberSymbolPtr(owner, accessorName))
            {
                for (const auto& sym : *syms)
                {
                    if (!std::holds_alternative<FunctionSignature>(sym.signature))
                        continue;
                    if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
                        continue;

                    accessors.push_back(sym);
                    foundForOwner = true;
                }
            }

            // A derived class overriding this accessor answers for it; the base's copy would only
            // be the same property said twice.
            if (foundForOwner)
                break;
        }
    }

    return accessors;
}

std::vector<Symbol> FindGlobalPropertyAccessors(const std::string& propertyName, const SymbolTable& symbolTable,
                                                bool keywordRequired)
{
    std::vector<Symbol> accessors;
    if (propertyName.empty())
    {
        return accessors;
    }

    char buf[128];
    for (const std::string_view prefix : {std::string_view("get_"), std::string_view("set_")})
    {
        if (prefix.size() + propertyName.size() >= sizeof(buf))
        {
            continue;
        }
        memcpy(buf, prefix.data(), prefix.size());
        memcpy(buf + prefix.size(), propertyName.data(), propertyName.size());
        const std::string_view key(buf, prefix.size() + propertyName.size());

        const auto syms = symbolTable.FindSymbolsPtr(key);
        if (!syms)
        {
            continue;
        }

        for (const auto& sym : *syms)
        {
            if (sym.type != SymbolType::Function || !sym.containerName.empty())
            {
                continue;
            }
            if (!std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }
            if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
            {
                continue;
            }

            accessors.push_back(sym);
        }
    }

    return accessors;
}

std::string PropertyTypeFromAccessors(const std::vector<Symbol>& accessors)
{
    for (const auto& sym : accessors)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
            continue;

        const FunctionSignature& func = sym.GetFunction();
        if (sym.name.compare(0, 4, "get_") == 0)
            return func.returnType;
    }

    for (const auto& sym : accessors)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
            continue;

        const FunctionSignature& func = sym.GetFunction();
        if (sym.name.compare(0, 4, "set_") == 0 && !func.parameters.empty())
            return func.parameters.front().typeName;
    }

    return {};
}

struct HierarchyClassifiedBase
{
    std::string name;
    bool isMixin = false;
};

std::vector<Symbol> FindHierarchyTypeSymbols(const std::string& curType, const SymbolTable& symbolTable)
{
    const auto symbols = symbolTable.FindSymbolsPtr(curType);
    if (symbols && !symbols->empty())
    {
        const bool hasType = std::any_of(symbols->begin(), symbols->end(), [](const Symbol& s)
                                         { return s.type == SymbolType::Class || s.type == SymbolType::Interface; });
        if (hasType)
        {
            return *symbols;
        }
    }
    if (!utils::ContainsScopeSeparator(curType))
    {
        return symbolTable.FindTypeSymbolsByShortName(curType);
    }
    return {};
}

void EnqueueClassBases(const Symbol& sym, const SymbolTable& symbolTable,
                       ankerl::unordered_dense::set<std::string>& visited, std::vector<std::string>& queue)
{
    const auto& cls = sym.GetClass();
    std::vector<HierarchyClassifiedBase> bases;
    bases.reserve(cls.bases.size());

    for (const auto& base : cls.bases)
    {
        std::string cleanBase = CleanBaseType(base);
        if (!cleanBase.empty())
        {
            const bool isMixin = IsMixinClass(cleanBase, symbolTable);
            bases.push_back(HierarchyClassifiedBase{std::move(cleanBase), isMixin});
        }
    }

    for (const bool wantMixins : {true, false})
    {
        for (const auto& base : bases)
        {
            if (base.isMixin == wantMixins && visited.insert(base.name).second)
            {
                queue.push_back(base.name);
            }
        }
    }
}

void EnqueueInterfaceBases(const Symbol& sym, ankerl::unordered_dense::set<std::string>& visited,
                           std::vector<std::string>& queue)
{
    const auto& iface = sym.GetInterface();
    for (const auto& base : iface.inheritedInterfaces)
    {
        std::string cleanBase = CleanBaseType(base);
        if (!cleanBase.empty() && visited.insert(cleanBase).second)
        {
            queue.push_back(cleanBase);
        }
    }
}

std::vector<std::string> GetInheritedTypeHierarchy(const std::string& className, const SymbolTable& symbolTable,
                                                   ExpressionTypeCache* cache)
{
    std::vector<std::string> hierarchy;
    const std::string rootType = CleanBaseType(className);
    if (rootType.empty())
    {
        return hierarchy;
    }

    if (parser::primitives::IsPrimitive(rootType))
    {
        return {rootType};
    }

    if (cache)
    {
        auto it = cache->hierarchies.find(rootType);
        if (it != cache->hierarchies.end())
        {
            return it->second;
        }
    }

    ankerl::unordered_dense::set<std::string> visited;
    std::vector<std::string> queue;

    visited.insert(rootType);
    queue.push_back(rootType);

    size_t head = 0;
    while (head < queue.size())
    {
        const std::string curType = queue[head++];
        hierarchy.push_back(curType);

        const auto symbolsToIterate = FindHierarchyTypeSymbols(curType, symbolTable);
        for (const auto& sym : symbolsToIterate)
        {
            if (sym.type == SymbolType::Class)
            {
                const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                if (visited.insert(qName).second)
                {
                    hierarchy.push_back(qName);
                }
                EnqueueClassBases(sym, symbolTable, visited, queue);
            }
            else if (sym.type == SymbolType::Interface)
            {
                EnqueueInterfaceBases(sym, visited, queue);
            }
        }
    }

    if (cache)
    {
        cache->hierarchies.emplace(rootType, hierarchy);
    }

    return hierarchy;
}

std::string ResolveBaseClass(std::string_view className, const SymbolTable& symbolTable)
{
    if (className.empty())
    {
        return "";
    }
    auto hier = GetInheritedTypeHierarchy(std::string(className), symbolTable);
    for (size_t i = 1; i < hier.size(); ++i)
    {
        if (!IsMixinClass(hier[i], symbolTable))
        {
            return hier[i];
        }
    }
    if (hier.size() > 1)
    {
        return hier[1];
    }
    return "";
}

void ExtractTypesFromMemberSymbols(const std::vector<Symbol>& syms, std::string_view className,
                                   std::vector<std::string>& result)
{
    for (const auto& s : syms)
    {
        std::string rawType;
        if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
        {
            if (std::holds_alternative<VariableSignature>(s.signature))
            {
                const auto& var = s.GetVariable();
                rawType = !var.baseTypeName.empty() ? var.baseTypeName : var.typeName;
            }
        }
        else if (s.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(s.signature))
        {
            const auto& fn = s.GetFunction();
            rawType = !fn.returnBaseTypeName.empty() ? fn.returnBaseTypeName : fn.returnType;
        }

        std::string clean = CleanBaseType(rawType);
        if (!clean.empty() && clean != className && !IsPrimitiveTypeName(clean))
        {
            if (std::find(result.begin(), result.end(), clean) == result.end())
            {
                result.push_back(std::move(clean));
            }
        }
    }
}

void InspectDelegateProperty(std::string_view className, const std::string& prop, const SymbolTable& symbolTable,
                             std::vector<std::string>& result)
{
    if (const auto syms = symbolTable.FindMemberSymbolPtr(className, prop))
    {
        ExtractTypesFromMemberSymbols(*syms, className, result);
    }
    if (!prop.starts_with("get_"))
    {
        if (const auto getterSyms = symbolTable.FindMemberSymbolPtr(className, "get_" + prop))
        {
            ExtractTypesFromMemberSymbols(*getterSyms, className, result);
        }
    }
}

void ExpandMixinHostHierarchies(std::string_view containerClass, const SymbolTable& symbolTable,
                                std::vector<std::string>& hierarchy)
{
    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    auto mixinIt = ruleIndex->hostClassesByMixin.find(containerClass);
    if (mixinIt == ruleIndex->hostClassesByMixin.end() && utils::ContainsScopeSeparator(containerClass))
    {
        mixinIt = ruleIndex->hostClassesByMixin.find(LastScopeSegment(containerClass));
    }
    if (mixinIt == ruleIndex->hostClassesByMixin.end())
    {
        return;
    }
    for (const auto& host : mixinIt->second)
    {
        std::string hostKey = host.qualifiedName.empty() ? host.name : host.qualifiedName;
        auto hostHier = GetInheritedTypeHierarchy(hostKey, symbolTable);
        for (auto& hCls : hostHier)
        {
            if (std::find(hierarchy.begin(), hierarchy.end(), hCls) == hierarchy.end())
            {
                hierarchy.push_back(std::move(hCls));
            }
        }
    }
}

std::vector<std::string> ResolveLinkedBaseClasses(std::string_view className, const SymbolTable& symbolTable,
                                                  const LinkedBaseOptions& options)
{
    std::vector<std::string> result;
    if (className.empty() || !options.HasActiveOptions())
    {
        return result;
    }

    if (options.proxyMap)
    {
        auto it = options.proxyMap->find(className);
        if (it == options.proxyMap->end() && utils::ContainsScopeSeparator(className))
        {
            it = options.proxyMap->find(LastScopeSegment(className));
        }
        if (it != options.proxyMap->end())
        {
            for (const auto& target : it->second)
            {
                std::string clean = CleanBaseType(target);
                if (!clean.empty() && clean != className && !IsPrimitiveTypeName(clean) &&
                    std::find(result.begin(), result.end(), clean) == result.end())
                {
                    result.push_back(std::move(clean));
                }
            }
        }
    }

    for (const auto& prop : options.delegateProps)
    {
        InspectDelegateProperty(className, prop, symbolTable, result);
    }

    return result;
}

std::vector<Symbol> FindOverriddenBaseMethodSymbols(std::string_view containerClass, std::string_view methodName,
                                                    const SymbolTable& symbolTable, const LinkedBaseOptions& options)
{
    std::vector<Symbol> result;
    if (containerClass.empty() || methodName.empty())
    {
        return result;
    }

    std::vector<std::string> hierarchy = GetInheritedTypeHierarchy(std::string(containerClass), symbolTable);
    ExpandMixinHostHierarchies(containerClass, symbolTable, hierarchy);

    auto appendMatchingMethods = [&](std::string_view ownerClass)
    {
        const auto syms = symbolTable.FindMemberSymbolPtr(ownerClass, methodName);
        if (!syms)
        {
            return;
        }
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Function)
            {
                result.push_back(s);
            }
        }
    };

    const bool hasLinkedOptions = options.HasActiveOptions();
    ankerl::unordered_dense::set<std::string> visitedOwners;
    for (const auto& ancestor : hierarchy)
    {
        if (ancestor != containerClass && visitedOwners.insert(ancestor).second)
        {
            appendMatchingMethods(ancestor);
        }

        if (hasLinkedOptions)
        {
            auto linkedBases = ResolveLinkedBaseClasses(ancestor, symbolTable, options);
            for (const auto& linkedBase : linkedBases)
            {
                if (linkedBase != containerClass && visitedOwners.insert(linkedBase).second)
                {
                    appendMatchingMethods(linkedBase);
                }
            }
        }
    }

    return result;
}

bool HierarchyIsFullyVisible(const std::string& typeName, const SymbolTable& symbolTable)
{
    for (const auto& ancestor : GetInheritedTypeHierarchy(typeName, symbolTable))
    {
        const auto symbols = symbolTable.FindSymbolsPtr(ancestor);
        if (!symbols)
        {
            return false;
        }

        // The hierarchy walk itself stops at a name it cannot resolve, so an unresolvable base
        // never appears in the list above and has to be looked for here, one level down.
        for (const auto& sym : *symbols)
        {
            if (sym.type != SymbolType::Class)
            {
                continue;
            }
            for (const auto& base : sym.GetClass().bases)
            {
                if (!symbolTable.FindSymbolsPtr(CleanBaseType(base)))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

void TraverseAncestorsAndInterfaces(const std::string& rootType, const SymbolTable& symbolTable,
                                    ankerl::unordered_dense::set<std::string>& visited,
                                    std::vector<std::string>& related)
{
    std::vector<std::string> queue;
    visited.insert(rootType);
    queue.push_back(rootType);

    size_t head = 0;
    while (head < queue.size())
    {
        const std::string curType = queue[head++];
        related.push_back(curType);

        const auto symbols = symbolTable.FindSymbolsPtr(curType);
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Class)
            {
                for (const auto& base : sym.GetClass().bases)
                {
                    std::string clean = CleanBaseType(base);
                    if (!clean.empty() && visited.insert(clean).second)
                    {
                        queue.push_back(clean);
                    }
                }
            }
            else if (sym.type == SymbolType::Interface)
            {
                for (const auto& base : sym.GetInterface().inheritedInterfaces)
                {
                    std::string clean = CleanBaseType(base);
                    if (!clean.empty() && visited.insert(clean).second)
                    {
                        queue.push_back(clean);
                    }
                }
            }
        }
    }
}

void TraverseDerivedClasses(const std::vector<std::string>& initialFrontier, const SymbolTable& symbolTable,
                            ankerl::unordered_dense::set<std::string>& visited, std::vector<std::string>& related)
{
    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }

    std::vector<std::string> frontier = initialFrontier;
    while (!frontier.empty())
    {
        std::vector<std::string> next;
        for (const auto& baseName : frontier)
        {
            const auto it = ruleIndex->derivedByBase.find(baseName);
            if (it == ruleIndex->derivedByBase.end())
            {
                continue;
            }
            for (const auto& derived : it->second)
            {
                if (visited.insert(derived.qualifiedName).second)
                {
                    related.push_back(derived.qualifiedName);
                    next.push_back(derived.qualifiedName);
                }
                if (!derived.name.empty() && derived.name != derived.qualifiedName &&
                    visited.insert(derived.name).second)
                {
                    related.push_back(derived.name);
                    next.push_back(derived.name);
                }
            }
        }
        frontier = std::move(next);
    }
}

std::vector<std::string> GetAllRelatedClasses(const std::string& className, const SymbolTable& symbolTable)
{
    std::vector<std::string> related;
    const std::string rootType = CleanBaseType(className);
    if (rootType.empty())
    {
        return related;
    }

    ankerl::unordered_dense::set<std::string> visited;
    TraverseAncestorsAndInterfaces(rootType, symbolTable, visited, related);

    const std::vector<std::string> frontier(related.begin(), related.end());
    TraverseDerivedClasses(frontier, symbolTable, visited, related);

    return related;
}

std::vector<std::string> GetDerivedClasses(const std::string& className, const SymbolTable& symbolTable)
{
    std::vector<std::string> derivedClasses;
    std::string rootType = CleanBaseType(className);
    if (rootType.empty())
    {
        return derivedClasses;
    }

    ankerl::unordered_dense::set<std::string> visited;
    visited.insert(rootType);

    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return derivedClasses;
    }

    std::vector<std::string> frontier;
    frontier.push_back(rootType);

    while (!frontier.empty())
    {
        std::vector<std::string> next;
        for (const auto& baseName : frontier)
        {
            const auto it = ruleIndex->derivedByBase.find(baseName);
            if (it == ruleIndex->derivedByBase.end())
            {
                continue;
            }

            for (const auto& derived : it->second)
            {
                if (visited.insert(derived.qualifiedName).second)
                {
                    derivedClasses.push_back(derived.qualifiedName);
                    next.push_back(derived.qualifiedName);
                }

                if (!derived.name.empty() && derived.name != derived.qualifiedName &&
                    visited.insert(derived.name).second)
                {
                    derivedClasses.push_back(derived.name);
                    next.push_back(derived.name);
                }
            }
        }

        frontier = std::move(next);
    }

    return derivedClasses;
}

void AddClassAndShortName(const std::string& c, ankerl::unordered_dense::set<std::string>& visited,
                          std::vector<std::string>& result)
{
    if (c.empty() || !visited.insert(c).second)
    {
        return;
    }
    result.push_back(c);
    const std::string_view shortName = utils::LastScopeSegment(c);
    if (shortName != c)
    {
        if (visited.insert(std::string(shortName)).second)
        {
            result.push_back(std::string(shortName));
        }
    }
}

bool HasNonPrivateMember(const std::string& typeName, const std::string& memberName, const SymbolTable& symbolTable)
{
    const auto syms = symbolTable.FindMemberSymbolPtr(typeName, memberName);
    if (!syms)
    {
        return false;
    }
    for (const auto& s : *syms)
    {
        AccessModifier a = AccessModifier::Public;
        if (std::holds_alternative<FunctionSignature>(s.signature))
        {
            a = s.GetFunction().modifiers.access;
        }
        else if (std::holds_alternative<VariableSignature>(s.signature))
        {
            a = s.GetVariable().modifiers.access;
        }
        if (a != AccessModifier::Private)
        {
            return true;
        }
    }
    return false;
}

std::string FindDeclaringRootClass(const std::string& cleanClass, const std::string& memberName,
                                   const SymbolTable& symbolTable)
{
    std::string rootClass = cleanClass;
    const auto hierarchy = GetInheritedTypeHierarchy(cleanClass, symbolTable);
    for (size_t i = 1; i < hierarchy.size(); ++i)
    {
        if (HasNonPrivateMember(hierarchy[i], memberName, symbolTable))
        {
            rootClass = hierarchy[i];
        }
    }
    return rootClass;
}

struct MixinExpansionContext
{
    const std::string& cleanClass;
    const SymbolTable& symbolTable;
    ankerl::unordered_dense::set<std::string>& visited;
    std::vector<std::string>& result;
};

void ExpandMixinFromRuleIndex(const rules::RuleIndex& ruleIndex, const MixinExpansionContext& ctx)
{
    auto it = ruleIndex.hostClassesByMixin.find(ctx.cleanClass);
    if (it == ruleIndex.hostClassesByMixin.end() && utils::ContainsScopeSeparator(ctx.cleanClass))
    {
        it = ruleIndex.hostClassesByMixin.find(std::string(LastScopeSegment(ctx.cleanClass)));
    }
    if (it == ruleIndex.hostClassesByMixin.end())
    {
        return;
    }
    for (const auto& host : it->second)
    {
        std::string hostKey = host.qualifiedName.empty() ? host.name : host.qualifiedName;
        AddClassAndShortName(hostKey, ctx.visited, ctx.result);
        if (auto hostSyms = ctx.symbolTable.FindSymbolsPtr(hostKey))
        {
            for (const auto& hs : *hostSyms)
            {
                if (hs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(hs.signature))
                {
                    for (const auto& sibMixin : hs.GetClass().includedMixins)
                    {
                        AddClassAndShortName(sibMixin, ctx.visited, ctx.result);
                    }
                }
            }
        }
    }
}

void ExpandMixinFromSymbolTable(const MixinExpansionContext& ctx)
{
    std::string_view shortKey = LastScopeSegment(ctx.cleanClass);
    ctx.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& hs : symbols)
            {
                if (hs.type != SymbolType::Class || !std::holds_alternative<ClassSignature>(hs.signature))
                {
                    continue;
                }
                const auto& cls = hs.GetClass();
                bool includes = false;
                for (const auto& m : cls.includedMixins)
                {
                    if (m == ctx.cleanClass || LastScopeSegment(m) == shortKey)
                    {
                        includes = true;
                        break;
                    }
                }
                if (includes)
                {
                    std::string hostKey = hs.qualifiedName.empty() ? hs.name : hs.qualifiedName;
                    AddClassAndShortName(hostKey, ctx.visited, ctx.result);
                    for (const auto& sibMixin : cls.includedMixins)
                    {
                        AddClassAndShortName(sibMixin, ctx.visited, ctx.result);
                    }
                }
            }
        });
}

void ExpandMixinHostsAndSiblings(const std::string& cleanClass, const SymbolTable& symbolTable,
                                 ankerl::unordered_dense::set<std::string>& visited, std::vector<std::string>& result)
{
    MixinExpansionContext ctx{cleanClass, symbolTable, visited, result};
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        ExpandMixinFromRuleIndex(*ruleIndex, ctx);
    }
    else
    {
        ExpandMixinFromSymbolTable(ctx);
    }
}

void CollectPrivateCompatibleClasses(const std::string& cleanClass, const SymbolTable& symbolTable,
                                     ankerl::unordered_dense::set<std::string>& visited,
                                     std::vector<std::string>& result)
{
    AddClassAndShortName(cleanClass, visited, result);
    if (!utils::ContainsScopeSeparator(cleanClass))
    {
        for (const auto& ts : symbolTable.FindTypeSymbolsByShortName(cleanClass))
        {
            if ((ts.type == SymbolType::Class || ts.type == SymbolType::Interface) && !ts.qualifiedName.empty())
            {
                AddClassAndShortName(ts.qualifiedName, visited, result);
            }
        }
    }
    ExpandMixinHostsAndSiblings(cleanClass, symbolTable, visited, result);
}

std::vector<std::string> GetCompatibleMemberClasses(const std::string& className, const std::string& memberName,
                                                    AccessModifier access, const SymbolTable& symbolTable)
{
    std::vector<std::string> result;
    const std::string cleanClass = CleanBaseType(className);
    if (cleanClass.empty())
    {
        return result;
    }

    ankerl::unordered_dense::set<std::string> visited;
    if (access == AccessModifier::Private)
    {
        CollectPrivateCompatibleClasses(cleanClass, symbolTable, visited, result);
        return result;
    }

    const std::string rootClass = FindDeclaringRootClass(cleanClass, memberName, symbolTable);
    AddClassAndShortName(rootClass, visited, result);

    for (const auto& d : GetDerivedClasses(rootClass, symbolTable))
    {
        AddClassAndShortName(d, visited, result);
    }

    ExpandMixinHostsAndSiblings(rootClass, symbolTable, visited, result);

    if (access == AccessModifier::Public && rootClass != cleanClass)
    {
        for (const auto& base : GetInheritedTypeHierarchy(rootClass, symbolTable))
        {
            AddClassAndShortName(base, visited, result);
        }
    }

    return result;
}

TSNode FindEnclosingAncestorNode(TSNode node, TSSymbol targetSym)
{
    TSNode parent = parser::GetParent(node);
    int depth = 0;
    while (!ts_node_is_null(parent) && ts_node_symbol(parent) != targetSym)
    {
        if (++depth > constants::k_maxAstClimbDepth)
        {
            return TSNode{};
        }
        parent = parser::GetParent(parent);
    }
    return parent;
}

bool HasBaseClassList(TSNode classNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t childCount = ts_node_named_child_count(classNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        if (ts_node_symbol(ts_node_named_child(classNode, i)) == gs.symBaseClassList)
        {
            return true;
        }
    }
    return false;
}

bool IsBaseConstructorCall(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode function = FindEnclosingAncestorNode(node, gs.symFuncDeclaration);
    if (ts_node_is_null(function) ||
        !ts_node_is_null(parser::GetChildByField(function, parser::GetGrammarSymbols().fieldReturnType)))
    {
        return false;
    }

    const TSNode functionName = parser::GetChildByField(function, parser::GetGrammarSymbols().fieldName);
    const TSNode owner = FindEnclosingAncestorNode(function, gs.symClassDeclaration);
    if (ts_node_is_null(functionName) || ts_node_is_null(owner))
    {
        return false;
    }

    const TSNode className = parser::GetChildByField(owner, parser::GetGrammarSymbols().fieldName);
    if (ts_node_is_null(className) || GetNodeText(className, sourceCode) != GetNodeText(functionName, sourceCode))
    {
        return false;
    }

    return HasBaseClassList(owner);
}

TSNode AdjustStartNodeIfDeclarationName(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode cur = node;
    for (int depth = 0; depth < 5 && !ts_node_is_null(cur); ++depth)
    {
        const TSNode parent = parser::GetParent(cur);
        if (ts_node_is_null(parent))
        {
            break;
        }
        const TSSymbol parentSym = ts_node_symbol(parent);
        if (parentSym == gs.symBaseClassList)
        {
            TSNode decl = parser::GetParent(parent);
            if (!ts_node_is_null(decl))
            {
                return decl;
            }
        }
        if (parentSym == gs.symClassDeclaration || parentSym == gs.symInterfaceDeclaration ||
            parentSym == gs.symNamespaceDeclaration || parentSym == gs.symMixinDeclaration)
        {
            const TSNode nameChild = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldName);
            if (!ts_node_is_null(nameChild) && ts_node_start_byte(nameChild) == ts_node_start_byte(node) &&
                ts_node_end_byte(nameChild) == ts_node_end_byte(node))
            {
                return parent;
            }
        }
        cur = parent;
    }
    return node;
}

std::optional<ContainerKind> ClassifyContainerKind(TSSymbol sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symClassDeclaration || sym == gs.symMixinDeclaration)
    {
        return ContainerKind::Class;
    }
    if (sym == gs.symInterfaceDeclaration)
    {
        return ContainerKind::Interface;
    }
    if (sym == gs.symNamespaceDeclaration)
    {
        return ContainerKind::Namespace;
    }
    return std::nullopt;
}

void ComputeContainerQualifiedNames(std::vector<ContainerInfo>& containers)
{
    for (size_t i = 0; i < containers.size(); ++i)
    {
        std::string qName;
        for (size_t j = containers.size(); j > i; --j)
        {
            if (!qName.empty())
            {
                qName += "::";
            }
            qName += containers[j - 1].name;
        }
        containers[i].qualifiedName = std::move(qName);
    }
}

static void AppendRawContainers(TSNode nameNode, ContainerKind kind, std::string_view sourceCode,
                                std::vector<ContainerInfo>& rawContainers)
{
    if (ts_node_is_null(nameNode))
    {
        return;
    }
    const uint32_t start = ts_node_start_byte(nameNode);
    const uint32_t end = ts_node_end_byte(nameNode);
    if (start >= end || end > sourceCode.size())
    {
        return;
    }

    std::string text(sourceCode.substr(start, end - start));
    if (kind == ContainerKind::Namespace && utils::ContainsScopeSeparator(text))
    {
        auto segs = SplitScopeSegments(text);
        for (auto it = segs.rbegin(); it != segs.rend(); ++it)
        {
            if (!it->empty())
            {
                rawContainers.push_back({*it, "", kind});
            }
        }
    }
    else
    {
        rawContainers.push_back({std::move(text), "", kind});
    }
}

std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode)
{
    std::vector<ContainerInfo> rawContainers;
    if (ts_node_is_null(node))
    {
        return rawContainers;
    }

    TSNode current = parser::GetParent(AdjustStartNodeIfDeclarationName(node));
    size_t depth = 0;
    bool crossedLambda = false;
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(current) && ++depth <= constants::k_maxAstClimbDepth)
    {
        if (ts_node_symbol(current) == gs.symLambdaExpression)
        {
            crossedLambda = true;
        }

        if (const auto kind = ClassifyContainerKind(ts_node_symbol(current)))
        {
            if (crossedLambda && (*kind == ContainerKind::Class || *kind == ContainerKind::Interface))
            {
                current = parser::GetParent(current);
                continue;
            }

            AppendRawContainers(parser::GetChildByField(current, parser::GetGrammarSymbols().fieldName), *kind,
                                sourceCode, rawContainers);
        }
        current = parser::GetParent(current);
    }

    ComputeContainerQualifiedNames(rawContainers);
    return rawContainers;
}

std::vector<std::string> CollectUsingNamespaces(const NodeIndex& nodeIndex, std::string_view sourceCode)
{
    std::vector<std::string> usings;
    for (TSNode cur : nodeIndex.Nodes(parser::nodes::UsingDeclaration))
    {
        TSNode nameNode = parser::GetChildByField(cur, parser::GetGrammarSymbols().fieldName);
        if (!ts_node_is_null(nameNode))
        {
            std::string uName = GetNodeText(nameNode, sourceCode);
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front())))
            {
                uName.erase(uName.begin());
            }
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back())))
            {
                uName.pop_back();
            }
            if (!uName.empty())
            {
                usings.push_back(uName);
            }
        }
    }
    return usings;
}

std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode)
{
    std::vector<std::string> usings;
    if (ts_node_is_null(root) || sourceCode.find("using") == std::string_view::npos)
    {
        return usings;
    }

    const TSQuery* query = parser::QueryRegistry::GetUsingQuery();
    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    if (!query || !cursor)
    {
        return usings;
    }

    ts_query_cursor_exec(cursor, query, root);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match))
    {
        for (uint16_t i = 0; i < match.capture_count; ++i)
        {
            TSNode nameNode = match.captures[i].node;
            std::string uName = GetNodeText(nameNode, sourceCode);
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front())))
                uName.erase(uName.begin());
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back())))
                uName.pop_back();
            if (!uName.empty())
            {
                usings.push_back(std::move(uName));
            }
        }
    }
    return usings;
}

const std::vector<std::string>& GetUsingNamespaces(const SemanticAnalysisRequest& request)
{
    if (!request.usingNamespacesCache.has_value())
    {
        if (request.sourceCode.find("using") == std::string_view::npos)
        {
            request.usingNamespacesCache = std::vector<std::string>{};
        }
        else if (request.nodeIndex)
        {
            request.usingNamespacesCache = CollectUsingNamespaces(*request.nodeIndex, request.sourceCode);
        }
        else if (request.tree)
        {
            request.usingNamespacesCache = CollectUsingNamespaces(ts_tree_root_node(request.tree), request.sourceCode);
        }
        else
        {
            request.usingNamespacesCache = std::vector<std::string>{};
        }
    }
    return *request.usingNamespacesCache;
}

/**
 * @brief Checks if any symbol or container in the symbol table matches the scope prefix.
 */
static bool HasContainerMatchingPrefix(const SymbolTable& table, const std::string& prefix)
{
    bool hasContainer = false;
    table.ForEachSymbol(
        [&](const std::string& qName, const std::vector<Symbol>& syms)
        {
            if (hasContainer)
            {
                return;
            }
            for (const auto& s : syms)
            {
                if (s.containerName == prefix || utils::HasScopePrefix(s.containerName, prefix) || s.name == prefix ||
                    qName == prefix || utils::HasScopePrefix(qName, prefix))
                {
                    hasContainer = true;
                    return;
                }
            }
        });
    return hasContainer;
}

/**
 * @brief Checks if the prefix is known relative to enclosing containers.
 */
static bool IsKnownInEnclosingContainers(const SymbolTable& table, const std::string& prefix, TSNode node,
                                         std::string_view sourceCode)
{
    auto containers = GetEnclosingContainers(node, sourceCode);
    for (const auto& c : containers)
    {
        std::string q = utils::JoinScopedName(c.qualifiedName, prefix);
        if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if the prefix is known relative to using namespaces.
 */
static bool IsKnownInUsingNamespaces(const SymbolTable& table, const std::string& prefix, TSNode node,
                                     std::string_view sourceCode, std::span<const std::string> precomputedUsings)
{
    if (!precomputedUsings.empty())
    {
        for (const auto& ns : precomputedUsings)
        {
            std::string q = utils::JoinScopedName(ns, prefix);
            if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
            {
                return true;
            }
        }
        return false;
    }

    if (sourceCode.find("using") == std::string_view::npos)
    {
        return false;
    }

    auto usings = CollectUsingNamespaces(GetRootNode(node), sourceCode);
    for (const auto& ns : usings)
    {
        std::string q = utils::JoinScopedName(ns, prefix);
        if (table.HasSymbol(q) || table.HasSymbolAnywhere(q))
        {
            return true;
        }
    }
    return false;
}

bool IsKnownScope(const std::string& prefix, TSNode node, std::string_view sourceCode, const SymbolTable& table,
                  std::span<const std::string> precomputedUsings)
{
    if (prefix.empty() || table.HasSymbol(prefix) || table.HasSymbolAnywhere(prefix))
    {
        return true;
    }

    if (HasContainerMatchingPrefix(table, prefix))
    {
        return true;
    }

    if (IsKnownInEnclosingContainers(table, prefix, node, sourceCode))
    {
        return true;
    }

    return IsKnownInUsingNamespaces(table, prefix, node, sourceCode, precomputedUsings);
}

/**
 * @brief Filters candidate symbols to ensure they match the expected container kind.
 * @param[in] found Symbol candidates to filter.
 * @param[in] expected Expected enclosing container kind.
 * @return Filtered vector containing only symbols consistent with expected container.
 */
static std::vector<Symbol> FilterSymbolsByContainerKind(std::vector<Symbol> found, ContainerKind expected)
{
    std::erase_if(found,
                  [expected](const Symbol& s)
                  {
                      if (expected == ContainerKind::Class || expected == ContainerKind::Interface)
                      {
                          return s.containerKind == ContainerKind::Namespace || s.containerKind == ContainerKind::Enum;
                      }
                      if (expected == ContainerKind::Namespace)
                      {
                          return s.containerKind == ContainerKind::Class ||
                                 s.containerKind == ContainerKind::Interface || s.containerKind == ContainerKind::Enum;
                      }
                      return false;
                  });
    return found;
}

/**
 * @brief Searches for a symbol name within an enclosing container hierarchy.
 */
static std::vector<Symbol> FindSymbolInContainerHierarchy(const SymbolTable& symbolTable,
                                                          const std::vector<ContainerInfo>& containers,
                                                          const std::string& name)
{
    size_t hops = 0;
    ankerl::unordered_dense::set<std::string> visitedContainers;
    for (const auto& container : containers)
    {
        if (++hops > kMaxScopeDepth)
        {
            break;
        }
        if (!visitedContainers.insert(container.qualifiedName).second)
        {
            continue;
        }

        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            auto hierarchy = GetInheritedTypeHierarchy(container.qualifiedName, symbolTable);
            for (const auto& cls : hierarchy)
            {
                auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(cls, name), container.kind);
                if (!found.empty())
                {
                    return found;
                }
            }

            if (container.name != container.qualifiedName)
            {
                auto bareHierarchy = GetInheritedTypeHierarchy(container.name, symbolTable);
                for (const auto& cls : bareHierarchy)
                {
                    auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(cls, name), container.kind);
                    if (!found.empty())
                    {
                        return found;
                    }
                }
            }
        }
        else if (container.kind == ContainerKind::Namespace)
        {
            auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(container.qualifiedName, name),
                                                      container.kind);
            if (!found.empty())
            {
                return found;
            }
        }
    }
    return {};
}

/**
 * @brief Searches for a symbol name across using namespaces.
 */
static std::vector<Symbol> FindSymbolInUsingNamespaces(const SymbolTable& symbolTable,
                                                       const std::vector<std::string>& usingNamespaces,
                                                       const std::string& name)
{
    for (const auto& ns : usingNamespaces)
    {
        auto found = symbolTable.FindMemberSymbols(ns, name);
        if (!found.empty())
        {
            return found;
        }
    }
    return {};
}

/**
 * @brief Fallback lookup for enum members and short-name types.
 */
static std::vector<Symbol> FindSymbolEnumOrTypeFallback(const SymbolTable& symbolTable, const std::string& name)
{
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        auto it = ruleIndex->enumSymbolsByMemberName.find(name);
        if (it != ruleIndex->enumSymbolsByMemberName.end())
        {
            return it->second;
        }
    }
    return symbolTable.FindTypeSymbolsByShortName(name);
}

std::vector<Symbol> FindSymbolsInScope(const SymbolTable& symbolTable, const std::vector<ContainerInfo>& containers,
                                       const std::string& name, const std::vector<std::string>& usingNamespaces)
{
    if (name.empty())
    {
        return {};
    }

    if (utils::HasLeadingGlobalPrefix(name))
    {
        const std::string_view rest = utils::StripGlobalPrefix(name);
        if (!utils::ContainsScopeSeparator(rest))
        {
            std::vector<Symbol> filtered;
            if (auto symsPtr = symbolTable.FindSymbolsPtr(std::string(rest)))
            {
                for (const auto& sym : *symsPtr)
                {
                    if (sym.containerName.empty() && sym.type != SymbolType::CallReference)
                    {
                        filtered.push_back(sym);
                    }
                }
            }
            return filtered;
        }
        return symbolTable.FindSymbols(rest);
    }

    if (auto found = FindSymbolInContainerHierarchy(symbolTable, containers, name); !found.empty())
    {
        return found;
    }

    if (auto globalFound = symbolTable.FindSymbols(name); !globalFound.empty())
    {
        return globalFound;
    }

    if (auto found = FindSymbolInUsingNamespaces(symbolTable, usingNamespaces, name); !found.empty())
    {
        return found;
    }

    return FindSymbolEnumOrTypeFallback(symbolTable, name);
}

static void CollectUsingsFromAncestor(TSNode p, std::string_view sourceCode, std::vector<std::string>& usings)
{
    TSTreeCursor cursor = ts_tree_cursor_new(p);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            const auto& gs = parser::GetGrammarSymbols();
            if (ts_node_symbol(ch) == gs.symUsingDeclaration)
            {
                TSNode nameNode = parser::GetChildByField(ch, gs.fieldName);
                if (!ts_node_is_null(nameNode))
                {
                    std::string uName = GetNodeText(nameNode, sourceCode);
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front())))
                        uName.erase(uName.begin());
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back())))
                        uName.pop_back();
                    if (!uName.empty())
                    {
                        usings.push_back(uName);
                    }
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

std::vector<Symbol> FindSymbolsInScope(const std::string& name, TSNode node, std::string_view sourceCode,
                                       const SymbolTable& symbolTable)
{
    std::vector<std::string> usings;
    if (sourceCode.find("using") != std::string_view::npos)
    {
        TSNode p = node;
        for (int depth = 0; depth < 16 && !ts_node_is_null(p); ++depth, p = parser::GetParent(p))
        {
            CollectUsingsFromAncestor(p, sourceCode, usings);
        }
    }

    std::vector<ContainerInfo> containers = GetEnclosingContainers(node, sourceCode);
    return FindSymbolsInScope(symbolTable, containers, name, usings);
}

static std::string ResolveFloatingPointLiteral(std::string_view text)
{
    if (text.find('f') != std::string_view::npos || text.find('F') != std::string_view::npos)
    {
        return "float";
    }
    if (text.find('.') != std::string_view::npos || text.find('e') != std::string_view::npos ||
        text.find('E') != std::string_view::npos)
    {
        return "double";
    }
    return "";
}

static std::string ResolveIntegerLiteralSuffix(std::string_view text)
{
    const bool hasU = text.find('u') != std::string_view::npos || text.find('U') != std::string_view::npos;
    const bool hasL = text.find('l') != std::string_view::npos || text.find('L') != std::string_view::npos;
    if (hasU && hasL)
    {
        return "uint64";
    }
    if (hasL || text.ends_with("i64"))
    {
        return "int64";
    }
    if (hasU)
    {
        return "uint";
    }
    return "int";
}

/**
 * @brief Parses numeric literal text to determine primitive type.
 */
static std::string ResolveNumberLiteral(std::string_view text)
{
    const bool isRadixPrefixed =
        text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X' || text[1] == 'b' || text[1] == 'B');

    if (!isRadixPrefixed)
    {
        if (auto fp = ResolveFloatingPointLiteral(text); !fp.empty())
        {
            return fp;
        }
    }
    return ResolveIntegerLiteralSuffix(text);
}

/**
 * @brief Resolves literal expression type.
 */
static std::string ResolveLiteralExpr(TSSymbol nodeSym, TSNode exprNode, std::string_view sourceCode,
                                      std::string_view stringTypeName)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (nodeSym == gs.symStringLiteral)
    {
        return std::string(stringTypeName);
    }
    if (nodeSym == gs.symBooleanLiteral)
    {
        return "bool";
    }
    if (nodeSym == gs.symNullLiteral)
    {
        return "null";
    }
    if (nodeSym == gs.symNumberLiteral)
    {
        return ResolveNumberLiteral(GetNodeText(exprNode, sourceCode));
    }
    return "";
}

/**
 * @brief Resolves parenthesized expression type.
 */
static std::string ResolveParenthesizedExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    const uint32_t namedCount = ts_node_named_child_count(exprNode);
    for (uint32_t i = 0; i < namedCount; ++i)
    {
        TSNode child = ts_node_named_child(exprNode, i);
        std::string res = ResolveExpressionType(child, ctx, depth + 1);
        if (!res.empty())
        {
            return res;
        }
    }
    const uint32_t count = ts_node_child_count(exprNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(exprNode, i);
        if (ts_node_is_named(child))
        {
            std::string res = ResolveExpressionType(child, ctx, depth + 1);
            if (!res.empty())
            {
                return res;
            }
        }
    }
    return "";
}

/**
 * @brief Resolves 'this' expression type based on enclosing container hierarchy.
 */
static std::string ResolveThisIdentifier(TSNode exprNode, const ExpressionTypeContext& ctx,
                                         const std::string& hostClass)
{
    if (!hostClass.empty())
    {
        return CleanExpressionType(hostClass);
    }
    auto containers = GetEnclosingContainers(exprNode, ctx.sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            return CleanExpressionType(c.qualifiedName.empty() ? c.name : c.qualifiedName);
        }
    }
    return "";
}

/**
 * @brief Resolves special 'this' identifier.
 */
static std::string ResolveIdentifierSpecial(std::string_view name, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const bool isVirtual = ctx.uri.starts_with("angelscript-virtual:") || ctx.uri.starts_with("angelscript-virtual://");
    const std::string hostClass = isVirtual ? SymbolTable::ExtractVirtualHostClass(ctx.uri) : "";

    if (name == "this")
    {
        return ResolveThisIdentifier(exprNode, ctx, hostClass);
    }
    return "";
}

static std::string ResolveThisExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    return ResolveIdentifierSpecial("this", exprNode, ctx);
}

/**
 * @brief Resolves scoped identifier expression type.
 */
static std::string ResolveScopedEnumMember(std::string_view whole, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto [qualView, mem] = utils::SplitScopedName(whole);
    if (qualView.empty())
    {
        return "";
    }
    std::string qual(qualView);
    auto qSyms = ctx.symbolTable.FindSymbols(qual);
    if (qSyms.empty())
    {
        qSyms = FindSymbolsInScope(qual, exprNode, ctx.sourceCode, ctx.symbolTable);
    }
    for (const auto& qSym : qSyms)
    {
        if (qSym.type == SymbolType::Enum && std::holds_alternative<EnumSignature>(qSym.signature))
        {
            for (const auto& m : qSym.GetEnum().members)
            {
                if (m.name == mem)
                {
                    return qSym.qualifiedName.empty() ? qSym.name : qSym.qualifiedName;
                }
            }
        }
    }
    return "";
}

static std::string ExtractVariableOrPropertyType(std::span<const Symbol> syms, const SymbolTable& table)
{
    for (const auto& sym : syms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            !sym.GetVariable().typeName.empty())
        {
            std::string type = sym.GetVariable().typeName;
            if (!sym.containerName.empty())
            {
                type = QualifyTypeInContainer(type, sym.containerName, table);
            }
            return CleanExpressionType(type);
        }
    }
    return {};
}

static std::string ResolveGlobalScopedExpr(TSNode exprNode, std::string_view rootPath, const ExpressionTypeContext& ctx)
{
    auto syms = ctx.symbolTable.FindSymbols(rootPath);
    if (std::string type = ExtractVariableOrPropertyType(syms, ctx.symbolTable); !type.empty())
    {
        return type;
    }
    return ResolveScopedEnumMember(std::string(rootPath), exprNode, ctx);
}

static std::string ResolveScopedIdentifierExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    const auto gsq = analysis::GetGlobalScopeQualifier(exprNode, ctx.sourceCode);
    if (gsq.isGlobal)
    {
        return ResolveGlobalScopedExpr(exprNode, gsq.rootPath, ctx);
    }

    TSNode lastIdentifier = {};
    bool qualified = false;
    const uint32_t childCount = ts_node_named_child_count(exprNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_named_child(exprNode, i);
        if (ts_node_symbol(child) == parser::GetGrammarSymbols().symIdentifier)
        {
            qualified = !ts_node_is_null(lastIdentifier);
            lastIdentifier = child;
        }
    }

    if (qualified)
    {
        const std::string whole = GetNodeText(exprNode, ctx.sourceCode);
        auto wholeSyms = ctx.symbolTable.FindSymbols(whole);
        if (wholeSyms.empty())
        {
            wholeSyms = FindSymbolsInScope(whole, exprNode, ctx.sourceCode, ctx.symbolTable);
        }
        if (std::string type = ExtractVariableOrPropertyType(wholeSyms, ctx.symbolTable); !type.empty())
        {
            return type;
        }
        return ResolveScopedEnumMember(whole, exprNode, ctx);
    }

    return ts_node_is_null(lastIdentifier) ? std::string() : ResolveExpressionType(lastIdentifier, ctx, depth + 1);
}

/**
 * @brief Resolves member symbols or accessors from the enclosing class hierarchy.
 * @param[in] name Identifier name.
 * @param[in] exprNode AST syntax node.
 * @param[in] ctx Expression type resolution context.
 * @return Matching member symbols or property accessors, if found.
 */
static std::vector<Symbol> ResolveEnclosingContainerMemberSymbols(const std::string& name, TSNode exprNode,
                                                                  const ExpressionTypeContext& ctx)
{
    auto containers = GetEnclosingContainers(exprNode, ctx.sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind != ContainerKind::Class && c.kind != ContainerKind::Interface)
        {
            continue;
        }
        const std::string& typeName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
        auto hierarchy = GetInheritedTypeHierarchy(typeName, ctx.symbolTable);
        for (const auto& cls : hierarchy)
        {
            auto found = FilterSymbolsByContainerKind(ctx.symbolTable.FindMemberSymbols(cls, name), c.kind);
            if (!found.empty())
            {
                return found;
            }
            auto accessors = FindPropertyAccessors(cls, name, ctx.symbolTable, false);
            if (!accessors.empty())
            {
                return accessors;
            }
        }
        break;
    }
    return {};
}

/**
 * @brief Finds symbol matches for a bare identifier.
 * @param[in] name Identifier name.
 * @param[in] exprNode AST syntax node.
 * @param[in] ctx Expression type resolution context.
 * @return Candidate symbols in lexical, member, or global scope.
 */
static std::vector<Symbol> ResolveIdentifierSymbols(const std::string& name, TSNode exprNode,
                                                    const ExpressionTypeContext& ctx)
{
    auto syms = FindSymbolsInScope(name, exprNode, ctx.sourceCode, ctx.symbolTable);
    if (!syms.empty())
    {
        return syms;
    }

    syms = ResolveEnclosingContainerMemberSymbols(name, exprNode, ctx);
    if (!syms.empty())
    {
        return syms;
    }

    return ctx.symbolTable.FindSymbols(name);
}

/**
 * @brief Virtual document fallback resolution for identifiers in mixin host classes.
 */
static std::string ResolveIdentifierVirtualFallback(const std::string& name, const ExpressionTypeContext& ctx)
{
    if (!ctx.uri.starts_with("angelscript-virtual:") && !ctx.uri.starts_with("angelscript-virtual://"))
    {
        return "";
    }
    std::string hostClass = SymbolTable::ExtractVirtualHostClass(ctx.uri);
    if (hostClass.empty())
    {
        return "";
    }
    auto hierarchy = GetInheritedTypeHierarchy(hostClass, ctx.symbolTable);
    for (const auto& cls : hierarchy)
    {
        auto found = ctx.symbolTable.FindMemberSymbols(cls, name);
        if (!found.empty())
        {
            for (const auto& sym : found)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                    !sym.GetVariable().typeName.empty())
                {
                    std::string type = sym.GetVariable().typeName;
                    if (!sym.containerName.empty())
                    {
                        type = QualifyTypeInContainer(type, sym.containerName, ctx.symbolTable);
                    }
                    return CleanExpressionType(type);
                }
            }
            break;
        }
    }
    for (const auto& cls : hierarchy)
    {
        auto accessors = FindPropertyAccessors(cls, name, ctx.symbolTable, false);
        if (!accessors.empty())
        {
            std::string pType = PropertyTypeFromAccessors(accessors);
            if (!pType.empty())
            {
                return CleanExpressionType(pType);
            }
        }
    }
    return "";
}

/**
 * @brief Fallback lookup for global accessor functions (get_X, set_X).
 */
static std::string ResolveIdentifierAccessorFallback(const std::string& name, const SymbolTable& symbolTable)
{
    for (const auto& prefix : {"get_", "set_"})
    {
        auto accessorSyms = symbolTable.FindSymbols(std::string(prefix) + name);
        for (const auto& sym : accessorSyms)
        {
            if (sym.type == SymbolType::Function && sym.containerName.empty() &&
                std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                const bool isIndexed = (sym.name.starts_with("get_") && !fn.parameters.empty()) ||
                                       (sym.name.starts_with("set_") && fn.parameters.size() > 1);
                if (isIndexed)
                {
                    continue;
                }
                if (!fn.returnType.empty() && fn.returnType != "void")
                {
                    return CleanExpressionType(fn.returnType);
                }
                if (!fn.parameters.empty())
                {
                    return CleanExpressionType(fn.parameters.front().typeName);
                }
            }
        }
    }
    return "";
}

/**
 * @brief Resolves bare identifier expression type.
 */
static std::string ResolveIdentifierExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth = 0)
{
    std::string name = GetNodeText(exprNode, ctx.sourceCode);
    if (auto special = ResolveIdentifierSpecial(name, exprNode, ctx); !special.empty())
    {
        return special;
    }
    if (ctx.scope)
    {
        const LocalDefinition* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(exprNode));
        if (def)
        {
            std::string dType = ResolveLocalDefinitionType(*def, ctx, depth);
            if (!dType.empty())
            {
                return CleanExpressionType(dType);
            }
        }
    }
    auto syms = ResolveIdentifierSymbols(name, exprNode, ctx);
    if (syms.empty())
    {
        if (auto virt = ResolveIdentifierVirtualFallback(name, ctx); !virt.empty())
        {
            return virt;
        }
    }
    for (const auto& sym : syms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            !sym.GetVariable().typeName.empty())
        {
            std::string type = sym.GetVariable().typeName;
            if (!sym.containerName.empty())
            {
                type = QualifyTypeInContainer(type, sym.containerName, ctx.symbolTable);
            }
            return CleanExpressionType(type);
        }
    }
    if (std::string pType = PropertyTypeFromAccessors(syms); !pType.empty())
    {
        return CleanExpressionType(pType);
    }
    return ResolveIdentifierAccessorFallback(name, ctx.symbolTable);
}

/**
 * @brief Maps binary operators to corresponding operator overload names.
 */
static std::pair<std::string_view, std::string_view> GetBinaryOpMethods(std::string_view op)
{
    if (op == "+")
        return {"opAdd", "opAdd_r"};
    if (op == "-")
        return {"opSub", "opSub_r"};
    if (op == "*")
        return {"opMul", "opMul_r"};
    if (op == "/")
        return {"opDiv", "opDiv_r"};
    if (op == "%")
        return {"opMod", "opMod_r"};
    if (op == "**")
        return {"opPow", "opPow_r"};
    if (op == "&")
        return {"opAnd", "opAnd_r"};
    if (op == "|")
        return {"opOr", "opOr_r"};
    if (op == "^")
        return {"opXor", "opXor_r"};
    if (op == "<<")
        return {"opShl", "opShl_r"};
    if (op == ">>")
        return {"opShr", "opShr_r"};
    if (op == ">>>")
        return {"opUShr", "opUShr_r"};
    return {"", ""};
}

struct BinaryOverloadResult
{
    bool hasMatch = false;
    std::vector<ArgumentConversion> bestConversions;
    std::string returnType;
};

/**
 * @brief Searches for binary operator overloads on an operand type.
 * @param[in] typeName Base type name of the receiver.
 * @param[in] opMethod Operator method name (e.g. opAdd or opAdd_r).
 * @param[in] argType Type of the operand argument.
 * @param[in] symbolTable Symbol table to look up candidate methods.
 * @return Overload resolution result with conversions and return type.
 */
static BinaryOverloadResult ResolveBinaryOverloadMatch(const std::string& typeName, std::string_view opMethod,
                                                       const std::string& argType, const SymbolTable& symbolTable)
{
    if (opMethod.empty() || typeName.empty() || parser::primitives::IsPrimitive(typeName))
    {
        return {};
    }
    std::vector<Symbol> candidates;
    auto hierarchy = GetInheritedTypeHierarchy(typeName, symbolTable);
    for (const auto& t : hierarchy)
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(t, opMethod))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function)
                {
                    candidates.push_back(sym);
                }
            }
        }
    }
    if (!candidates.empty())
    {
        auto match = ResolveBestOverload(candidates, {argType}, symbolTable);
        const Symbol* best = match.bestCandidate ? match.bestCandidate : match.priorityCandidate;
        if (best && std::holds_alternative<FunctionSignature>(best->signature))
        {
            return {true, std::move(match.bestConversions), CleanExpressionType(best->GetFunction().returnType)};
        }
    }
    return {};
}

static std::string FindSingleOpImplConvPrimitive(const std::string& typeName, const SymbolTable& symbolTable)
{
    if (typeName.empty() || parser::primitives::IsPrimitive(typeName))
    {
        return "";
    }
    std::string matchedPrim;
    for (const auto& t : GetInheritedTypeHierarchy(typeName, symbolTable))
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(t, "opImplConv"))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                {
                    std::string ret = CleanBaseType(sym.GetFunction().returnType);
                    if (parser::primitives::IsNumeric(ret) || ret == "bool")
                    {
                        if (!matchedPrim.empty() && matchedPrim != ret)
                        {
                            return "";
                        }
                        matchedPrim = std::move(ret);
                    }
                }
            }
        }
    }
    return matchedPrim;
}

static bool IsFloatOrIntPrimitive(std::string_view type)
{
    return type == "float" || type == "double" || type == "int" || type == "uint";
}

static std::string ResolveBinaryShift(const std::string& cleanLeft)
{
    if (cleanLeft == "int64" || cleanLeft == "uint64")
    {
        return cleanLeft;
    }
    return IsUnsignedInteger(cleanLeft) ? "uint" : "int";
}

static std::string ResolveBinaryBitwiseLogic(const std::string& cleanLeft, const std::string& cleanRight,
                                             const SymbolTable* symbolTable)
{
    if (symbolTable && (ResolvesToEnum(cleanLeft, *symbolTable) || ResolvesToEnum(cleanRight, *symbolTable)))
    {
        return "int";
    }
    if (cleanLeft == "int64" || cleanRight == "int64")
    {
        return "int64";
    }
    if (cleanLeft == "uint64" || cleanRight == "uint64")
    {
        return "uint64";
    }
    if (cleanLeft == "uint8" || cleanLeft == "uint16")
    {
        return "uint";
    }
    if (cleanLeft == "int8" || cleanLeft == "int16")
    {
        return "int";
    }
    if (cleanLeft == cleanRight)
    {
        return cleanLeft;
    }
    if (IsUnsignedInteger(cleanLeft) || IsUnsignedInteger(cleanRight))
    {
        return "uint";
    }
    return "int";
}

static std::string ResolveBinaryShiftBitwise(std::string_view op, const std::string& cleanLeft,
                                             const std::string& cleanRight, const SymbolTable* symbolTable = nullptr)
{
    if (op == "<<" || op == ">>" || op == ">>>")
    {
        return ResolveBinaryShift(cleanLeft);
    }
    if (op == "|" || op == "&" || op == "^")
    {
        return ResolveBinaryBitwiseLogic(cleanLeft, cleanRight, symbolTable);
    }
    return "";
}

/**
 * @brief Resolves binary promotion for 64-bit integers and floating-point types.
 * @param[in] cleanLeft Cleaned left operand type.
 * @param[in] cleanRight Cleaned right operand type.
 * @return Widened type name, or empty string.
 */
static std::string ResolveBinary64OrFloat(const std::string& cleanLeft, const std::string& cleanRight)
{
    if (cleanLeft == "double" || cleanRight == "double")
    {
        return "double";
    }
    if (cleanLeft == "float" || cleanRight == "float")
    {
        return "float";
    }
    if (cleanLeft == "uint64" || cleanRight == "uint64")
    {
        return "uint64";
    }
    if (cleanLeft == "int64" || cleanRight == "int64")
    {
        return "int64";
    }
    return "";
}

/**
 * @brief Resolves binary promotion for 32-bit or narrower integer primitives.
 * @param[in] cleanLeft Cleaned left operand type.
 * @param[in] cleanRight Cleaned right operand type.
 * @return Narrower promoted type name, or empty string.
 */
static std::string ResolveBinary32OrLess(const std::string& cleanLeft, const std::string& cleanRight)
{
    if (cleanLeft == "void" || cleanRight == "void")
    {
        return "";
    }
    if (cleanLeft == "uint" || cleanRight == "uint" || cleanLeft == "uint32" || cleanRight == "uint32")
    {
        return "uint";
    }
    if (IsPrimitiveTypeName(cleanLeft) && IsPrimitiveTypeName(cleanRight))
    {
        return "int";
    }
    return "";
}

/**
 * @brief Determines primitive numeric promotion for division operations.
 * @param[in] cleanLeft Cleaned left operand type.
 * @param[in] cleanRight Cleaned right operand type.
 * @param[in] disableIntegerDivision True if asEP_DISABLE_INTEGER_DIVISION is enabled.
 * @return Promoted division type name.
 */
static std::string ResolveBinaryDivisionPromotion(const std::string& cleanLeft, const std::string& cleanRight,
                                                  bool disableIntegerDivision)
{
    if (cleanLeft == "double" || cleanRight == "double")
    {
        return "double";
    }
    if (IsFloatingPointPrimitive(cleanLeft) || IsFloatingPointPrimitive(cleanRight))
    {
        return "float";
    }
    if (!disableIntegerDivision && IsIntegerPrimitive(cleanLeft) && IsIntegerPrimitive(cleanRight))
    {
        return ResolveBinary32OrLess(cleanLeft, cleanRight);
    }
    return "float";
}

/**
 * @brief Determines primitive numeric promotion result for binary expressions.
 * @param[in] op Binary operator string.
 * @param[in] cleanLeft Cleaned left operand type.
 * @param[in] cleanRight Cleaned right operand type.
 * @param[in] disableIntegerDivision True if asEP_DISABLE_INTEGER_DIVISION is enabled.
 * @return Promoted type name, or empty string.
 */
static std::string ResolveBinaryPrimitivePromotion(std::string_view op, const std::string& cleanLeft,
                                                   const std::string& cleanRight, bool disableIntegerDivision = false,
                                                   const SymbolTable* symbolTable = nullptr)
{
    if (cleanLeft.empty() || cleanRight.empty() || cleanLeft == "void" || cleanRight == "void")
    {
        return "";
    }
    if (op == "/" && IsFloatOrIntPrimitive(cleanLeft) && IsFloatOrIntPrimitive(cleanRight))
    {
        return ResolveBinaryDivisionPromotion(cleanLeft, cleanRight, disableIntegerDivision);
    }
    if (auto wide = ResolveBinary64OrFloat(cleanLeft, cleanRight); !wide.empty())
    {
        return wide;
    }
    if (auto shiftRes = ResolveBinaryShiftBitwise(op, cleanLeft, cleanRight, symbolTable); !shiftRes.empty())
    {
        return shiftRes;
    }
    return ResolveBinary32OrLess(cleanLeft, cleanRight);
}

static bool IsBooleanBinaryOpSym(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.tokEqEq || sym == gs.tokBangEq || sym == gs.tokLt || sym == gs.tokLtEq || sym == gs.tokGt ||
           sym == gs.tokGtEq || sym == gs.tokIs || sym == gs.tokBangIs || sym == gs.tokAmpAmp ||
           sym == gs.tokPipePipe || sym == gs.tokAnd || sym == gs.tokOr || sym == gs.tokCaretCaret || sym == gs.tokXor;
}

/**
 * @brief Selects the return type between forward and reverse operator overloads.
 * @param[in] forwardMatch Forward operator overload resolution result.
 * @param[in] reverseMatch Reverse operator overload resolution result.
 * @return Resolved return type, or empty string if neither candidate matched.
 */
static std::string SelectBestBinaryOverload(const BinaryOverloadResult& forwardMatch,
                                            const BinaryOverloadResult& reverseMatch)
{
    if (forwardMatch.hasMatch && reverseMatch.hasMatch)
    {
        if (!forwardMatch.bestConversions.empty() && !reverseMatch.bestConversions.empty())
        {
            if (reverseMatch.bestConversions[0] < forwardMatch.bestConversions[0])
            {
                return reverseMatch.returnType;
            }
            return forwardMatch.returnType;
        }
        return forwardMatch.returnType;
    }
    if (forwardMatch.hasMatch)
    {
        return forwardMatch.returnType;
    }
    if (reverseMatch.hasMatch)
    {
        return reverseMatch.returnType;
    }
    return {};
}

/**
 * @brief Resolves fallback implicit conversion for binary expression with one non-primitive operand.
 * @param[in] op Operator symbol.
 * @param[in] cleanLeft Left clean base type.
 * @param[in] cleanRight Right clean base type.
 * @param[in] ctx Expression typing context.
 * @return Promoted return type or empty string if no single conversion exists.
 */
static std::string ResolveBinaryFallbackConv(std::string_view op, const std::string& cleanLeft,
                                             const std::string& cleanRight, const ExpressionTypeContext& ctx)
{
    const bool leftIsPrim = parser::primitives::IsPrimitive(cleanLeft);
    const bool rightIsPrim = parser::primitives::IsPrimitive(cleanRight);
    if (!leftIsPrim && rightIsPrim)
    {
        if (auto conv = FindSingleOpImplConvPrimitive(cleanLeft, ctx.symbolTable); !conv.empty())
        {
            return ResolveBinaryPrimitivePromotion(op, conv, cleanRight, ctx.disableIntegerDivision, &ctx.symbolTable);
        }
    }
    else if (leftIsPrim && !rightIsPrim)
    {
        if (auto conv = FindSingleOpImplConvPrimitive(cleanRight, ctx.symbolTable); !conv.empty())
        {
            return ResolveBinaryPrimitivePromotion(op, cleanLeft, conv, ctx.disableIntegerDivision, &ctx.symbolTable);
        }
    }
    return {};
}

/**
 * @brief Resolves string concatenation result type if applicable.
 * @param[in] op Operator symbol.
 * @param[in] leftType Left operand type.
 * @param[in] rightType Right operand type.
 * @param[in] ctx Expression typing context.
 * @return Effective string type name if concatenation, or empty string.
 */
static std::string ResolveStringConcatenation(std::string_view op, const std::string& leftType,
                                              const std::string& rightType, const ExpressionTypeContext& ctx)
{
    if (op != "+")
    {
        return {};
    }
    const std::string_view effectiveStr =
        ctx.stringTypeName.empty() ? ctx.symbolTable.GetStringType() : ctx.stringTypeName;
    if (CleanBaseType(leftType, ctx.arrayTypeName) == effectiveStr ||
        CleanBaseType(rightType, ctx.arrayTypeName) == effectiveStr)
    {
        return std::string(effectiveStr);
    }
    return {};
}

/**
 * @brief Resolves user-defined operator overloads for a binary expression.
 * @param[in] op Operator string.
 * @param[in] leftType Left operand type.
 * @param[in] rightType Right operand type.
 * @param[in] symbolTable Symbol table for member lookup.
 * @return Resolved return type, or empty string if no overload matches.
 */
static std::string ResolveBinaryOverload(std::string_view op, const std::string& leftType, const std::string& rightType,
                                         const SymbolTable& symbolTable)
{
    const std::string cleanLeft = CleanBaseType(leftType);
    const std::string cleanRight = CleanBaseType(rightType);
    const bool leftIsPrim = parser::primitives::IsPrimitive(cleanLeft);
    const bool rightIsPrim = parser::primitives::IsPrimitive(cleanRight);

    auto [opMethod, revOpMethod] = GetBinaryOpMethods(op);

    BinaryOverloadResult forwardMatch;
    if (!leftIsPrim)
    {
        forwardMatch = ResolveBinaryOverloadMatch(cleanLeft, opMethod, rightType, symbolTable);
    }

    BinaryOverloadResult reverseMatch;
    if (!rightIsPrim)
    {
        reverseMatch = ResolveBinaryOverloadMatch(cleanRight, revOpMethod, leftType, symbolTable);
    }

    return SelectBestBinaryOverload(forwardMatch, reverseMatch);
}

/**
 * @brief Resolves binary expression type.
 * @param[in] exprNode Binary expression AST node.
 * @param[in] ctx Expression typing context.
 * @param[in] depth Current recursion depth.
 * @return Resolved expression type string or poison type.
 */
static std::string ResolveBinaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode left = parser::GetChildByField(exprNode, gs.fieldLeft);
    TSNode opNode = parser::GetChildByField(exprNode, gs.fieldOperator);
    TSNode right = parser::GetChildByField(exprNode, gs.fieldRight);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }

    std::string leftType = ResolveExpressionType(left, ctx, depth + 1);
    std::string rightType = ResolveExpressionType(right, ctx, depth + 1);
    if (ExpressionTypeCache::IsPoisonType(leftType) || ExpressionTypeCache::IsPoisonType(rightType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }

    const TSSymbol opSym = ts_node_symbol(opNode);
    if (IsBooleanBinaryOpSym(opSym, gs))
    {
        return "bool";
    }
    const std::string_view op = GetNodeTextView(opNode, ctx.sourceCode);
    if (std::string strType = ResolveStringConcatenation(op, leftType, rightType, ctx); !strType.empty())
    {
        return strType;
    }

    std::string cleanLeft = CleanBaseType(leftType);
    std::string cleanRight = CleanBaseType(rightType);

    if (parser::primitives::IsPrimitive(cleanLeft) && parser::primitives::IsPrimitive(cleanRight))
    {
        return ResolveBinaryPrimitivePromotion(op, cleanLeft, cleanRight, ctx.disableIntegerDivision, &ctx.symbolTable);
    }

    if (std::string overloadType = ResolveBinaryOverload(op, leftType, rightType, ctx.symbolTable);
        !overloadType.empty())
    {
        return overloadType;
    }

    if (std::string fallbackType = ResolveBinaryFallbackConv(op, cleanLeft, cleanRight, ctx); !fallbackType.empty())
    {
        return fallbackType;
    }

    return ResolveBinaryPrimitivePromotion(op, cleanLeft, cleanRight, ctx.disableIntegerDivision, &ctx.symbolTable);
}

struct TernaryBranchTypes
{
    std::string t1;
    std::string t2;
    std::string c1;
    std::string c2;
};

/**
 * @brief Resolves ternary branches using inheritance hierarchies.
 */
static std::string ResolveTernaryHierarchy(const TernaryBranchTypes& types, const SymbolTable& symbolTable)
{
    auto h1 = GetInheritedTypeHierarchy(types.c1, symbolTable);
    for (const auto& b : h1)
    {
        if (CanonicalizeType(CleanBaseType(b)) == types.c2)
        {
            return types.t2;
        }
    }
    auto h2 = GetInheritedTypeHierarchy(types.c2, symbolTable);
    for (const auto& b : h2)
    {
        if (CanonicalizeType(CleanBaseType(b)) == types.c1)
        {
            return types.t1;
        }
    }
    if (HasConversionMethod(types.c1, types.c2, symbolTable))
    {
        return types.t2;
    }
    if (HasConversionMethod(types.c2, types.c1, symbolTable))
    {
        return types.t1;
    }
    return "";
}

static std::string ResolveTernaryNumericPromotion(const std::string& c1, const std::string& c2)
{
    if (c1 == "double" || c2 == "double")
        return "double";
    if (c1 == "float" || c2 == "float")
        return "float";
    if (c1 == "int64" || c2 == "int64")
        return "int64";
    if (c1 == "uint64" || c2 == "uint64")
        return "uint64";
    if (c1 == "uint" || c2 == "uint")
        return "uint";
    return "int";
}

static std::string ResolveTernaryEnumPromotion(const std::string& c1, const std::string& c2,
                                               const SymbolTable& symbolTable)
{
    if (ResolvesToEnum(c1, symbolTable) && IsIntegerPrimitive(c2))
        return c2;
    if (ResolvesToEnum(c2, symbolTable) && IsIntegerPrimitive(c1))
        return c1;
    return "";
}

/**
 * @brief Resolves ternary expression type when one or both branches are null.
 * @param[in] t1 Consequence type.
 * @param[in] t2 Alternative type.
 * @param[in] c1 Cleaned consequence base type.
 * @param[in] c2 Cleaned alternative base type.
 * @return Resolved handle type or "null", or empty string if not applicable.
 */
static std::string ResolveTernaryNullBranch(const std::string& t1, const std::string& t2, const std::string& c1,
                                            const std::string& c2)
{
    if (c1 == "null" && t2.ends_with("@"))
    {
        return t2;
    }
    if (c2 == "null" && t1.ends_with("@"))
    {
        return t1;
    }
    return (c1 == "null" && c2 == "null") ? "null" : "";
}

/**
 * @brief Resolves result type between ternary consequence and alternative branch types.
 */
static std::string ResolveTernaryBranchTypes(std::string_view t1, std::string_view t2, const SymbolTable& symbolTable)
{
    std::string clean1 = CleanExpressionType(t1);
    std::string clean2 = CleanExpressionType(t2);
    if (!clean1.empty() && clean1 == clean2)
    {
        return clean1;
    }

    std::string c1 = CanonicalizeType(CleanBaseType(t1));
    std::string c2 = CanonicalizeType(CleanBaseType(t2));
    if (!c1.empty() && c1 == c2)
    {
        return (t1.ends_with("@") && t2.ends_with("@")) ? c1 + "@" : c1;
    }
    if (auto nullRes = ResolveTernaryNullBranch(clean1, clean2, c1, c2); !nullRes.empty())
    {
        return nullRes;
    }
    if (IsNumericPrimitive(c1) && IsNumericPrimitive(c2))
    {
        return ResolveTernaryNumericPromotion(c1, c2);
    }
    if (auto enumRes = ResolveTernaryEnumPromotion(c1, c2, symbolTable); !enumRes.empty())
    {
        return enumRes;
    }
    return ResolveTernaryHierarchy(TernaryBranchTypes{std::string(t1), std::string(t2), c1, c2}, symbolTable);
}

/**
 * @brief Resolves ternary expression type.
 */
static std::string ResolveTernaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    TSNode consequence = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldConsequence);
    TSNode alternative = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldAlternative);
    if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    std::string t1 = ResolveExpressionType(consequence, ctx, depth + 1);
    std::string t2 = ResolveExpressionType(alternative, ctx, depth + 1);
    if (ExpressionTypeCache::IsPoisonType(t1) || ExpressionTypeCache::IsPoisonType(t2))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (t1.empty())
        return t2;
    if (t2.empty())
        return t1;
    if (t1 == "void" || t2 == "void")
        return "";

    return ResolveTernaryBranchTypes(t1, t2, ctx.symbolTable);
}

/**
 * @brief Builds the property search order for member expressions, prioritizing non-mixins.
 */
static std::vector<std::string> OrderMemberPropertySearchHierarchy(const std::string& cleanObj,
                                                                   const SymbolTable& symbolTable)
{
    auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
    if (hierarchy.empty() && !cleanObj.empty())
    {
        hierarchy.push_back(cleanObj);
    }
    std::vector<std::string> propSearchOrder;
    if (!hierarchy.empty())
    {
        propSearchOrder.push_back(hierarchy[0]);
        for (size_t i = 1; i < hierarchy.size(); ++i)
        {
            if (!IsMixinClass(hierarchy[i], symbolTable))
            {
                propSearchOrder.push_back(hierarchy[i]);
            }
        }
        for (size_t i = 1; i < hierarchy.size(); ++i)
        {
            if (IsMixinClass(hierarchy[i], symbolTable))
            {
                propSearchOrder.push_back(hierarchy[i]);
            }
        }
    }
    return propSearchOrder;
}

/**
 * @brief Checks if a symbol is an instance member rather than an enum constant.
 */
static bool IsNonEnumSymbol(const Symbol& s)
{
    if (s.type == SymbolType::Variable && std::holds_alternative<VariableSignature>(s.signature))
    {
        return !s.GetVariable().isEnumConstant;
    }
    return true;
}

/**
 * @brief Finds candidate member symbols in a specific type.
 */
static std::vector<Symbol> FindMemberSymbolsInType(const std::string& typeName, const std::string& memName,
                                                   const SymbolTable& symbolTable)
{
    auto found = symbolTable.FindMemberSymbols(typeName, memName);
    const bool hasInstance = std::any_of(found.begin(), found.end(), IsNonEnumSymbol);
    if (!hasInstance)
    {
        auto getters = symbolTable.FindMemberSymbols(typeName, "get_" + memName);
        auto setters = symbolTable.FindMemberSymbols(typeName, "set_" + memName);
        if (!getters.empty() || !setters.empty())
        {
            found.insert(found.begin(), setters.begin(), setters.end());
            found.insert(found.begin(), getters.begin(), getters.end());
        }
    }
    if (found.empty())
    {
        auto allSymbols = symbolTable.FindSymbols(memName);
        for (const auto& sSym : allSymbols)
        {
            if (sSym.containerName == typeName)
            {
                found.push_back(sSym);
            }
        }
    }
    std::stable_partition(found.begin(), found.end(), IsNonEnumSymbol);
    return found;
}

/**
 * @brief Retrieves node text with leading and trailing whitespace trimmed.
 * @param[in] node Tree-sitter node to extract text from.
 * @param[in] sourceCode Source buffer text.
 * @return Trimmed node text.
 */
static std::string GetTrimmedNodeText(TSNode node, std::string_view sourceCode)
{
    std::string text = GetNodeText(node, sourceCode);
    while (!text.empty() && isspace(static_cast<unsigned char>(text.front())))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    return text;
}

/**
 * @brief Extracts the resolved type of an accessor function member.
 * @param[in] sym Accessor function symbol to inspect.
 * @return Cleaned property type, or empty string if indexed or invalid.
 */
static std::string ExtractAccessorMemberType(const Symbol& sym)
{
    if (!std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return "";
    }
    const auto& fn = sym.GetFunction();
    const bool isGet = sym.name.starts_with("get_");
    const bool isSet = sym.name.starts_with("set_");
    if (!isGet && !isSet && !fn.modifiers.isProperty)
    {
        return "";
    }
    const bool isIndexed = (isGet && !fn.parameters.empty()) || (isSet && fn.parameters.size() > 1);
    if (isIndexed)
    {
        return "";
    }
    if (!fn.returnType.empty() && fn.returnType != "void")
    {
        return CleanExpressionType(fn.returnType);
    }
    if (!fn.parameters.empty())
    {
        return CleanExpressionType(fn.parameters.back().typeName);
    }
    return "";
}

/**
 * @brief Extracts the resolved type of a member symbol.
 * @param[in] sym Member symbol to inspect.
 * @return Cleaned member type, or empty string if unresolved.
 */
static std::string ExtractMemberSymbolType(const Symbol& sym, const SymbolTable& table)
{
    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
    {
        std::string type = sym.GetVariable().typeName;
        if (!sym.containerName.empty())
        {
            type = QualifyTypeInContainer(type, sym.containerName, table);
        }
        return CleanExpressionType(type);
    }
    if (sym.type == SymbolType::Function)
    {
        std::string type = ExtractAccessorMemberType(sym);
        if (!sym.containerName.empty())
        {
            type = QualifyTypeInContainer(type, sym.containerName, table);
        }
        return type;
    }
    return "";
}

/**
 * @brief Resolves member expression type.
 * @param[in] exprNode Member expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] depth Current AST recursion depth.
 * @return Resolved member type, or empty string if unresolved.
 */
static std::string ResolveMemberExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    TSNode objNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }

    std::string objType = ResolveExpressionType(objNode, ctx, depth + 1);
    if (ExpressionTypeCache::IsPoisonType(objType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty())
    {
        cleanObj = GetTrimmedNodeText(objNode, ctx.sourceCode);
    }
    if (cleanObj.empty())
    {
        return "";
    }

    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);
    auto propSearchOrder = OrderMemberPropertySearchHierarchy(cleanObj, ctx.symbolTable);
    for (const auto& typeName : propSearchOrder)
    {
        auto found = FindMemberSymbolsInType(typeName, memName, ctx.symbolTable);
        for (const auto& sym : found)
        {
            if (auto resolved = ExtractMemberSymbolType(sym, ctx.symbolTable); !resolved.empty())
            {
                return resolved;
            }
        }
    }
    return "";
}

/**
 * @brief Resolves known container method return types (length, size, isEmpty).
 * @param[in] memName Member name invoked.
 * @param[in] templateInfo Parsed template info of the receiver.
 * @param[in] objType Raw object type name.
 * @return Fallback method return type, or empty string.
 */
static std::string ResolveContainerMethodFallback(std::string_view memName, const TemplateTypeInfo& templateInfo,
                                                  std::string_view objType, std::string_view arrayTypeName)
{
    const std::string_view effectiveArrayName = arrayTypeName;
    if ((templateInfo.containerName == effectiveArrayName && !templateInfo.templateArgs.empty()) ||
        objType.ends_with("[]"))
    {
        if (memName == "length" || memName == "size")
        {
            return "uint";
        }
        if (memName == "isEmpty")
        {
            return "bool";
        }
    }
    return "";
}

/**
 * @brief Resolves return type from candidate method overloads, substituting template arguments.
 * @param[in] candidates Candidate method symbols.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] objType Object receiver type.
 * @param[in] symbolTable Symbol table for template binding.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveCandidateMethodReturn(const std::vector<Symbol>& candidates,
                                                const std::vector<std::string>& argTypes, const std::string& objType,
                                                const SymbolTable& symbolTable)
{
    if (candidates.empty())
    {
        return "";
    }
    auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
    const Symbol* chosen = match.bestCandidate ? match.bestCandidate : &candidates[0];
    if (chosen && std::holds_alternative<FunctionSignature>(chosen->signature))
    {
        std::string ret = chosen->GetFunction().returnType;
        auto binding = BindTemplateArguments(objType, symbolTable);
        if (binding.usable)
        {
            ret = SubstituteTemplateParameters(ret, binding);
        }
        else
        {
            auto templateInfo = ParseTemplateType(objType);
            if (!templateInfo.templateArgs.empty())
            {
                ret = SubstituteTypeParam(ret, "T", templateInfo.templateArgs[0]);
            }
        }
        return CleanExpressionType(ret);
    }
    return CleanExpressionType(candidates[0].GetFunction().returnType);
}
/**
 * @brief Appends candidate function if not already overridden by signature.
 * @param[in,out] candidates Accumulating list of candidate symbols.
 * @param[in] sym Candidate symbol to check and potentially append.
 */
static void AppendCandidateIfUnique(std::vector<Symbol>& candidates, const Symbol& sym)
{
    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        bool overridden = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const Symbol& kept) { return HasSameParameterList(kept, sym); });
        if (!overridden)
        {
            candidates.push_back(sym);
        }
    }
}

/**
 * @brief Collects candidate member methods along the type inheritance hierarchy.
 * @param[in] ownerType Owning type name.
 * @param[in] memName Member function name.
 * @param[in] symbolTable Symbol table to query.
 * @return List of matching function candidate symbols.
 */
static std::vector<Symbol> CollectMemberMethodCandidates(const std::string& ownerType, const std::string& memName,
                                                         const SymbolTable& symbolTable)
{
    std::vector<Symbol> candidates;
    auto hierarchy = GetInheritedTypeHierarchy(ownerType, symbolTable);
    if (hierarchy.empty() && !ownerType.empty())
    {
        hierarchy.emplace_back(ownerType);
    }
    for (const auto& typeName : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbols(typeName, memName);
        for (const auto& sym : found)
        {
            AppendCandidateIfUnique(candidates, sym);
        }
    }
    return candidates;
}

/**
 * @brief Resolves member method call expression type.
 * @param[in] funcNode Function AST node (member_expression).
 * @param[in] argTypes Evaluated argument types.
 * @param[in] ctx Expression type resolution context.
 * @param[in] depth Current AST recursion depth.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveMemberCallExpr(TSNode funcNode, const std::vector<std::string>& argTypes,
                                         const ExpressionTypeContext& ctx, int depth)
{
    TSNode objNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }

    std::string objType = CanonicalizeArrayType(ResolveExpressionType(objNode, ctx, depth + 1), ctx.arrayTypeName);
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);

    std::string ownerType = MemberOwnerType(objType, ctx.arrayTypeName);
    if (ownerType.empty())
    {
        ownerType = CleanExpressionType(objType);
    }
    if (ownerType.empty())
    {
        ownerType = GetTrimmedNodeText(objNode, ctx.sourceCode);
    }

    auto candidates = CollectMemberMethodCandidates(ownerType, memName, ctx.symbolTable);
    if (!candidates.empty())
    {
        return ResolveCandidateMethodReturn(candidates, argTypes, objType, ctx.symbolTable);
    }

    auto templateInfo = ParseTemplateType(objType);
    return ResolveContainerMethodFallback(memName, templateInfo, objType, ctx.arrayTypeName);
}

/**
 * @brief Appends call candidates from mixins included in a class.
 * @param[in] containerName Enclosing container name.
 * @param[in] funcName Function name being called.
 * @param[in] symbolTable Symbol table to query.
 * @param[in,out] candidates Accumulating candidate symbols.
 */
static void CollectMixinCallCandidates(std::string_view containerName, const std::string& funcName,
                                       const SymbolTable& symbolTable, std::vector<Symbol>& candidates)
{
    auto classSyms = symbolTable.FindSymbols(containerName);
    for (const auto& cs : classSyms)
    {
        if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
        {
            for (const auto& mixin : cs.GetClass().includedMixins)
            {
                auto mixinFound = symbolTable.FindMemberSymbols(mixin, funcName);
                for (const auto& sym : mixinFound)
                {
                    AppendCandidateIfUnique(candidates, sym);
                }
            }
        }
    }
}

/**
 * @brief Collects candidate function symbols from enclosing class/interface hierarchy.
 * @param[in] exprNode Expression AST node inside enclosing container.
 * @param[in] funcName Function name being called.
 * @param[in] ctx Expression type resolution context.
 * @return Vector of candidate function symbols.
 */
static std::vector<Symbol> CollectEnclosingCallCandidates(TSNode exprNode, const std::string& funcName,
                                                          const ExpressionTypeContext& ctx)
{
    std::vector<Symbol> candidates;
    auto containers = GetEnclosingContainers(exprNode, ctx.sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            std::string containerName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
            auto hierarchy = GetInheritedTypeHierarchy(containerName, ctx.symbolTable);
            for (const auto& cls : hierarchy)
            {
                auto found = ctx.symbolTable.FindMemberSymbols(cls, funcName);
                for (const auto& sym : found)
                {
                    AppendCandidateIfUnique(candidates, sym);
                }
            }
            CollectMixinCallCandidates(containerName, funcName, ctx.symbolTable, candidates);
            break;
        }
    }
    return candidates;
}

/**
 * @brief Resolves return type from call candidates given argument types.
 * @param[in] candidates Function overload candidates.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveCallOverloadReturn(const std::vector<Symbol>& candidates,
                                             const std::vector<std::string>& argTypes, const SymbolTable& symbolTable)
{
    if (candidates.empty())
    {
        return "";
    }
    auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
    const Symbol* chosen = match.bestCandidate ? match.bestCandidate : &candidates[0];
    if (chosen && std::holds_alternative<FunctionSignature>(chosen->signature))
    {
        std::string ret = CleanExpressionType(chosen->GetFunction().returnType);
        if (ret.empty() &&
            (chosen->name == chosen->containerName || LastScopeSegment(chosen->containerName) == chosen->name))
        {
            return chosen->containerName;
        }
        return ret;
    }
    std::string ret = CleanExpressionType(candidates[0].GetFunction().returnType);
    if (ret.empty() && (candidates[0].name == candidates[0].containerName ||
                        LastScopeSegment(candidates[0].containerName) == candidates[0].name))
    {
        return candidates[0].containerName;
    }
    return ret;
}

/**
 * @brief Collects function symbols from scope and global table matching funcName.
 * @param[in] funcName Target function name.
 * @param[in] exprNode Call expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in,out] candidates Accumulating candidate symbols.
 */
static void CollectScopeAndGlobalFunctions(const std::string& funcName, TSNode exprNode,
                                           const ExpressionTypeContext& ctx, std::vector<Symbol>& candidates)
{
    std::vector<Symbol> inScope = FindSymbolsInScope(funcName, exprNode, ctx.sourceCode, ctx.symbolTable);
    for (const auto& sym : inScope)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            candidates.push_back(sym);
        }
    }
    if (!candidates.empty())
    {
        return;
    }
    auto globalFound = ctx.symbolTable.FindSymbols(funcName);
    for (const auto& sym : globalFound)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            candidates.push_back(sym);
        }
    }
}

/**
 * @brief Checks if funcName refers to a class or interface type (for constructor-like bare calls).
 * @param[in] funcName Candidate type name.
 * @param[in] symbolTable Symbol table to query.
 * @return Matched type name, or empty string.
 */
static std::string FindClassOrInterfaceName(const std::string& funcName, const SymbolTable& symbolTable)
{
    auto globalFound = symbolTable.FindSymbols(funcName);
    for (const auto& sym : globalFound)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
        {
            if (!sym.qualifiedName.empty())
            {
                return sym.qualifiedName;
            }
            if (!sym.containerName.empty())
            {
                return utils::JoinScopedName(sym.containerName, sym.name);
            }
            return sym.name;
        }
    }
    return "";
}

/**
 * @brief Resolves non-member or bare function calls.
 * @param[in] exprNode Call expression AST node.
 * @param[in] funcNode Callee AST node.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveBareCallExpr(TSNode exprNode, TSNode funcNode, const std::vector<std::string>& argTypes,
                                       const ExpressionTypeContext& ctx)
{
    std::string funcName = GetTrimmedNodeText(funcNode, ctx.sourceCode);
    if (argTypes.size() == 1 && IsCorePrimitive(funcName))
    {
        return funcName;
    }
    std::vector<Symbol> candidates = CollectEnclosingCallCandidates(exprNode, funcName, ctx);
    if (candidates.empty())
    {
        CollectScopeAndGlobalFunctions(funcName, exprNode, ctx, candidates);
    }
    if (!candidates.empty())
    {
        return ResolveCallOverloadReturn(candidates, argTypes, ctx.symbolTable);
    }
    return FindClassOrInterfaceName(funcName, ctx.symbolTable);
}

/**
 * @brief Resolves call expression type.
 */
static std::string ResolveCallExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    TSNode funcNode = parser::GetCallCallee(exprNode);
    if (ts_node_is_null(funcNode))
    {
        return "";
    }

    std::vector<std::string> argTypes;
    TSNode argsNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldArguments);
    if (!ts_node_is_null(argsNode))
    {
        uint32_t count = ts_node_named_child_count(argsNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            argTypes.push_back(ResolveExpressionType(ts_node_named_child(argsNode, i), ctx, depth + 1));
        }
    }

    if (ts_node_symbol(funcNode) == parser::GetGrammarSymbols().symMemberExpression)
    {
        if (auto res = ResolveMemberCallExpr(funcNode, argTypes, ctx, depth); !res.empty())
        {
            return res;
        }
    }
    else
    {
        if (auto res = ResolveBareCallExpr(exprNode, funcNode, argTypes, ctx); !res.empty())
        {
            return res;
        }
    }

    std::string calleeType = ResolveExpressionType(funcNode, ctx, depth + 1);
    std::string cleanCallee = CleanBaseType(calleeType);
    if (!cleanCallee.empty())
    {
        if (auto funcdef = FindFuncdefSymbol(cleanCallee, ctx.symbolTable))
        {
            return CleanExpressionType(funcdef->GetFuncdef().returnType);
        }
    }
    return calleeType;
}

/**
 * @brief Checks if an expression is a function handle target for unary '@'.
 */
static bool IsFunctionHandleTarget(TSNode target, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (ts_node_is_null(target))
    {
        return false;
    }
    std::string targetName = GetTrimmedNodeText(target, ctx.sourceCode);
    if (targetName.empty() || targetName.find('(') != std::string::npos)
    {
        return false;
    }
    if (const auto bucket = ctx.symbolTable.FindSymbolsPtr(targetName))
    {
        for (const auto& candidate : *bucket)
        {
            if (candidate.type == SymbolType::Function)
                return true;
        }
    }
    auto syms = FindSymbolsInScope(targetName, exprNode, ctx.sourceCode, ctx.symbolTable);
    if (syms.empty())
    {
        syms = ctx.symbolTable.FindSymbols(targetName);
    }
    for (const auto& candidate : syms)
    {
        if (candidate.type == SymbolType::Function)
            return true;
    }
    return false;
}

static std::string ResolveUnaryHandle(TSNode operandNode, TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    if (IsFunctionHandleTarget(operandNode, exprNode, ctx))
    {
        return {};
    }
    std::string operandType = ResolveExpressionType(operandNode, ctx, depth + 1);
    if (ExpressionTypeCache::IsPoisonType(operandType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (operandType.empty())
    {
        return {};
    }
    std::string clean = CleanExpressionType(operandType);
    return clean.ends_with("@") ? clean : clean + "@";
}

static std::string ResolveUnaryMinus(const std::string& operandType)
{
    std::string clean = CleanExpressionType(operandType);
    if (clean == "uint" || clean == "uint32")
    {
        return "int";
    }
    if (clean == "uint8")
    {
        return "int8";
    }
    if (clean == "uint16")
    {
        return "int16";
    }
    if (clean == "uint64")
    {
        return "int64";
    }
    return operandType;
}

/**
 * @brief Resolves unary expression type.
 */
static std::string ResolveUnaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    TSNode operandNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldOperand);
    if (ts_node_is_null(operandNode) || ts_node_is_error(operandNode) || ts_node_is_missing(operandNode))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode operatorNode = parser::GetChildByField(exprNode, gs.fieldOperator);
    const TSSymbol opSym = !ts_node_is_null(operatorNode) ? ts_node_symbol(operatorNode) : 0;
    if (opSym == gs.tokBang || opSym == gs.tokNot)
    {
        return "bool";
    }
    if (opSym == gs.tokAt)
    {
        return ResolveUnaryHandle(operandNode, exprNode, ctx, depth);
    }
    std::string operandType = ResolveExpressionType(operandNode, ctx, depth + 1);
    if (ExpressionTypeCache::IsPoisonType(operandType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (opSym == gs.tokDash)
    {
        return ResolveUnaryMinus(operandType);
    }
    return operandType;
}

/**
 * @brief Resolves constructor call expression type.
 */
static std::string ResolveConstructCallExpr(TSNode exprNode, std::string_view sourceCode)
{
    TSNode typeNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldType);
    if (ts_node_is_null(typeNode))
    {
        return "";
    }
    std::string written = GetNodeText(typeNode, sourceCode);
    const uint32_t childCount = ts_node_named_child_count(exprNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_named_child(exprNode, i);
        if (ts_node_symbol(child) == parser::GetGrammarSymbols().symTemplateTypeList)
        {
            written += GetNodeText(child, sourceCode);
            break;
        }
    }
    return CleanExpressionType(written);
}

/**
 * @brief Resolves cast or constructor call expression types.
 * @param[in] nodeType Tree-sitter AST node type string.
 * @param[in] exprNode Expression AST node.
 * @param[in] sourceCode Document source text.
 * @return Resolved expression type string if handled, std::nullopt otherwise.
 */
static std::optional<std::string> ResolveCastOrConstructExpr(std::string_view nodeType, TSNode exprNode,
                                                             std::string_view sourceCode)
{
    if (nodeType == "cast_expression" || nodeType == "functional_cast_expression")
    {
        TSNode typeNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldType);
        return ts_node_is_null(typeNode) ? "" : CleanExpressionType(GetNodeText(typeNode, sourceCode));
    }
    if (nodeType == "construct_call_expression")
    {
        return ResolveConstructCallExpr(exprNode, sourceCode);
    }
    return std::nullopt;
}

/**
 * @brief Resolves initializer list or lambda expression types.
 * @param[in] nodeType Tree-sitter AST node type string.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved expression type string if handled, std::nullopt otherwise.
 */
static std::optional<std::string> ResolveLambdaOrInitExpr(std::string_view nodeType, TSNode exprNode,
                                                          const ExpressionTypeContext& ctx)
{
    if (nodeType == "initializer_list")
    {
        return "init_list";
    }
    if (nodeType == "lambda_expression")
    {
        if (auto target = FuncdefTargetOfLambda(exprNode, ctx.symbolTable, ctx.sourceCode))
        {
            return target->qualifiedName.empty() ? target->name : target->qualifiedName;
        }
        return "";
    }
    return std::nullopt;
}

/**
 * @brief Searches a collection of accessor symbols for an indexed getter return type.
 * @param[in] accessors Vector of accessor symbols.
 * @param[in] symbolTable Global symbol table pointer for overload resolution.
 * @param[in] indexArgType Deduced type of the index argument.
 * @return Return type of the indexed getter, or empty string.
 */
static std::string FindIndexedGetterType(const std::vector<Symbol>& accessors, const SymbolTable* symbolTable = nullptr,
                                         std::string_view indexArgType = "")
{
    std::vector<Symbol> getters;
    for (const auto& sym : accessors)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto& fn = sym.GetFunction();
            if (sym.name.starts_with("get_") && !fn.parameters.empty())
            {
                getters.push_back(sym);
            }
        }
    }
    if (getters.empty())
    {
        return "";
    }
    if (symbolTable && !indexArgType.empty())
    {
        auto match = ResolveBestOverload(getters, {std::string(indexArgType)}, *symbolTable);
        if (match.bestCandidate)
        {
            return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
        }
    }
    return CleanExpressionType(getters.front().GetFunction().returnType);
}

/**
 * @brief Resolves indexed property accessor type from member expression receiver.
 * @param[in] objNode Member expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] depth AST recursion depth.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
static std::string ResolveMemberIndexedProperty(TSNode objNode, const ExpressionTypeContext& ctx, int depth,
                                                std::string_view indexArgType = "")
{
    TSNode recNode = parser::GetChildByField(objNode, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(objNode, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(recNode) || ts_node_is_null(memNode))
    {
        return "";
    }
    std::string recType = CleanBaseType(ResolveExpressionType(recNode, ctx, depth + 1));
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);
    if (recType.empty() || memName.empty())
    {
        return "";
    }
    return FindIndexedGetterType(FindPropertyAccessors(recType, memName, ctx.symbolTable, false), &ctx.symbolTable,
                                 indexArgType);
}

/**
 * @brief Resolves indexed property accessor type from scoped or bare identifier.
 * @param[in] objNode Scoped identifier or identifier AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
static std::string ResolveScopedOrBareIndexedProperty(TSNode objNode, const ExpressionTypeContext& ctx,
                                                      std::string_view indexArgType = "")
{
    const auto& gs = parser::GetGrammarSymbols();
    TSSymbol objSym = ts_node_symbol(objNode);
    if (objSym == gs.symScopedIdentifier)
    {
        std::string_view text = GetNodeTextView(objNode, ctx.sourceCode);
        const auto [scopeName, name] = utils::SplitScopedName(text);
        if (!scopeName.empty())
        {
            return FindIndexedGetterType(
                FindPropertyAccessors(std::string(scopeName), std::string(name), ctx.symbolTable, false),
                &ctx.symbolTable, indexArgType);
        }
        return "";
    }

    std::string_view name = GetNodeTextView(objNode, ctx.sourceCode);
    if (name.empty())
    {
        return "";
    }
    if (std::string hit = FindIndexedGetterType(FindGlobalPropertyAccessors(std::string(name), ctx.symbolTable, false),
                                                &ctx.symbolTable, indexArgType);
        !hit.empty())
    {
        return hit;
    }
    if (!ctx.uri.empty())
    {
        std::string enclosing = ctx.symbolTable.FindEnclosingClassName(ctx.uri, ts_node_start_point(objNode).row);
        if (!enclosing.empty())
        {
            if (std::string hit =
                    FindIndexedGetterType(FindPropertyAccessors(enclosing, std::string(name), ctx.symbolTable, false),
                                          &ctx.symbolTable, indexArgType);
                !hit.empty())
            {
                return hit;
            }
        }
    }
    return "";
}

/**
 * @brief Resolves index expression types for containers and indexed property accessors.
 * @param[in] exprNode Index expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] depth AST recursion depth.
 * @return Resolved indexed expression type, or empty string.
 */
static std::string ResolveIndexExpr(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode objNode = ts_node_child_by_field_id(exprNode, gs.fieldObject);
    if (ts_node_is_null(objNode))
    {
        return "";
    }

    TSNode indexNode = ts_node_child_by_field_id(exprNode, gs.fieldIndex);
    std::string indexArgType;
    if (!ts_node_is_null(indexNode))
    {
        indexArgType = ResolveExpressionType(indexNode, ctx, depth + 1);
    }

    if (ts_node_symbol(objNode) == gs.symMemberAccess)
    {
        if (std::string propType = ResolveMemberIndexedProperty(objNode, ctx, depth, indexArgType); !propType.empty())
        {
            return propType;
        }
    }
    else
    {
        if (std::string propType = ResolveScopedOrBareIndexedProperty(objNode, ctx, indexArgType); !propType.empty())
        {
            return propType;
        }
    }

    std::string objType = ResolveExpressionType(objNode, ctx, depth + 1);
    if (objType.empty())
    {
        return "";
    }
    return ResolveIndexedTypeWithArg(objType, ctx.symbolTable, ctx.arrayTypeName, indexArgType);
}

/**
 * @brief Resolves miscellaneous and secondary expression types.
 */
static std::string ResolveOtherExpr(std::string_view nodeType, TSNode exprNode, const ExpressionTypeContext& ctx,
                                    int depth)
{
    if (auto castOrConstruct = ResolveCastOrConstructExpr(nodeType, exprNode, ctx.sourceCode))
    {
        return *castOrConstruct;
    }
    if (auto lambdaOrInit = ResolveLambdaOrInitExpr(nodeType, exprNode, ctx))
    {
        return *lambdaOrInit;
    }
    if (nodeType == "assignment_expression")
    {
        TSNode left = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldLeft);
        return ts_node_is_null(left) ? "" : ResolveExpressionType(left, ctx, depth + 1);
    }
    if (nodeType == "index_expression")
    {
        return ResolveIndexExpr(exprNode, ctx, depth);
    }
    if (nodeType == "unary_expression")
    {
        return ResolveUnaryExpr(exprNode, ctx, depth);
    }
    if (nodeType == "postfix_expression")
    {
        TSNode operandNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldOperand);
        return ts_node_is_null(operandNode) ? "" : ResolveExpressionType(operandNode, ctx, depth + 1);
    }
    return "";
}

/**
 * @brief Resolves primary/atomic expression types (parentheses, this, literals, identifiers).
 * @param[in] nodeType Tree-sitter AST node type string.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] depth Current AST recursion depth.
 * @return Optional containing resolved type if nodeType is primary, or std::nullopt.
 */
static std::optional<std::string> ResolvePrimaryExpr(std::string_view nodeType, TSNode exprNode,
                                                     const ExpressionTypeContext& ctx, int depth)
{
    if (nodeType == "parenthesized_expression")
    {
        return ResolveParenthesizedExpr(exprNode, ctx, depth);
    }
    if (nodeType == "typed_initializer_list")
    {
        TSNode typeNode = parser::GetChildByField(exprNode, parser::GetGrammarSymbols().fieldType);
        if (!ts_node_is_null(typeNode))
        {
            return GetNodeText(typeNode, ctx.sourceCode);
        }
    }
    if (nodeType == "this_expression")
    {
        return ResolveThisExpr(exprNode, ctx);
    }
    if (auto lit = ResolveLiteralExpr(ts_node_symbol(exprNode), exprNode, ctx.sourceCode, ctx.stringTypeName);
        !lit.empty())
    {
        return lit;
    }
    if (nodeType == "scoped_identifier")
    {
        return ResolveScopedIdentifierExpr(exprNode, ctx, depth);
    }
    if (nodeType == "identifier" || nodeType == "function" || parser::keywords::IsKeyword(nodeType))
    {
        return ResolveIdentifierExpr(exprNode, ctx, depth);
    }
    return std::nullopt;
}

/**
 * @brief Resolves composite expression types (binary, ternary, member, call).
 * @param[in] nodeType Tree-sitter AST node type string.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] depth Current AST recursion depth.
 * @return Resolved type string, or empty string.
 */
static std::string ResolveCompositeExpr(std::string_view nodeType, TSNode exprNode, const ExpressionTypeContext& ctx,
                                        int depth)
{
    if (nodeType == "binary_expression")
    {
        return ResolveBinaryExpr(exprNode, ctx, depth);
    }
    if (nodeType == "ternary_expression")
    {
        return ResolveTernaryExpr(exprNode, ctx, depth);
    }
    if (nodeType == "member_expression")
    {
        return ResolveMemberExpr(exprNode, ctx, depth);
    }
    if (nodeType == "call_expression")
    {
        return ResolveCallExpr(exprNode, ctx, depth);
    }
    return ResolveOtherExpr(nodeType, exprNode, ctx, depth);
}

static std::string ResolveExpressionTypeUncached(std::string_view nodeType, TSNode exprNode,
                                                 const ExpressionTypeContext& ctx, int depth)
{
    if (auto primary = ResolvePrimaryExpr(nodeType, exprNode, ctx, depth))
    {
        return *primary;
    }
    return ResolveCompositeExpr(nodeType, exprNode, ctx, depth);
}

static std::string_view GetTrimmedNodeTextView(TSNode node, std::string_view sourceCode) noexcept
{
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return {};
    }
    std::string_view sv = sourceCode.substr(start, end - start);
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.front())))
    {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.back())))
    {
        sv.remove_suffix(1);
    }
    return sv;
}

std::string ResolveExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx, int depth)
{
    // This resolver recurses on operands and member chains with nothing else bounding it, so a
    // deeply nested expression walked the stack down until it ran out. Returning empty is the
    // established "cannot see enough to judge" answer everywhere in this file, and every caller
    // already treats it as "stay silent" - so a truncated resolution costs a diagnostic, never
    // a wrong one. See k_maxAstDepth in ASTUtils.h.
    if (depth > k_maxAstDepth || ts_node_is_null(exprNode) || ctx.sourceCode.empty())
    {
        return "";
    }
    if (ts_node_is_error(exprNode) || ts_node_is_missing(exprNode) ||
        (ctx.nodeIndex && ctx.nodeIndex->NodeHasSyntaxError(exprNode)))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }

    if (ctx.cache)
    {
        const ExpressionCacheKey key{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode),
                                     ts_node_symbol(exprNode)};
        auto it = ctx.cache->types.find(key);
        if (it != ctx.cache->types.end())
        {
            return it->second;
        }

        std::string_view rawText = GetTrimmedNodeTextView(exprNode, ctx.sourceCode);
        if (rawText == "void")
        {
            return "void";
        }

        const std::string_view nodeType = parser::GetNodeType(exprNode);
        std::string resolved = ResolveExpressionTypeUncached(nodeType, exprNode, ctx, depth);
        ctx.cache->types.emplace(key, resolved);
        return resolved;
    }

    std::string_view rawText = GetTrimmedNodeTextView(exprNode, ctx.sourceCode);
    if (rawText == "void")
    {
        return "void";
    }

    const std::string_view nodeType = parser::GetNodeType(exprNode);
    return ResolveExpressionTypeUncached(nodeType, exprNode, ctx, depth);
}

struct ActiveLocalDefKey
{
    std::string_view name;
    uint32_t line = 0;
    uint32_t col = 0;
};

thread_local std::vector<ActiveLocalDefKey> s_activeLocalDefs;

struct ActiveLocalDefGuard
{
    bool pushed = false;
    ~ActiveLocalDefGuard()
    {
        if (pushed && !s_activeLocalDefs.empty())
        {
            s_activeLocalDefs.pop_back();
        }
    }
};

static bool IsLocalDefActive(const LocalDefinition& def) noexcept
{
    return std::any_of(s_activeLocalDefs.begin(), s_activeLocalDefs.end(), [&](const ActiveLocalDefKey& k)
                       { return k.line == def.startLine && k.col == def.startCharacter && k.name == def.name; });
}

static TSNode FindLocalDefinitionValueNode(const LocalDefinition& def, const ExpressionTypeContext& ctx)
{
    TSNode root = !ts_node_is_null(ctx.rootNode) ? ctx.rootNode : (ctx.nodeIndex ? ctx.nodeIndex->Root() : TSNode{});
    if (!ts_node_is_null(root))
    {
        TSNode idNode = (def.startByte != 0 || def.endByte != 0)
                            ? ts_node_descendant_for_byte_range(root, def.startByte, def.endByte)
                            : ts_node_descendant_for_point_range(root, TSPoint{def.startLine, def.startCharacter},
                                                                 TSPoint{def.endLine, def.endCharacter});
        TSNode decl = idNode;
        for (int i = 0; i < 4 && !ts_node_is_null(decl); ++i)
        {
            if (ts_node_symbol(decl) == parser::GetGrammarSymbols().symVariableDeclarator)
            {
                return FindDeclaratorValueNode(decl, ctx.sourceCode);
            }
            decl = parser::GetParent(decl);
        }
    }
    return {};
}

static std::string FindFallbackSymbolType(std::string_view name, const SymbolTable& table)
{
    if (const auto syms = table.FindSymbolsPtr(name))
    {
        for (const auto& sym : *syms)
        {
            if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                std::holds_alternative<VariableSignature>(sym.signature))
            {
                const auto& var = sym.GetVariable();
                if (!var.typeName.empty())
                {
                    return var.typeName;
                }
            }
        }
    }
    return "";
}

static bool IsValidDeducedRhsType(std::string_view rhsType)
{
    return !rhsType.empty() && rhsType != "void" && rhsType != "null" && rhsType != "init_list" &&
           !ExpressionTypeCache::IsPoisonType(rhsType);
}

static std::string FormatDeducedType(const LocalDefinition& def, std::string rhsType)
{
    if ((def.isHandleType || def.typeName.find('@') != std::string::npos) && !rhsType.ends_with('@'))
    {
        rhsType += '@';
    }
    return rhsType;
}

static std::string TryDeduceAutoDefinition(const LocalDefinition& def, const ExpressionTypeContext& ctx, int depth)
{
    TSNode valNode = FindLocalDefinitionValueNode(def, ctx);
    if (ts_node_is_null(valNode))
    {
        return def.typeName;
    }
    std::string rhsType = ResolveExpressionType(valNode, ctx, depth + 1);
    if (!IsValidDeducedRhsType(rhsType))
    {
        return def.typeName;
    }
    std::string finalType = FormatDeducedType(def, std::move(rhsType));
    const_cast<LocalDefinition&>(def).typeName = finalType;
    return finalType;
}

std::string ResolveLocalDefinitionType(const LocalDefinition& def, const ExpressionTypeContext& ctx, int depth)
{
    if (!def.typeName.empty() && CleanBaseTypeView(def.typeName) != "auto")
    {
        return def.typeName;
    }

    if (def.typeName.empty())
    {
        std::string fallback = FindFallbackSymbolType(def.name, ctx.symbolTable);
        if (!fallback.empty())
        {
            const_cast<LocalDefinition&>(def).typeName = fallback;
            return fallback;
        }
    }

    if (CleanBaseTypeView(def.typeName) != "auto" && def.typeKind != TypeKind::Auto)
    {
        return def.typeName;
    }

    if (depth > 8 || IsLocalDefActive(def))
    {
        return "";
    }

    s_activeLocalDefs.push_back({def.name, def.startLine, def.startCharacter});
    ActiveLocalDefGuard guard{true};

    return TryDeduceAutoDefinition(def, ctx, depth);
}

/**
 * @brief Extracts the member owner type from a variable or function symbol.
 */
static std::string ExtractMemberSymbolOwner(const Symbol& sym, std::string_view arrayTypeName)
{
    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
        std::holds_alternative<VariableSignature>(sym.signature))
    {
        const auto& var = sym.GetVariable();
        if (!var.typeName.empty())
        {
            return MemberOwnerType(var.typeName, arrayTypeName);
        }
    }
    else if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        const auto& fn = sym.GetFunction();
        if (!fn.returnType.empty() && fn.returnType != "void")
        {
            return MemberOwnerType(fn.returnType, arrayTypeName);
        }
    }
    return "";
}

/**
 * @brief Resolves a member name within a class's inherited type hierarchy.
 */
static std::string ResolveMemberInHierarchy(const std::string& hostClass, const std::string& memberName,
                                            const SymbolTable& symbolTable, std::string_view arrayTypeName)
{
    auto hier = GetInheritedTypeHierarchy(hostClass, symbolTable);
    for (const auto& cls : hier)
    {
        auto memberSyms = symbolTable.FindMemberSymbols(cls, memberName);
        for (const auto& sym : memberSyms)
        {
            if (auto owner = ExtractMemberSymbolOwner(sym, arrayTypeName); !owner.empty())
            {
                return owner;
            }
        }
        auto accessors = FindPropertyAccessors(cls, memberName, symbolTable, false);
        if (!accessors.empty())
        {
            std::string pType = PropertyTypeFromAccessors(accessors);
            if (!pType.empty())
            {
                return MemberOwnerType(pType, arrayTypeName);
            }
        }
    }
    return "";
}

/**
 * @brief Resolves special 'this' receiver expressions.
 */
static std::string ResolveThisReceiver(TSNode objNode, std::string_view sourceCode, std::string_view effectiveHostClass)
{
    if (!effectiveHostClass.empty())
    {
        return std::string(effectiveHostClass);
    }
    auto containers = GetEnclosingContainers(objNode, sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            return c.qualifiedName.empty() ? c.name : c.qualifiedName;
        }
    }
    return "";
}

/**
 * @brief Resolves receiver member in enclosing class or interface containers.
 */
static std::string ResolveReceiverInEnclosingContainers(TSNode objNode, std::string_view sourceCode,
                                                        const std::string& objText, const SymbolTable& symbolTable,
                                                        std::string_view arrayTypeName)
{
    auto containers = GetEnclosingContainers(objNode, sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            std::string enclosing = c.qualifiedName.empty() ? c.name : c.qualifiedName;
            return ResolveMemberInHierarchy(enclosing, objText, symbolTable, arrayTypeName);
        }
    }
    return "";
}

/**
 * @brief Resolves receiver name against global and short-name symbols.
 */
static std::string ResolveReceiverInGlobalOrShort(const std::string& objText, const SymbolTable& symbolTable,
                                                  std::string_view arrayTypeName)
{
    auto globSyms = symbolTable.FindSymbols(objText);
    for (const auto& sym : globSyms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            std::holds_alternative<VariableSignature>(sym.signature))
        {
            const auto& var = sym.GetVariable();
            if (!var.typeName.empty())
            {
                return MemberOwnerType(var.typeName, arrayTypeName);
            }
        }
        else if (sym.type == SymbolType::Class || sym.type == SymbolType::Namespace)
        {
            return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
        }
    }

    auto shortMatches = symbolTable.FindTypeSymbolsByShortName(objText);
    for (const auto& sym : shortMatches)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Namespace)
        {
            return sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
        }
    }
    return "";
}

static std::string GetEffectiveHostClass(std::string_view virtualHostClass, std::string_view fileUri)
{
    if (!virtualHostClass.empty())
    {
        return std::string(virtualHostClass);
    }
    if (fileUri.starts_with("angelscript-virtual:") || fileUri.starts_with("angelscript-virtual://"))
    {
        return SymbolTable::ExtractVirtualHostClass(fileUri);
    }
    return "";
}

static std::string ResolveReceiverScope(const Scope* scope, const std::string& objText, uint32_t refByte = UINT32_MAX,
                                        const ExpressionTypeContext* exprCtx = nullptr,
                                        std::string_view arrayTypeName = {})
{
    if (scope)
    {
        const LocalDefinition* objDef = ResolveInScopeAt(scope, objText, refByte);
        if (objDef)
        {
            if (!objDef->typeName.empty() && CleanBaseTypeView(objDef->typeName) != "auto")
            {
                return MemberOwnerType(objDef->typeName, arrayTypeName);
            }
            if (exprCtx)
            {
                std::string deduced = ResolveLocalDefinitionType(*objDef, *exprCtx);
                if (!deduced.empty())
                {
                    return MemberOwnerType(deduced, arrayTypeName);
                }
            }
            if (!objDef->typeName.empty())
            {
                return MemberOwnerType(objDef->typeName, arrayTypeName);
            }
        }
    }
    return "";
}

std::string ResolveReceiverType(TSNode objNode, std::string_view sourceCode, const SymbolTable& symbolTable,
                                const ReceiverTypeContext& ctx)
{
    if (ts_node_is_null(objNode))
    {
        return "";
    }

    std::string objText = GetNodeText(objNode, sourceCode);
    if (objText.empty())
    {
        return "";
    }

    std::string effectiveHostClass = GetEffectiveHostClass(ctx.virtualHostClass, ctx.fileUri);
    const std::string_view arrayTypeName =
        ctx.arrayTypeName.empty() ? symbolTable.GetDefaultArrayType() : ctx.arrayTypeName;

    if (objText == "this")
    {
        return ResolveThisReceiver(objNode, sourceCode, effectiveHostClass);
    }

    ExpressionTypeContext exprCtx(ctx.scope, symbolTable, sourceCode, ctx.fileUri, parser::GetRootNode(objNode));
    if (auto scopeRes = ResolveReceiverScope(ctx.scope, objText, ts_node_start_byte(objNode), &exprCtx, arrayTypeName);
        !scopeRes.empty())
    {
        return scopeRes;
    }

    if (auto inferred = InferLambdaParamType(objNode, objText, symbolTable, sourceCode); !inferred.empty())
    {
        return inferred;
    }

    if (!effectiveHostClass.empty())
    {
        if (auto res = ResolveMemberInHierarchy(effectiveHostClass, objText, symbolTable, arrayTypeName); !res.empty())
        {
            return res;
        }
    }

    if (auto res = ResolveReceiverInEnclosingContainers(objNode, sourceCode, objText, symbolTable, arrayTypeName);
        !res.empty())
    {
        return res;
    }

    std::string exprType = ResolveExpressionType(objNode, {ctx.scope, symbolTable, sourceCode, ctx.fileUri});
    if (!exprType.empty() && exprType != "void" && exprType != "unknown")
    {
        return MemberOwnerType(exprType, arrayTypeName);
    }

    return ResolveReceiverInGlobalOrShort(objText, symbolTable, arrayTypeName);
}

bool IsVariableType(std::string_view typeName)
{
    // Strip whatever decoration the declaration carried: const, &, @, in/out/inout, spaces.
    // What has to remain is a bare '?' and nothing else.
    std::string cleaned;
    cleaned.reserve(typeName.size());
    for (const char c : typeName)
    {
        if (c == '?')
            cleaned.push_back(c);
        else if (c != ' ' && c != '\t' && c != '&' && c != '@')
        {
            // Any other identifier character means this is a real type, not the wildcard.
            // "const" / "in" / "out" / "inout" are the only words legally adjacent to it.
            cleaned.push_back(c);
        }
    }

    static constexpr std::string_view k_decorations[] = {"const", "inout", "out", "in"};
    for (const auto decoration : k_decorations)
    {
        for (size_t at = cleaned.find(decoration); at != std::string::npos; at = cleaned.find(decoration, at))
            cleaned.erase(at, decoration.size());
    }

    return cleaned == "?";
}

/**
 * @brief Resolves argument_list node from either argument_list or call_expression.
 * @param[in] node AST node.
 * @return argument_list node or null node.
 */
TSNode ResolveArgumentListNode(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symArgumentList)
    {
        return node;
    }
    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression || sym == gs.symVariableDeclarator)
    {
        TSNode args = parser::GetChildByField(node, gs.fieldArguments);
        if (!ts_node_is_null(args))
        {
            return args;
        }
        TSTreeCursor cursor = ts_tree_cursor_new(node);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode ch = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(ch) == gs.symArgumentList)
                {
                    ts_tree_cursor_delete(&cursor);
                    return ch;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
            ts_tree_cursor_delete(&cursor);
        }
    }
    return TSNode{};
}

void ProcessCallArgumentChild(TSNode child, const char* field, std::string_view sourceCode, CallArgumentInfo& current)
{
    if (field && std::string_view(field) == "arg_name")
    {
        current.nameNode = child;
        if (!sourceCode.empty())
        {
            current.name = GetNodeText(child, sourceCode);
        }
        return;
    }

    current.exprNode = child;
}

std::vector<CallArgumentInfo> ExtractCallArguments(TSNode argumentList, std::string_view sourceCode)
{
    TSNode targetList = ResolveArgumentListNode(argumentList);
    if (ts_node_is_null(targetList))
    {
        return {};
    }

    std::vector<CallArgumentInfo> result;
    TSTreeCursor cursor = ts_tree_cursor_new(targetList);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return result;
    }

    CallArgumentInfo current;
    current.index = 0;
    bool inArgument = false;

    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(child);

        if (sym == gs.tokOpenParen || sym == gs.symComment || sym == gs.tokColon)
        {
            continue;
        }
        if (sym == gs.tokCloseParen)
        {
            break;
        }
        if (sym == gs.tokComma)
        {
            if (inArgument)
            {
                result.push_back(std::move(current));
                current = CallArgumentInfo{};
                current.index = static_cast<uint32_t>(result.size());
                inArgument = false;
            }
            continue;
        }

        if (inArgument && !ts_node_is_null(current.exprNode))
        {
            result.push_back(std::move(current));
            current = CallArgumentInfo{};
            current.index = static_cast<uint32_t>(result.size());
        }

        const char* field = ts_tree_cursor_current_field_name(&cursor);
        ProcessCallArgumentChild(child, field, sourceCode, current);
        inArgument = true;

    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);

    if (inArgument)
    {
        result.push_back(std::move(current));
    }

    return result;
}

size_t CountCallArguments(TSNode argumentList)
{
    return ExtractCallArguments(argumentList, "").size();
}

static bool IsConstArgumentNode(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (ts_node_is_null(exprNode))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(exprNode);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return false;
    }
    const std::string name = GetTrimmedNodeText(exprNode, ctx.sourceCode);
    if (ctx.scope)
    {
        if (const auto* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(exprNode)))
        {
            if (!def->typeName.empty() && HasConstModifier(def->typeName))
            {
                return true;
            }
        }
    }
    if (const auto syms = ctx.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& foundSym : *syms)
        {
            if ((foundSym.type == SymbolType::Variable || foundSym.type == SymbolType::Property) &&
                foundSym.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const ExpressionTypeContext& ctx)
{
    auto args = ExtractCallArguments(callNode, ctx.sourceCode);
    std::vector<std::string> types;
    types.reserve(args.size());
    for (const auto& arg : args)
    {
        if (ts_node_is_null(arg.exprNode))
        {
            types.push_back("");
        }
        else
        {
            std::string argType = ResolveExpressionType(arg.exprNode, ctx);
            if (!argType.empty() && !HasConstModifier(argType) && IsConstArgumentNode(arg.exprNode, ctx))
            {
                argType.insert(0, "const ");
            }
            types.push_back(std::move(argType));
        }
    }
    return types;
}

std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, TSNode contextNode,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable)
{
    std::vector<Symbol> candidateSymbols;
    auto ctorSyms = symbolTable.FindMemberSymbols(baseName, LastScopeSegment(baseName));
    for (const auto& s : ctorSyms)
    {
        if (s.type == SymbolType::Function)
        {
            candidateSymbols.push_back(s);
        }
    }

    if (candidateSymbols.empty())
    {
        auto scopeSyms = FindSymbolsInScope(baseName, contextNode, sourceCode, symbolTable);
        for (const auto& s : scopeSyms)
        {
            if (s.type == SymbolType::Class)
            {
                std::string qName = s.qualifiedName.empty() ? s.name : s.qualifiedName;
                auto qCtors = symbolTable.FindMemberSymbols(qName, s.name);
                for (const auto& cs : qCtors)
                {
                    if (cs.type == SymbolType::Function)
                    {
                        candidateSymbols.push_back(cs);
                    }
                }
            }
        }
    }

    if (candidateSymbols.empty())
    {
        auto fnSyms = symbolTable.FindSymbols(baseName);
        for (const auto& s : fnSyms)
        {
            if (s.type == SymbolType::Function)
            {
                candidateSymbols.push_back(s);
            }
        }
    }

    return candidateSymbols;
}

struct CallDelimiterState
{
    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    int angleDepth = 0;
    bool inString = false;
    char stringChar = '\0';
    bool inLineComment = false;
    bool inBlockComment = false;

    [[nodiscard]] bool AtTopLevel() const noexcept
    {
        return parenDepth == 0 && bracketDepth == 0 && braceDepth == 0 && angleDepth == 0;
    }
};

bool StepCommentState(std::string_view code, size_t& i, size_t cursorByte, CallDelimiterState& state)
{
    if (state.inLineComment)
    {
        if (code[i] == '\n')
        {
            state.inLineComment = false;
        }
        return true;
    }

    if (state.inBlockComment)
    {
        if (code[i] == '*' && i + 1 < cursorByte && code[i + 1] == '/')
        {
            state.inBlockComment = false;
            ++i;
        }
        return true;
    }

    if (code[i] == '/' && i + 1 < cursorByte)
    {
        if (code[i + 1] == '/')
        {
            state.inLineComment = true;
            ++i;
            return true;
        }
        if (code[i + 1] == '*')
        {
            state.inBlockComment = true;
            ++i;
            return true;
        }
    }
    return false;
}

bool StepStringState(std::string_view code, size_t& i, size_t cursorByte, CallDelimiterState& state)
{
    if (state.inString)
    {
        if (code[i] == '\\' && i + 1 < cursorByte)
        {
            ++i;
        }
        else if (code[i] == state.stringChar)
        {
            state.inString = false;
        }
        return true;
    }

    if (code[i] == '"' || code[i] == '\'')
    {
        state.inString = true;
        state.stringChar = code[i];
        return true;
    }
    return false;
}

void StepDelimiterDepth(char c, CallDelimiterState& state, uint32_t& activeParam)
{
    switch (c)
    {
    case '(':
        ++state.parenDepth;
        break;
    case ')':
        if (state.parenDepth > 0)
            --state.parenDepth;
        break;
    case '[':
        ++state.bracketDepth;
        break;
    case ']':
        if (state.bracketDepth > 0)
            --state.bracketDepth;
        break;
    case '{':
        ++state.braceDepth;
        break;
    case '}':
        if (state.braceDepth > 0)
            --state.braceDepth;
        break;
    case '<':
        ++state.angleDepth;
        break;
    case '>':
        if (state.angleDepth > 0)
            --state.angleDepth;
        break;
    case ',':
        if (state.AtTopLevel())
        {
            ++activeParam;
        }
        break;
    default:
        break;
    }
}

uint32_t CalculateActiveCallParameter(TSNode argListNode, size_t cursorByte, std::string_view sourceCode)
{
    TSNode targetList = ResolveArgumentListNode(argListNode);
    if (ts_node_is_null(targetList) || sourceCode.empty())
    {
        return 0;
    }

    uint32_t startByte = ts_node_start_byte(targetList);
    uint32_t endByte = ts_node_end_byte(targetList);
    if (cursorByte <= startByte)
    {
        return 0;
    }

    if (startByte < sourceCode.size() && sourceCode[startByte] == '(')
    {
        ++startByte;
    }

    const size_t bound = std::min(cursorByte, static_cast<size_t>(endByte));
    uint32_t activeParam = 0;
    CallDelimiterState state;

    for (size_t i = startByte; i < bound; ++i)
    {
        if (StepCommentState(sourceCode, i, bound, state) || StepStringState(sourceCode, i, bound, state))
        {
            continue;
        }
        StepDelimiterDepth(sourceCode[i], state, activeParam);
    }

    return activeParam;
}

// --- A lambda against the funcdef it is being handed to -------------------------------

std::string LastSegmentOf(const std::string& name)
{
    return std::string(utils::LastScopeSegment(name));
}

/** @brief True for a name that denotes a typedef, whose alias no spelling can see through. */
bool NamesATypedef(const std::string& name, const SymbolTable& table)
{
    const auto bucket = table.FindSymbolsPtr(name);
    if (!bucket)
    {
        return false;
    }
    for (const auto& sym : *bucket)
    {
        if (sym.type == SymbolType::Typedef)
        {
            return true;
        }
    }
    return false;
}

bool IsLambdaExpression(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    return ts_node_symbol(node) == parser::GetGrammarSymbols().symLambdaExpression;
}

/**
 * @brief Resolves lambda_parameter_list node from either parameter list or lambda expression.
 * @param[in] node AST node.
 * @return lambda_parameter_list node or null node.
 */
static TSNode ResolveLambdaParameterListNode(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symLambdaParameterList)
    {
        return node;
    }
    if (sym == gs.symLambdaExpression)
    {
        TSNode params = parser::GetChildByField(node, gs.fieldParameters);
        if (!ts_node_is_null(params))
        {
            return params;
        }
        TSTreeCursor cursor = ts_tree_cursor_new(node);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode ch = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(ch) == gs.symLambdaParameterList)
                {
                    ts_tree_cursor_delete(&cursor);
                    return ch;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
            ts_tree_cursor_delete(&cursor);
        }
    }
    return TSNode{};
}

void ApplyLambdaParamChild(TSNode child, const char* field, std::string_view sourceCode, LambdaParamASTInfo& current)
{
    std::string_view fieldName = field ? std::string_view(field) : std::string_view{};
    std::string text = GetNodeText(child, sourceCode);
    std::string_view type = parser::GetNodeType(child);

    if (fieldName == "param_type")
    {
        current.typeNode = child;
        current.hasWrittenType = true;
        current.isConst = text.starts_with("const ") || text == "const";
        current.isHandle = text.find('@') != std::string::npos;
        current.typeName = CleanBaseType(text);
    }
    else if (text == "&")
    {
        current.isReference = true;
    }
    else if (text == "in" || text == "out" || text == "inout")
    {
        current.modifier = (text == "in")    ? ParameterModifier::In
                           : (text == "out") ? ParameterModifier::Out
                                             : ParameterModifier::InOut;
    }
    else if (fieldName == "name" || type == "identifier")
    {
        current.nameNode = child;
        current.name = std::move(text);
    }
}

std::vector<LambdaParamASTInfo> ExtractLambdaParameters(TSNode lambdaParamList, std::string_view sourceCode)
{
    TSNode targetList = ResolveLambdaParameterListNode(lambdaParamList);
    if (ts_node_is_null(targetList))
    {
        return {};
    }

    std::vector<LambdaParamASTInfo> result;
    TSTreeCursor cursor = ts_tree_cursor_new(targetList);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return result;
    }

    LambdaParamASTInfo current;
    current.index = 0;
    bool inParam = false;

    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(child);

        if (sym == gs.tokOpenParen || sym == gs.symComment)
        {
            continue;
        }
        if (sym == gs.tokCloseParen)
        {
            break;
        }
        if (sym == gs.tokComma)
        {
            if (inParam)
            {
                result.push_back(std::move(current));
                current = LambdaParamASTInfo{};
                current.index = static_cast<uint32_t>(result.size());
                inParam = false;
            }
            continue;
        }

        if (ts_node_is_null(current.startNode))
        {
            current.startNode = child;
        }
        inParam = true;

        const char* field = ts_tree_cursor_current_field_name(&cursor);
        ApplyLambdaParamChild(child, field, sourceCode, current);

    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);

    if (inParam)
    {
        result.push_back(std::move(current));
    }

    return result;
}

std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode)
{
    std::vector<LambdaParameter> parameters;
    auto astParams = ExtractLambdaParameters(listNode, sourceCode);
    parameters.reserve(astParams.size());
    for (const auto& p : astParams)
    {
        LambdaParameter lp;
        lp.typeName = p.typeName;
        lp.hasWrittenType = p.hasWrittenType;
        lp.isConst = p.isConst;
        lp.isHandle = p.isHandle;
        lp.isReference = p.isReference;
        lp.modifier = p.modifier;
        parameters.push_back(std::move(lp));
    }
    return parameters;
}

bool LambdaContradictsFuncdef(const std::vector<LambdaParameter>& lambdaParameters, const FuncdefSignature& funcdefSig,
                              const SymbolTable& table)
{
    if (lambdaParameters.size() != funcdefSig.parameters.size())
    {
        return true;
    }

    for (size_t i = 0; i < lambdaParameters.size(); ++i)
    {
        const LambdaParameter& written = lambdaParameters[i];
        if (!written.hasWrittenType)
        {
            continue;
        }

        const ParameterInformation& expected = funcdefSig.parameters[i];
        if (written.isHandle != expected.isHandle || written.isReference != expected.isReference ||
            written.modifier != expected.modifier)
        {
            return true;
        }

        // Compared by last `::` segment, so a name the funcdef writes bare and the lambda
        // writes qualified - or the other way round - is the same name. That costs the
        // `A::Foo` against `B::Foo` case, which is a rejection this misses rather than a legal
        // program it reports.
        const std::string writtenBase(CanonicalizeType(LastSegmentOf(written.typeName)));
        const std::string expectedBase(CanonicalizeType(LastSegmentOf(CleanBaseType(expected.typeName))));

        if (writtenBase.empty() || expectedBase.empty() || writtenBase == expectedBase)
        {
            continue;
        }
        if (NamesATypedef(writtenBase, table) || NamesATypedef(expectedBase, table))
        {
            continue;
        }
        return true;
    }

    return false;
}

bool LambdaContradictsFuncdef(TSNode lambdaNode, const FuncdefSignature& funcdefSig, const SymbolTable& table,
                              std::string_view sourceCode)
{
    TSNode listNode = parser::GetChildByField(lambdaNode, parser::GetGrammarSymbols().fieldParameters);
    if (ts_node_is_null(listNode))
    {
        // No parameter list to read is not the same as an empty one, and guessing which it is
        // would be guessing about the whole signature.
        return false;
    }
    return LambdaContradictsFuncdef(ReadLambdaParameters(listNode, sourceCode), funcdefSig, table);
}

std::optional<Symbol> FindFuncdefSymbol(std::string_view typeName, const SymbolTable& table)
{
    if (typeName.empty() || parser::primitives::IsPrimitive(typeName))
    {
        return std::nullopt;
    }

    if (const auto bucket = table.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *bucket)
        {
            if (sym.type == SymbolType::Funcdef)
            {
                return sym;
            }
        }
    }

    const std::string_view bare = LastScopeSegment(typeName);
    const auto matches = table.FindTypeSymbolsByShortName(bare);
    for (const auto& sym : matches)
    {
        if (sym.type == SymbolType::Funcdef)
        {
            return sym;
        }
    }
    if (const auto bucket = table.FindSymbolsPtr(bare))
    {
        for (const auto& sym : *bucket)
        {
            if (sym.type == SymbolType::Funcdef)
            {
                return sym;
            }
        }
    }
    return std::nullopt;
}

bool IsHandleType(std::string_view typeName, const SymbolTable& symbolTable) noexcept
{
    if (typeName.empty())
    {
        return false;
    }
    if (typeName.find('@') != std::string_view::npos)
    {
        return true;
    }
    const std::string clean = CleanBaseType(typeName);
    return !clean.empty() && FindFuncdefSymbol(clean, symbolTable).has_value();
}

/**
 * @brief The funcdef a lambda is being handed to, read from where it is written.
 *
 * A lambda has no return type of its own - the grammar gives `lambda_expression` a parameter
 * list and a body and nothing else - so every question about what its body must return has to
 * come from the target. That is what this answers, for the three shapes a lambda reaches a
 * funcdef through:
 *
 *     CB@ cb = function(...) { };      a declaration whose written type is a funcdef
 *     Register(CB(function(...)));     a funcdef used as a conversion
 *     Take(function(...));             an argument landing on a funcdef parameter
 *
 * Anything else answers nullopt, and so does a call with more than one candidate offering a
 * different funcdef at that position: which one the lambda was written against is precisely
 * what is undecided there, and a return-type verdict drawn from a guess would be worse than
 * the silence it replaced.
 */
/**
 * @brief Finds the target funcdef symbol for a variable declarator.
 */
static std::optional<Symbol> FindDeclaratorFuncdefTarget(TSNode parent, const SymbolTable& table,
                                                         std::string_view sourceCode)
{
    TSNode declaration = parser::GetParent(parent);
    if (!ts_node_is_null(declaration))
    {
        TSNode typeNode = parser::GetChildByField(declaration, parser::GetGrammarSymbols().fieldVarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(declaration, parser::GetGrammarSymbols().fieldType);
        }
        if (!ts_node_is_null(typeNode))
        {
            return FindFuncdefSymbol(CleanBaseType(GetNodeText(typeNode, sourceCode)), table);
        }
    }
    return std::nullopt;
}

/**
 * @brief Locates the index of targetChild within parent's named children.
 */
static std::optional<uint32_t> FindChildPosition(TSNode parent, TSNode targetChild)
{
    for (uint32_t i = 0; i < ts_node_named_child_count(parent); ++i)
    {
        if (ts_node_eq(ts_node_named_child(parent, i), targetChild))
        {
            return i;
        }
    }
    return std::nullopt;
}

/**
 * @brief Extracts the callee syntax node from a call expression node.
 */
static TSNode ExtractCallCalleeNode(TSNode call)
{
    return parser::GetCallCallee(call);
}

struct FuncdefAgreement
{
    std::optional<Symbol> agreed;
    bool sawCandidate = false;
    bool conflict = false;
};

/**
 * @brief Matches candidate function parameters at position against funcdef symbols.
 */
static bool MatchCandidateFuncdef(const SymbolTable& table, const std::string& name, uint32_t position,
                                  FuncdefAgreement& agreement)
{
    if (agreement.conflict)
    {
        return false;
    }
    const auto bucket = table.FindSymbolsPtr(name);
    if (!bucket)
    {
        return false;
    }
    for (const auto& sym : *bucket)
    {
        if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }
        const auto& parameters = sym.GetFunction().parameters;
        if (position >= parameters.size())
        {
            continue;
        }
        auto funcdef = FindFuncdefSymbol(CleanBaseType(parameters[position].typeName), table);
        if (!funcdef)
        {
            continue;
        }
        if (agreement.sawCandidate && agreement.agreed && agreement.agreed->name != funcdef->name)
        {
            agreement.agreed = std::nullopt;
            agreement.conflict = true;
            return false;
        }
        agreement.agreed = std::move(funcdef);
        agreement.sawCandidate = true;
    }
    return agreement.sawCandidate && !agreement.conflict;
}

/**
 * @brief Finds the target funcdef symbol for an assignment expression.
 */
static std::optional<Symbol> FindAssignmentFuncdefTarget(TSNode parent, const SymbolTable& table,
                                                         std::string_view sourceCode)
{
    TSNode leftNode = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldLeft);
    if (ts_node_is_null(leftNode))
    {
        return std::nullopt;
    }
    std::string varName = CleanBaseType(GetNodeText(leftNode, sourceCode));
    const size_t dot = varName.rfind('.');
    if (dot != std::string::npos)
    {
        varName = varName.substr(dot + 1);
    }
    const auto syms = table.FindSymbolsPtr(varName);
    if (!syms)
    {
        return std::nullopt;
    }
    for (const auto& s : *syms)
    {
        if (s.type == SymbolType::Variable && std::holds_alternative<VariableSignature>(s.signature))
        {
            if (auto funcdef = FindFuncdefSymbol(CleanBaseType(s.GetVariable().typeName), table))
            {
                return funcdef;
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Resolves target funcdef symbol across possible callee member/function names.
 */
static std::optional<Symbol> FindCallArgumentFuncdef(const SymbolTable& table, const std::string& calleeName,
                                                     uint32_t position)
{
    FuncdefAgreement agreement;
    auto lookUp = [&](const std::string& name) -> bool
    { return MatchCandidateFuncdef(table, name, position, agreement); };

    if (lookUp(calleeName))
    {
        return agreement.agreed;
    }
    if (agreement.conflict)
    {
        return std::nullopt;
    }

    const size_t dot = calleeName.rfind('.');
    if (dot != std::string::npos && lookUp(calleeName.substr(dot + 1)))
    {
        return agreement.agreed;
    }
    if (agreement.conflict)
    {
        return std::nullopt;
    }

    if (!utils::ContainsScopeSeparator(calleeName) && lookUp(utils::JoinScopedName(calleeName, calleeName)))
    {
        return agreement.agreed;
    }
    if (agreement.conflict)
    {
        return std::nullopt;
    }

    const std::string_view leaf = utils::LastScopeSegment(calleeName);
    if (leaf != calleeName && lookUp(std::string(leaf)))
    {
        return agreement.agreed;
    }

    return std::nullopt;
}

std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode, const SymbolTable& table, std::string_view sourceCode)
{
    if (!IsLambdaExpression(lambdaNode))
    {
        return std::nullopt;
    }

    TSNode parent = parser::GetParent(lambdaNode);
    if (ts_node_is_null(parent))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symVariableDeclarator)
    {
        return FindDeclaratorFuncdefTarget(parent, table, sourceCode);
    }
    if (parentSym == gs.symAssignmentExpression)
    {
        return FindAssignmentFuncdefTarget(parent, table, sourceCode);
    }
    if (parentSym != gs.symArgumentList)
    {
        return std::nullopt;
    }

    auto position = FindChildPosition(parent, lambdaNode);
    if (!position)
    {
        return std::nullopt;
    }

    TSNode call = parser::GetParent(parent);
    if (ts_node_is_null(call))
    {
        return std::nullopt;
    }

    TSNode callee = ExtractCallCalleeNode(call);
    if (ts_node_is_null(callee))
    {
        return std::nullopt;
    }

    const std::string calleeName = CleanBaseType(GetNodeText(callee, sourceCode));
    if (*position == 0 && ts_node_named_child_count(parent) == 1)
    {
        if (auto asConversion = FindFuncdefSymbol(calleeName, table))
        {
            return asConversion;
        }
    }

    return FindCallArgumentFuncdef(table, calleeName, *position);
}

static uint32_t FindLambdaParamIndex(TSNode paramsNode, std::string_view paramName, std::string_view sourceCode)
{
    auto astParams = ExtractLambdaParameters(paramsNode, sourceCode);
    for (const auto& p : astParams)
    {
        if (p.name == paramName)
        {
            return p.index;
        }
    }
    return UINT32_MAX;
}

std::string InferLambdaParamType(TSNode nodeInLambda, std::string_view paramName, const SymbolTable& symbolTable,
                                 std::string_view sourceCode)
{
    if (ts_node_is_null(nodeInLambda) || paramName.empty())
    {
        return "";
    }

    TSNode lambdaNode{};
    TSNode cur = nodeInLambda;
    for (int i = 0; i < constants::k_maxAstClimbDepth && !ts_node_is_null(cur); ++i, cur = parser::GetParent(cur))
    {
        if (IsLambdaExpression(cur))
        {
            lambdaNode = cur;
            break;
        }
    }
    if (ts_node_is_null(lambdaNode))
    {
        return "";
    }

    TSNode paramsNode = parser::GetChildByField(lambdaNode, parser::GetGrammarSymbols().fieldParameters);
    if (ts_node_is_null(paramsNode))
    {
        return "";
    }

    uint32_t paramIdx = FindLambdaParamIndex(paramsNode, paramName, sourceCode);
    if (paramIdx == UINT32_MAX)
    {
        return "";
    }

    auto targetFuncdef = FuncdefTargetOfLambda(lambdaNode, symbolTable, sourceCode);
    if (!targetFuncdef)
    {
        return "";
    }

    if (std::holds_alternative<FuncdefSignature>(targetFuncdef->signature))
    {
        const auto& fd = targetFuncdef->GetFuncdef();
        if (paramIdx < fd.parameters.size())
        {
            return MemberOwnerType(fd.parameters[paramIdx].typeName, symbolTable.GetDefaultArrayType());
        }
    }
    else if (std::holds_alternative<FunctionSignature>(targetFuncdef->signature))
    {
        const auto& fn = targetFuncdef->GetFunction();
        if (paramIdx < fn.parameters.size())
        {
            return MemberOwnerType(fn.parameters[paramIdx].typeName, symbolTable.GetDefaultArrayType());
        }
    }
    return "";
}

Symbol FuncdefToFunctionSymbol(const Symbol& funcdefSym, std::string_view callName)
{
    Symbol sym;
    sym.type = SymbolType::Function;
    sym.name = std::string(callName);
    sym.containerName = funcdefSym.containerName;
    sym.containerKind = funcdefSym.containerKind;
    sym.qualifiedName = funcdefSym.qualifiedName;
    sym.fileUri = funcdefSym.fileUri;
    sym.startLine = funcdefSym.startLine;
    sym.startCharacter = funcdefSym.startCharacter;
    sym.endLine = funcdefSym.endLine;
    sym.endCharacter = funcdefSym.endCharacter;

    if (std::holds_alternative<FuncdefSignature>(funcdefSym.signature))
    {
        const auto& fd = funcdefSym.GetFuncdef();
        FunctionSignature fnSig;
        fnSig.returnType = fd.returnType;
        fnSig.returnBaseTypeName = fd.returnBaseTypeName;
        fnSig.returnTemplateName = fd.returnTemplateName;
        fnSig.returnTypeKind = fd.returnTypeKind;
        fnSig.returnIsArray = fd.returnIsArray;
        fnSig.returnIsConst = fd.returnIsConst;
        fnSig.returnHasPrimitiveHandle = fd.returnHasPrimitiveHandle;
        fnSig.returnArrayDepth = fd.returnArrayDepth;
        fnSig.modifiers = fd.modifiers;
        fnSig.parameters = fd.parameters;
        sym.signature = std::move(fnSig);
    }
    return sym;
}

std::optional<Symbol> TryResolveCallableFuncdef(const std::string& name, const Scope* scope, const SymbolTable& table,
                                                uint32_t refByte)
{
    if (!scope || name.empty())
    {
        return std::nullopt;
    }
    const LocalDefinition* def = ResolveInScopeAt(scope, name, refByte);
    if (!def || (def->kind != LocalDefinitionKind::Variable && def->kind != LocalDefinitionKind::Parameter))
    {
        return std::nullopt;
    }
    const std::string clean = CleanBaseType(def->typeName);
    if (clean.empty())
    {
        return std::nullopt;
    }
    auto funcdefSym = FindFuncdefSymbol(clean, table);
    if (!funcdefSym)
    {
        return std::nullopt;
    }
    return FuncdefToFunctionSymbol(*funcdefSym, name);
}

bool IsValidNumberLiteral(std::string_view s) noexcept
{
    const size_t start = (s.front() == '+' || s.front() == '-') ? 1 : 0;
    if (start >= s.size() || !std::isdigit(static_cast<unsigned char>(s[start])))
    {
        return false;
    }
    constexpr std::string_view k_allowed = "0123456789abcdefABCDEF.xXfFuUlL";
    return s.substr(start).find_first_not_of(k_allowed) == std::string_view::npos;
}

bool IsQuotedLiteral(std::string_view s) noexcept
{
    if (s.starts_with("\"\"\""))
    {
        return s.size() >= 6 && s.ends_with("\"\"\"");
    }
    if (s.size() < 2)
    {
        return false;
    }
    return (s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'');
}

bool IsSimpleLiteralExpression(std::string_view expr)
{
    while (!expr.empty() && (expr.front() == ' ' || expr.front() == '\t'))
    {
        expr.remove_prefix(1);
    }
    while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t'))
    {
        expr.remove_suffix(1);
    }
    if (expr.empty())
    {
        return false;
    }
    if (expr == "true" || expr == "false" || expr == "null")
    {
        return true;
    }
    if (IsQuotedLiteral(expr))
    {
        return true;
    }
    return IsValidNumberLiteral(expr);
}

TSNode FindMixinInclusionNode(TSNode hostNode, std::string_view mixinName, std::string_view mixinShortName,
                              std::string_view sourceCode)
{
    if (ts_node_is_null(hostNode))
    {
        return {};
    }

    TSNode found{};
    TSNode baseList = parser::FindFirstChildOfType(hostNode, "base_class_list");
    if (!ts_node_is_null(baseList))
    {
        parser::ForEachNamedChildNodeUntil(baseList,
                                           [&](TSNode baseChild)
                                           {
                                               const std::string bText = GetNodeText(baseChild, sourceCode);
                                               if (CleanBaseTypeView(bText) == mixinName || bText == mixinShortName ||
                                                   utils::EndsWithScopedMember(bText, mixinShortName))
                                               {
                                                   found = baseChild;
                                                   return false;
                                               }
                                               return true;
                                           });
        if (!ts_node_is_null(found))
        {
            return found;
        }
    }

    TSNode bodyNode = parser::GetChildByField(hostNode, parser::GetGrammarSymbols().fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        parser::ForEachChildNodeUntil(bodyNode,
                                      [&](TSNode mNode)
                                      {
                                          const std::string mText = GetNodeText(mNode, sourceCode);
                                          if (!mixinShortName.empty() &&
                                              mText.find(mixinShortName) != std::string::npos)
                                          {
                                              found = mNode;
                                              return false;
                                          }
                                          return true;
                                      });
    }
    return found;
}

std::string ExtractDatatypeScopePrefix(TSNode datatypeNode, std::string_view sourceCode)
{
    TSNode typeNode = parser::GetParent(datatypeNode);
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(typeNode) || ts_node_symbol(typeNode) != gs.symType)
    {
        return {};
    }
    TSTreeCursor cursor = ts_tree_cursor_new(typeNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode c = ts_tree_cursor_current_node(&cursor);
            if (ts_node_symbol(c) == gs.symScope)
            {
                const uint32_t sStart = ts_node_start_byte(c);
                const uint32_t sEnd = ts_node_end_byte(c);
                ts_tree_cursor_delete(&cursor);
                if (sStart < sourceCode.size() && sEnd <= sourceCode.size() && sStart < sEnd)
                {
                    std::string scopeText(sourceCode.substr(sStart, sEnd - sStart));
                    if (!utils::EndsWithScopeSeparator(scopeText))
                    {
                        scopeText.append(utils::k_scopeSeparator);
                    }
                    return scopeText;
                }
                return {};
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
        ts_tree_cursor_delete(&cursor);
    }
    return {};
}

std::string_view CleanCandidateIdentifier(std::string_view text) noexcept
{
    if (text.starts_with('@'))
    {
        text.remove_prefix(1);
    }
    if (text.starts_with("this."))
    {
        text.remove_prefix(5);
    }
    return text;
}

bool IsValidIdentifierOrQualified(std::string_view text) noexcept
{
    text = CleanCandidateIdentifier(text);
    if (text.empty() || text.size() > 128)
    {
        return false;
    }
    while (!text.empty())
    {
        const auto [part, rest] = utils::SplitFirstScopeSegment(text);
        if (part.empty() || (!std::isalpha(static_cast<unsigned char>(part[0])) && part[0] != '_'))
        {
            return false;
        }
        for (size_t i = 1; i < part.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(part[i]);
            if (!std::isalnum(c) && c != '_')
            {
                return false;
            }
        }
        text = rest;
    }
    return true;
}

std::vector<std::pair<uint32_t, uint32_t>> CollectNamespaceAndContainerLineRanges(const SymbolTable& symbolTable,
                                                                                  const std::string& fileUri,
                                                                                  std::string_view declaringNamespace)
{
    std::vector<std::pair<uint32_t, uint32_t>> nsRanges;
    if (declaringNamespace.empty())
    {
        return nsRanges;
    }
    symbolTable.ForEachSymbolInFile(
        fileUri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& sList)
        {
            for (const auto& s : sList)
            {
                if (s.fileUri != fileUri)
                {
                    continue;
                }
                if (s.type == analysis::SymbolType::Namespace &&
                    (s.name == declaringNamespace || s.qualifiedName == declaringNamespace))
                {
                    nsRanges.push_back({s.startLine, s.endLine});
                }
                else if ((s.type == analysis::SymbolType::Class || s.type == analysis::SymbolType::Interface) &&
                         IsSameOrNestedNamespace(s.containerName, declaringNamespace))
                {
                    nsRanges.push_back({s.startLine, s.endLine});
                }
            }
        });
    return nsRanges;
}

} // namespace angel_lsp::analysis
