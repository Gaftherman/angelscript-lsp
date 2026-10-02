#include "analysis/rules/TypeRules.h"
#include "analysis/ASTUtils.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
namespace
{
/**
 * @brief Searches for the enum_declaration AST node matching the symbol's start point or name.
 */
TSNode FindEnumDeclarationNode(TSNode root, const Symbol& sym, std::string_view sourceCode)
{
    TSNode found{};
    parser::ForEachDescendantNode(root,
                                  [&](TSNode node)
                                  {
                                      if (!ts_node_is_null(found))
                                      {
                                          return;
                                      }
                                      if (NodeType(node) == parser::nodes::EnumDeclaration)
                                      {
                                          const TSPoint pt = ts_node_start_point(node);
                                          if (pt.row == sym.startLine && pt.column == sym.startCharacter)
                                          {
                                              found = node;
                                              return;
                                          }
                                          const TSNode nameNode = parser::GetChildByField(node, parser::fields::Name);
                                          if (!ts_node_is_null(nameNode) && NodeText(nameNode, sourceCode) == sym.name)
                                          {
                                              found = node;
                                          }
                                      }
                                  });
    return found;
}

/** @brief True when the name collides with a keyword or a built-in type name. */
bool IsUnusableName(const std::string& name, const DiagnosticContext& ctx)
{
    return IsReservedKeyword(name) || IsPrimitiveTypeName(name) || name == ctx.request.GetEffectiveStringTypeName() ||
           name == ctx.request.GetEffectiveArrayTypeName();
}

bool IsPrefixedIntegerLiteral(std::string_view text, size_t i)
{
    char prefix = text[i + 1];
    if (prefix == 'x' || prefix == 'X')
    {
        return text.substr(i + 2).find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
    }
    if (prefix == 'b' || prefix == 'B')
    {
        return text.substr(i + 2).find_first_not_of("01") == std::string_view::npos;
    }
    if (prefix == 'o' || prefix == 'O')
    {
        return text.substr(i + 2).find_first_not_of("01234567") == std::string_view::npos;
    }
    return false;
}

/** @brief True for an integer literal, including signed forms like `-1`. */
bool IsIntegerLiteral(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }
    if (!text.empty() && (text.front() == '-' || text.front() == '+'))
    {
        text.remove_prefix(1);
    }
    if (text.empty())
    {
        return false;
    }

    if (text.size() > 2 && text[0] == '0')
    {
        if (IsPrefixedIntegerLiteral(text, 0))
        {
            return true;
        }
    }
    return text.find_first_not_of("0123456789") == std::string_view::npos;
}

/** @brief Formats a symbol type into the noun the compiler uses in as-err-name-conflict. */
std::string KindWord(SymbolType type)
{
    switch (type)
    {
    case SymbolType::Function:
        return "function";
    case SymbolType::Class:
        return "class";
    case SymbolType::Interface:
        return "interface";
    case SymbolType::Enum:
        return "enum";
    case SymbolType::Typedef:
        return "typedef";
    case SymbolType::Funcdef:
        return "funcdef";
    case SymbolType::Namespace:
        return "namespace";
    default:
        return "variable";
    }
}
} // namespace

void ValidateTypedef(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Typedef)
    {
        return;
    }

    if (IsUnusableName(sym.name, ctx))
    {
        ctx.LogRule("ValidateTypedef", "as-err-reserved-keyword-name", sym);
        ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
        return;
    }

    const auto& sig = sym.GetTypedef();
    const std::string baseType = CleanBaseType(sig.baseType);

    if (!sig.hasSemicolon)
    {
        ctx.LogRule("ValidateTypedef", "as-syntax-error-missing", sym);
        ctx.Emit(sym, "as-syntax-error-missing", ";");
    }

    if (!baseType.empty() && !IsPrimitiveTypeName(baseType))
    {
        ctx.LogRule("ValidateTypedef", diagnostics::codes::TypedefNonPrimitive, sym);
        if (sig.baseTypeEndCharacter > sig.baseTypeStartCharacter || sig.baseTypeEndLine > sig.baseTypeStartLine)
        {
            ctx.EmitAtRange(
                {sig.baseTypeStartLine, sig.baseTypeStartCharacter, sig.baseTypeEndLine, sig.baseTypeEndCharacter},
                diagnostics::codes::TypedefNonPrimitive, baseType);
        }
        else
        {
            ctx.Emit(sym, diagnostics::codes::TypedefNonPrimitive, baseType);
        }
    }
}

