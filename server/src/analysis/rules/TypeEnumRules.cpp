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
/**
 * @brief Searches for the indexed enum declaration matching the symbol's start point or name.
 * @param[in] facts Extracted collection of indexed enum declarations.
 * @param[in] sym Target enum symbol to match.
 * @return Pointer to the matching IndexedEnumDeclaration if found; otherwise nullptr.
 */
static const IndexedEnumDeclaration* FindEnumDeclaration(const std::vector<IndexedEnumDeclaration>& facts,
                                                         const Symbol& sym)
{
    for (const auto& decl : facts)
    {
        if (decl.start.line == sym.startLine && decl.start.character == sym.startCharacter)
        {
            return &decl;
        }
        if (decl.name == sym.name)
        {
            return &decl;
        }
    }
    return nullptr;
}

static bool IsPrefixedIntegerLiteral(std::string_view text, size_t i)
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

/**
 * @brief True for an integer literal, including signed forms like `-1`.
 */
static bool IsIntegerLiteral(std::string_view text)
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

/**
 * @brief Validates enum declaration body presence and legal modifier combinations.
 * @param[in] sym Enum symbol being inspected.
 * @param[in] sig Enum signature containing modifiers and body brace flags.
 * @param[in] ctx Diagnostic collection and analysis context.
 */
static void CheckEnumModifiersAndBody(const Symbol& sym, const EnumSignature& sig, const DiagnosticContext& ctx)
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

static void CheckEnumMemberInitializers(const Symbol& sym, const EnumSignature& sig, const DiagnosticContext& ctx)
{
    for (const auto& member : sig.members)
    {
        if (member.value.empty())
        {
            continue;
        }

        const bool isLiteralNode = member.valueNodeType == node_types::StringLiteral ||
                                   member.valueNodeType == node_types::BooleanLiteral ||
                                   member.valueNodeType == node_types::NullLiteral;
        const bool isNonIntegerNumber =
            member.valueNodeType == node_types::NumberLiteral && !IsIntegerLiteral(member.value);

        if (isLiteralNode || isNonIntegerNumber)
        {
            ctx.LogRule("ValidateEnum", "as-err-enum-invalid-initializer", sym);
            ctx.Emit(sym, "as-err-enum-invalid-initializer", member.value);
        }
    }
}

/**
 * @brief Checks and emits diagnostics for duplicate member names in an enum declaration.
 * @param[in] decl Indexed enum declaration containing member elements.
 * @param[in] sym Enum symbol associated with this declaration.
 * @param[in] ctx Diagnostic collection and analysis context.
 */
static void CheckDuplicateEnumMembers(const IndexedEnumDeclaration& decl, const Symbol& sym,
                                      const DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string_view> seenMemberNames;
    for (const auto& member : decl.members)
    {
        if (member.name.empty())
        {
            continue;
        }
        if (!seenMemberNames.insert(member.name).second)
        {
            ctx.LogRule("ValidateEnum", diagnostics::codes::DuplicateEnumMember, sym);
            ctx.EmitAtRange({member.span.startRow, member.span.startColumn, member.span.endRow, member.span.endColumn},
                            diagnostics::codes::DuplicateEnumMember, member.name);
        }
    }
}

static void CheckDuplicateEnumFallback(const Symbol& sym, const EnumSignature& sig, const DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string_view> seenMemberNames;
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

/**
 * @brief Validates an enum declaration: name, body, member initializers, and duplicates.
 * @param[in] sym Enum symbol to validate.
 * @param[in] ctx Diagnostic collection and analysis context.
 */
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

    const auto* nodeIndex = ctx.request.GetNodeIndex();
    if (nodeIndex && !ctx.request.sourceCode.empty())
    {
        const std::vector<IndexedEnumDeclaration> facts = BuildEnumDeclarationFacts(*nodeIndex, ctx.request.sourceCode);
        const auto* decl = FindEnumDeclaration(facts, sym);
        if (decl != nullptr)
        {
            CheckDuplicateEnumMembers(*decl, sym, ctx);
        }
    }
    else
    {
        CheckDuplicateEnumFallback(sym, sig, ctx);
    }
}

} // namespace angel_lsp::analysis::rules
