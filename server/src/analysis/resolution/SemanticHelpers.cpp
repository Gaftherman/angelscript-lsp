#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "config/EngineRuleConfig.h"
#include "parser/ASTUtils.h"
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
/**
 * @brief Normalizes an expression type name by stripping const, references, handles, and whitespace.
 * @param[in] typeName Raw expression type name to sanitize.
 * @return Cleaned base type name without qualifiers or decorations.
 */
std::string CleanExpressionType(std::string_view typeName)
{
    typeName = StripLeadingConst(typeName);
    TrimTypeWhitespace(typeName);
    if (const size_t refLen = TrailingReferenceModifierLength(typeName); refLen > 0)
    {
        typeName.remove_suffix(refLen);
    }
    TrimTypeWhitespace(typeName);
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

std::string GetTrimmedNodeText(TSNode node, std::string_view sourceCode)
{
    std::string text = parser::GetNodeText(node, sourceCode);
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

/**
 * @brief Has list constructor param.
 * @param[in] p Parameter or path under evaluation.
 * @return True if the condition is satisfied; false otherwise.
 */
static bool HasListConstructorParam(const ParameterInformation& p)
{
    const std::string norm = StripTypeDecorations(p.baseTypeName.empty() ? p.typeName : p.baseTypeName);
    const bool isInt = (norm == "int" || norm == "uint" || norm == "int64" || norm == "uint64");
    return isInt && (p.isReference || p.modifier == ParameterModifier::In || p.modifier == ParameterModifier::InOut ||
                     p.isInOutRef || IsReferenceTypeView(p.typeName));
}

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
    std::string name = parser::GetNodeText(node, sourceCode);
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
    node = parser::UnwrapParentheses(node);
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
    const std::string memName = parser::NodeText(memNode, sourceCode);
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

static bool IsLocalVariableOrFieldInScope(const Scope* scope, TSSymbol sym, const std::string& name,
                                          uint32_t refByte = UINT32_MAX)
{
    const auto& gs = parser::GetGrammarSymbols();
    const bool isUnqualified =
        (sym == gs.symIdentifier || (sym == gs.symScopedIdentifier && !utils::ContainsScopeSeparator(name)));
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
    std::string name = parser::GetNodeText(node, sourceCode);
    if (name.empty())
    {
        return false;
    }
    if (IsLocalVariableOrFieldInScope(scope, ts_node_symbol(node), name, ts_node_start_byte(node)))
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

TSNode FindEnclosingAncestorNode(TSNode node, TSSymbol targetSym)
{
    return parser::FindAncestorWithSymbol(parser::GetParent(node), targetSym);
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
    if (ts_node_is_null(className) ||
        parser::GetNodeText(className, sourceCode) != parser::GetNodeText(functionName, sourceCode))
    {
        return false;
    }

    return HasBaseClassList(owner);
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

} // namespace angel_lsp::analysis
