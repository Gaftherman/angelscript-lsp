#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/EnumDeclarationFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/TypeRules.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
void ValidateInterfaceMembers(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Interface || IsFromPredefinedStub(sym, ctx))
    {
        return;
    }

    const std::string_view qualified =
        sym.qualifiedName.empty() ? std::string_view(sym.name) : std::string_view(sym.qualifiedName);

    const auto members = ctx.request.symbolTable.FindMemberSymbolPtr(qualified, sym.name);
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

static void CollectDuplicateCandidates(const std::vector<Symbol>& symbols, const DiagnosticContext& ctx,
                                       std::vector<const Symbol*>& local, std::vector<const Symbol*>& moduleMates)
{
    const bool currentIsPredefined = utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension);

    for (const auto& sym : symbols)
    {
        if (sym.type == SymbolType::CallReference || sym.type == SymbolType::Namespace)
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

        if (!currentIsPredefined)
        {
            if (IsFromPredefinedStub(sym, ctx) ||
                utils::IsPredefinedFile(sym.fileUri, ctx.request.predefinedFileExtension))
            {
                moduleMates.push_back(&sym);
                continue;
            }
        }

        if (!ctx.request.moduleFileUris.empty() && ctx.request.moduleFileUris.contains(sym.fileUri) &&
            !currentIsPredefined)
        {
            moduleMates.push_back(&sym);
        }
    }
}

static bool AreIdenticalFunctionSignatures(const Symbol& first, const Symbol& other)
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
            first.name == "opImplCast" || first.name == "opIndex")
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
            firstParams[p].isHandle != otherParams[p].isHandle || firstParams[p].isConst != otherParams[p].isConst)
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
static const Symbol* FindEnclosingSharedInterface(const Symbol& method, const std::vector<Symbol>& ifaces)
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
static bool IsSharedInterfaceDuplicateMethod(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
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
static bool IsAllowedDuplicateVariable(const Symbol& first, const Symbol& other)
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
static bool IsAllowedDuplicateClass(const Symbol& first, const Symbol& other)
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
static bool IsAllowedDuplicateInterface(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
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
static bool IsAllowedDuplicate(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
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

/**
 * @brief Checks if duplicate pair consists of compatible namespaces or homonymous containers.
 * @param[in] first First symbol.
 * @param[in] other Other symbol.
 * @return True if symbols are allowed to coexist.
 */
static bool IsCompatibleNamespaceOrHomonym(const Symbol& first, const Symbol& other) noexcept
{
    if (first.type == SymbolType::Namespace && other.type == SymbolType::Namespace)
    {
        return true;
    }
    if ((first.type == SymbolType::Namespace && other.type == SymbolType::Class) ||
        (first.type == SymbolType::Class && other.type == SymbolType::Namespace))
    {
        return true;
    }
    return first.containerName == other.containerName && first.containerKind != other.containerKind;
}

static bool CheckDuplicatePair(const Symbol& first, const Symbol& other, const DiagnosticContext& ctx)
{
    if (first.fileUri == other.fileUri && first.startLine == other.startLine &&
        first.startCharacter == other.startCharacter)
    {
        return false;
    }

    if (IsCompatibleNamespaceOrHomonym(first, other))
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

void ValidateDuplicates(const std::vector<Symbol>& symbols, const DiagnosticContext& ctx)
{
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

    for (size_t i = 0; i < local.size(); ++i)
    {
        const Symbol& other = *local[i];
        bool foundDup = false;

        for (const Symbol* mate : moduleMates)
        {
            if (CheckDuplicatePair(*mate, other, ctx))
            {
                foundDup = true;
                break;
            }
        }

        if (foundDup)
        {
            continue;
        }

        for (size_t j = 0; j < i; ++j)
        {
            if (CheckDuplicatePair(*local[j], other, ctx))
            {
                break;
            }
        }
    }
}

} // namespace angel_lsp::analysis::rules
