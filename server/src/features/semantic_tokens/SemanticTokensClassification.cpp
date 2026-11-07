#include "features/semantic_tokens/SemanticTokensInternal.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace angel_lsp::features
{

void CollectReferenceKinds(const analysis::Scope* scope,
                           ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& out)
{
    for (const auto& def : scope->definitions)
    {
        out[PositionKey(def.startLine, def.startCharacter)] = def.kind;
    }

    for (const auto& ref : scope->references)
    {
        if (ref.isMemberAccess)
        {
            continue;
        }

        if (const analysis::LocalDefinition* def = analysis::ResolveInScope(scope, ref.name))
        {
            out[PositionKey(ref.startLine, ref.startCharacter)] = def->kind;
        }
    }

    for (const auto& child : scope->children)
    {
        CollectReferenceKinds(child.get(), out);
    }
}

uint32_t
ResolveReferenceTokenType(uint32_t line, uint32_t column,
                          const ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind>& referenceKinds,
                          uint32_t defaultType)
{
    auto resolved = referenceKinds.find(PositionKey(line, column));
    if (resolved == referenceKinds.end())
    {
        return defaultType;
    }
    switch (resolved->second)
    {
    case analysis::LocalDefinitionKind::Variable:
        return analysis::Type_Variable;
    case analysis::LocalDefinitionKind::Parameter:
        return analysis::Type_Parameter;
    case analysis::LocalDefinitionKind::Field:
        return analysis::Type_Property;
    case analysis::LocalDefinitionKind::Function:
        return analysis::Type_Function;
    case analysis::LocalDefinitionKind::Method:
        return analysis::Type_Method;
    case analysis::LocalDefinitionKind::Type:
        return analysis::Type_Type;
    case analysis::LocalDefinitionKind::Constant:
        return analysis::Type_EnumMember;
    default:
        return defaultType;
    }
}

std::optional<uint32_t> ClassifyReservedKeywordOrModifier(std::string_view tokenText) noexcept
{
    static constexpr std::string_view k_declarationKeywords[] = {"class",     "interface", "mixin",
                                                                 "namespace", "funcdef",   "typedef"};
    for (const auto kw : k_declarationKeywords)
    {
        if (tokenText == kw)
        {
            return analysis::Type_Keyword;
        }
    }
    static constexpr std::string_view k_modifiers[] = {"const", "final", "override", "private", "protected"};
    for (const auto mod : k_modifiers)
    {
        if (tokenText == mod)
        {
            return analysis::Type_Modifier;
        }
    }
    return std::nullopt;
}

bool ShouldSuppressScopedLeafToken(const analysis::IndexedHighlightCapture& capture, std::string_view tokenText,
                                   const TokenRefinementContext& ctx)
{
    if (!capture.isBaseListScopedLeaf)
    {
        return false;
    }

    const std::string_view qualText =
        utils::SliceSource(ctx.request.sourceCode, capture.qualifierStartByte, capture.qualifierEndByte);
    if (qualText.empty())
    {
        return false;
    }

    const auto qualSyms = ctx.request.symbolTable.FindSymbolsPtr(qualText);
    if (qualSyms && !qualSyms->empty())
    {
        const bool isNamespace = std::any_of(qualSyms->begin(), qualSyms->end(), [](const analysis::Symbol& s)
                                             { return s.type == analysis::SymbolType::Namespace; });
        if (!isNamespace)
        {
            return true;
        }
    }

    if (ctx.request.symbolTable.FindMemberSymbolPtr(qualText, tokenText))
    {
        return false;
    }

    // FindMemberSymbolPtr failed; the scoped leaf token does not exist in the qualifier scope.
    return true;
}

} // namespace angel_lsp::features