namespace
{
void ValidateFuncdefReturnType(const Symbol& sym, const FuncdefSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.returnHasPrimitiveHandle)
    {
        ctx.LogRule("ValidateFuncdef", "as-err-handle-on-primitive", sym);
        ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
    }

    const std::string retBase = CleanBaseType(sig.returnBaseTypeName.empty() ? sig.returnType : sig.returnBaseTypeName);
    if (!retBase.empty() && retBase != "void" && retBase != "auto" && !IsKnownType(retBase, ctx))
    {
        ctx.LogRule("ValidateFuncdef", "as-err-unresolved-type", sym);
        if (sig.returnTypeEndCharacter > sig.returnTypeStartCharacter ||
            sig.returnTypeEndLine > sig.returnTypeStartLine)
        {
            ctx.EmitAtRange({sig.returnTypeStartLine, sig.returnTypeStartCharacter, sig.returnTypeEndLine,
                             sig.returnTypeEndCharacter},
                            "as-err-unresolved-type", retBase);
        }
        else
        {
            ctx.Emit(sym, "as-err-unresolved-type", retBase);
        }
    }
}

void ValidateFuncdefParameter(const Symbol& sym, const ParameterInformation& param, const DiagnosticContext& ctx)
{
    if (param.hasPrimitiveHandle)
    {
        ctx.LogRule("ValidateFuncdef", "as-err-handle-on-primitive", sym);
        ctx.Emit(sym, "as-err-handle-on-primitive", param.baseTypeName);
    }

    const bool isPredefined =
        angel_lsp::utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension);
    const std::string paramBase = CleanBaseType(param.baseTypeName.empty() ? param.typeName : param.baseTypeName);
    if (!paramBase.empty() && paramBase != "void" && paramBase != "auto" && !(isPredefined && paramBase == "?") &&
        !IsKnownType(paramBase, ctx))
    {
        ctx.LogRule("ValidateFuncdef", "as-err-unresolved-type", sym);
        if (param.endCharacter > param.startCharacter || param.endLine > param.startLine)
        {
            ctx.EmitAtRange({param.startLine, param.startCharacter, param.endLine, param.endCharacter},
                            "as-err-unresolved-type", paramBase);
        }
        else
        {
            ctx.Emit(sym, "as-err-unresolved-type", paramBase);
        }
    }
}

void ValidateFuncdefParameters(const Symbol& sym, const std::vector<ParameterInformation>& params,
                               const DiagnosticContext& ctx)
{
    for (const auto& param : params)
    {
        ValidateFuncdefParameter(sym, param, ctx);
    }
}
} // namespace

void ValidateFuncdef(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Funcdef)
    {
        return;
    }

    if (IsUnusableName(sym.name, ctx))
    {
        ctx.LogRule("ValidateFuncdef", "as-err-reserved-keyword-name", sym);
        ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
        return;
    }

    const auto& sig = sym.GetFuncdef();

    const std::string_view attribute = FirstAttributeName(sig.modifiers);
    if (!attribute.empty())
    {
        ctx.LogRule("ValidateFuncdef", "as-err-funcdef-attribute", sym);
        ctx.Emit(sym, "as-err-funcdef-attribute", attribute, sym.name);
    }

    ValidateFuncdefReturnType(sym, sig, ctx);
    ValidateFuncdefParameters(sym, sig.parameters, ctx);
}

namespace
{
void CheckEnumModifiersAndBody(const Symbol& sym, const EnumSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.hasBraces && !sig.modifiers.isExternal)
    {
        ctx.LogRule("ValidateEnum", "as-err-declaration-missing-body", sym);
        ctx.Emit(sym, "as-err-declaration-missing-body", sym.name);
    }

    if (sig.modifiers.isExternal && !sig.modifiers.isShared)
    {
        ctx.LogRule("ValidateEnum", "as-err-external-not-shared", sym);
        ctx.Emit(sym, "as-err-external-not-shared", sym.name);
    }

    if (sig.modifiers.isExternal && sig.modifiers.isShared)
    {
        bool hasFullSharedDefinition = false;
        if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
        {
            for (const auto& s : *symsPtr)
            {
                if (s.type == SymbolType::Enum && s.GetEnum().hasBraces && s.GetEnum().modifiers.isShared &&
                    !s.GetEnum().modifiers.isExternal)
                {
                    hasFullSharedDefinition = true;
                    break;
                }
            }
        }
        if (!hasFullSharedDefinition)
        {
            ctx.LogRule("ValidateEnum", "as-err-external-not-found", sym);
            ctx.Emit(sym, "as-err-external-not-found", sym.name);
        }
    }
}

void CheckEnumMemberInitializers(const Symbol& sym, const EnumSignature& sig, const DiagnosticContext& ctx)
{
    for (const auto& member : sig.members)
    {
        if (member.value.empty())
        {
            continue;
        }

        const bool isLiteralNode = member.valueNodeType == std::string(node_types::StringLiteral) ||
                                   member.valueNodeType == std::string(node_types::BooleanLiteral) ||
                                   member.valueNodeType == std::string(node_types::NullLiteral);
        const bool isNonIntegerNumber =
            member.valueNodeType == std::string(node_types::NumberLiteral) && !IsIntegerLiteral(member.value);

        if (isLiteralNode || isNonIntegerNumber)
        {
            ctx.LogRule("ValidateEnum", "as-err-enum-invalid-initializer", sym);
            ctx.Emit(sym, "as-err-enum-invalid-initializer", member.value);
        }
    }
}

void CheckDuplicateEnumMembers(TSNode enumNode, const Symbol& sym, const DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string> seenMemberNames;
    TSTreeCursor cursor = ts_tree_cursor_new(enumNode);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return;
    }

    do
    {
        const TSNode child = ts_tree_cursor_current_node(&cursor);
        if (NodeType(child) != parser::nodes::EnumMember)
        {
            continue;
        }
        const TSNode memberNameNode = parser::GetChildByField(child, parser::fields::Name);
        if (ts_node_is_null(memberNameNode))
        {
            continue;
        }
        const std::string memberName(NodeText(memberNameNode, ctx.request.sourceCode));
        if (memberName.empty())
        {
            continue;
        }
        if (!seenMemberNames.insert(memberName).second)
        {
            const TSPoint start = ts_node_start_point(memberNameNode);
            const TSPoint end = ts_node_end_point(memberNameNode);
            ctx.LogRule("ValidateEnum", diagnostics::codes::DuplicateEnumMember, sym);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::DuplicateEnumMember,
                            memberName);
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);
}

void CheckDuplicateEnumFallback(const Symbol& sym, const EnumSignature& sig, const DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string> seenMemberNames;
    for (const auto& member : sig.members)
    {
        if (member.name.empty())
        {
            continue;
        }
        if (!seenMemberNames.insert(member.name).second)
        {
            ctx.LogRule("ValidateEnum", diagnostics::codes::DuplicateEnumMember, sym);
            ctx.Emit(sym, diagnostics::codes::DuplicateEnumMember, member.name);
        }
    }
}
} // namespace

void ValidateEnum(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Enum || IsFromPredefinedStub(sym, ctx))
    {
        return;
    }

    if (IsUnusableName(sym.name, ctx))
    {
        ctx.LogRule("ValidateEnum", "as-err-reserved-keyword-name", sym);
        ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
        return;
    }

    const auto& sig = sym.GetEnum();
    CheckEnumModifiersAndBody(sym, sig, ctx);
    CheckEnumMemberInitializers(sym, sig, ctx);

    if (ctx.request.tree && !ctx.request.sourceCode.empty())
    {
        const TSNode root = ts_tree_root_node(ctx.request.tree);
        const TSNode enumNode = FindEnumDeclarationNode(root, sym, ctx.request.sourceCode);
        if (!ts_node_is_null(enumNode))
        {
            CheckDuplicateEnumMembers(enumNode, sym, ctx);
        }
    }
    else
    {
        CheckDuplicateEnumFallback(sym, sig, ctx);
    }
}

void ValidateInterfaceMembers(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Interface || IsFromPredefinedStub(sym, ctx))
    {
        return;
    }

    const std::string qualified = sym.containerName.empty() ? sym.name : sym.containerName + "::" + sym.name;

    const auto members = ctx.request.symbolTable.FindSymbolsPtr(qualified + "::" + sym.name);
    if (!members)
    {
        return;
    }

    for (const auto& member : *members)
    {
        if (member.type != SymbolType::Function || member.fileUri != ctx.request.fileUri)
        {
            continue;
        }
        if (!std::holds_alternative<FunctionSignature>(member.signature) || !member.GetFunction().returnType.empty())
        {
            continue;
        }

        ctx.LogRule("ValidateInterfaceMembers", diagnostics::codes::InterfaceConstructor, member);
        ctx.Emit(member, diagnostics::codes::InterfaceConstructor, sym.name);
    }
}

namespace
{
void CollectDuplicateCandidates(const std::vector<Symbol>& symbols, const DiagnosticContext& ctx,
                                std::vector<const Symbol*>& local, std::vector<const Symbol*>& moduleMates)
{
    for (const auto& sym : symbols)
    {
        if (sym.type == SymbolType::CallReference || sym.type == SymbolType::Namespace || sym.type == SymbolType::Enum)
        {
            continue;
        }
        if (IsDestructorDeclaration(sym, ctx) || ctx.request.GetRuleIndex().enumMemberNames.contains(sym.name))
        {
            continue;
        }

        if (sym.fileUri == ctx.request.fileUri)
        {
            local.push_back(&sym);
            continue;
        }

        if (!ctx.request.moduleFileUris.empty() && ctx.request.moduleFileUris.contains(sym.fileUri) &&
            !IsFromPredefinedStub(sym, ctx) &&
            !utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
        {
            moduleMates.push_back(&sym);
        }
    }
}

bool AreIdenticalFunctionSignatures(const Symbol& first, const Symbol& other)
{
    const auto& firstParams = first.GetFunction().parameters;
    const auto& otherParams = other.GetFunction().parameters;
    if (firstParams.size() != otherParams.size())
    {
        return false;
    }
    if (first.GetFunction().returnType != other.GetFunction().returnType)
    {
        if (first.name == "opConv" || first.name == "opImplConv" || first.name == "opCast" ||
            first.name == "opImplCast")
        {
            return false;
        }
    }

    if (first.GetFunction().modifiers.isConst != other.GetFunction().modifiers.isConst)
    {
        return false;
    }

    for (size_t p = 0; p < firstParams.size(); ++p)
    {
        if (firstParams[p].typeName != otherParams[p].typeName || firstParams[p].modifier != otherParams[p].modifier ||
            firstParams[p].isReference != otherParams[p].isReference ||
            firstParams[p].isHandle != otherParams[p].isHandle)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Locates the enclosing shared interface symbol containing a method declaration.
 * @param[in] method Method symbol to inspect.
 * @param[in] ifaces Candidate interface symbols matching the container name.
 * @return Pointer to enclosing shared interface symbol, or nullptr if none matched.
 */
const Symbol* FindEnclosingSharedInterface(const Symbol& method, const std::vector<Symbol>& ifaces)
{
    for (const auto& sym : ifaces)
    {
        if (sym.type == SymbolType::Interface && sym.GetInterface().modifiers.isShared &&
            sym.fileUri == method.fileUri && method.startLine >= sym.startLine && method.endLine <= sym.endLine)
        {
            return &sym;
        }
    }
    return nullptr;
}

/**
 * @brief Checks if two duplicate methods belong to distinct declarations of the same shared interface.
 * @param[in] first First method symbol.
 * @param[in] other Other method symbol.
 * @param[in] ctx Diagnostic context holding symbol table and options.
 * @return True when duplicate is permissible under asEP_IGNORE_DUPLICATE_SHARED_INTF.
 */
bool IsSharedInterfaceDuplicateMethod(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
{
    if (!ctx.request.IgnoresDuplicateSharedInterface() || !first.GetFunction().isInterfaceMethod ||
        !other.GetFunction().isInterfaceMethod || first.containerName.empty() ||
        first.containerName != other.containerName)
    {
        return false;
    }

    const auto ifaceSymbols = ctx.request.symbolTable.FindSymbolsPtr(first.containerName);
    if (!ifaceSymbols)
    {
        return false;
    }

    const Symbol* ifaceFirst = FindEnclosingSharedInterface(first, *ifaceSymbols);
    const Symbol* ifaceOther = FindEnclosingSharedInterface(other, *ifaceSymbols);
    if (!ifaceFirst || !ifaceOther)
    {
        return false;
    }

    return (ifaceFirst != ifaceOther) || (ifaceFirst->startLine != ifaceOther->startLine) ||
           (ifaceFirst->startCharacter != ifaceOther->startCharacter) || (ifaceFirst->fileUri != ifaceOther->fileUri);
}

/**
 * @brief Checks if two duplicate variables represent valid virtual property accessors.
 * @param[in] first First variable symbol.
 * @param[in] other Other variable symbol.
 * @return True if either symbol is a virtual property.
 */
bool IsAllowedDuplicateVariable(const Symbol& first, const Symbol& other)
{
    return first.type == SymbolType::Variable &&
           (first.GetVariable().isVirtualProperty || other.GetVariable().isVirtualProperty);
}

/**
 * @brief Checks if two duplicate class declarations are forward declarations without braces.
 * @param[in] first First class symbol.
 * @param[in] other Other class symbol.
 * @return True if either class declaration lacks braces.
 */
bool IsAllowedDuplicateClass(const Symbol& first, const Symbol& other)
{
    return first.type == SymbolType::Class && other.type == SymbolType::Class &&
           (!first.GetClass().hasBraces || !other.GetClass().hasBraces);
}

/**
 * @brief Checks if two duplicate interface declarations are permitted shared interfaces.
 * @param[in] first First interface symbol.
 * @param[in] other Other interface symbol.
 * @param[in] ctx Diagnostic context.
 * @return True if both are shared interfaces and duplicate shared interfaces are ignored.
 */
bool IsAllowedDuplicateInterface(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
{
    return ctx.request.IgnoresDuplicateSharedInterface() && first.type == SymbolType::Interface &&
           other.type == SymbolType::Interface && first.GetInterface().modifiers.isShared &&
           other.GetInterface().modifiers.isShared;
}

/**
 * @brief Determines whether a duplicate symbol pair is allowed by language rules or configuration.
 * @param[in] first First symbol.
 * @param[in] other Other symbol.
 * @param[in] ctx Diagnostic context.
 * @return True if duplicate is allowed; false otherwise.
 */
bool IsAllowedDuplicate(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
{
    if (IsAllowedDuplicateVariable(first, other) || IsAllowedDuplicateClass(first, other))
    {
        return true;
    }

    if (IsAllowedDuplicateInterface(first, other, ctx))
    {
        return true;
    }

    return first.type == SymbolType::Function && other.type == SymbolType::Function &&
           IsSharedInterfaceDuplicateMethod(first, other, ctx);
}

bool CheckDuplicatePair(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
{
    if (first.startLine == other.startLine && first.startCharacter == other.startCharacter)
    {
        return false;
    }

    if (first.type != other.type)
    {
        ctx.LogRule("ValidateDuplicates", "as-err-name-conflict", other);
        ctx.EmitAtRange(other.selectionRange, "as-err-name-conflict", other.name, KindWord(first.type));
        return true;
    }

    if (first.type == SymbolType::Function)
    {
        if (!AreIdenticalFunctionSignatures(first, other))
        {
            return false;
        }

        if (IsAllowedDuplicate(first, other, ctx))
        {
            return false;
        }

        ctx.LogRule("ValidateDuplicates", "as-err-duplicate-symbol", other);
        ctx.EmitAtRange(other.selectionRange, "as-err-duplicate-symbol", other.name);
        return true;
    }

    if (IsAllowedDuplicate(first, other, ctx))
    {
        return false;
    }

    ctx.LogRule("ValidateDuplicates", "as-err-duplicate-symbol", other);
    ctx.EmitAtRange(other.selectionRange, "as-err-duplicate-symbol", other.name);
    return true;
}
} // namespace

void ValidateDuplicates(const std::vector<Symbol>& symbols, const DiagnosticContext& ctx)
{
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (symbols.size() < 2)
    {
        return;
    }

    std::vector<const Symbol*> local;
    std::vector<const Symbol*> moduleMates;
    CollectDuplicateCandidates(symbols, ctx, local, moduleMates);

    if (local.empty() || local.size() + moduleMates.size() < 2)
    {
        return;
    }

    std::vector<const Symbol*> candidates = moduleMates;
    candidates.insert(candidates.end(), local.begin(), local.end());

    for (size_t i = 1; i < candidates.size(); ++i)
    {
        const Symbol& other = *candidates[i];
        if (other.fileUri != ctx.request.fileUri)
        {
            continue;
        }

        for (size_t j = 0; j < i; ++j)
        {
            if (CheckDuplicatePair(*candidates[j], other, ctx))
            {
                break;
            }
        }
    }
}
} // namespace angel_lsp::analysis::rules
