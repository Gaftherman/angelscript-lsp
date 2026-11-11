#include "analysis/resolution/ExpressionTypeResolver.h"

#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/Constants.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

thread_local std::vector<ActiveLocalDefKey> s_activeLocalDefs;

ActiveLocalDefGuard::~ActiveLocalDefGuard()
{
    if (pushed && !s_activeLocalDefs.empty())
    {
        s_activeLocalDefs.pop_back();
    }
}

static bool IsLocalDefActive(const LocalDefinition& def) noexcept
{
    return std::any_of(s_activeLocalDefs.begin(), s_activeLocalDefs.end(), [&](const ActiveLocalDefKey& k)
                       { return k.line == def.startLine && k.col == def.startCharacter && k.name == def.name; });
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
    return !rhsType.empty() && rhsType != "void" && rhsType != "null" &&
           rhsType != constants::k_initializerListTypeSentinel && !ExpressionTypeCache::IsPoisonType(rhsType);
}

/**
 * @brief Formats a deduced variable type, appending handle '@' decoration if declared as a handle.
 * @param[in] def Local definition containing type and handle qualifier flags.
 * @param[in] rhsType Deduced right-hand-side expression type name.
 * @return Formatted type string matching the definition's handle characteristics.
 */
static std::string FormatDeducedType(const LocalDefinition& def, std::string rhsType)
{
    if ((def.isHandleType || IsHandleTypeView(def.typeName)) && !IsHandleTypeView(rhsType))
    {
        rhsType += '@';
    }
    return rhsType;
}

static std::string TryDeduceAutoDefinition(const LocalDefinition& def, const ExpressionTypeContext& ctx)
{
    if (ctx.nodeIndex)
    {
        const auto declFacts = BuildDeclarationSiteFacts(*ctx.nodeIndex, ctx.sourceCode);
        for (const auto& dec : declFacts.declarators)
        {
            if (dec.hasValue && dec.nameRange.startLine == def.startLine &&
                dec.nameRange.startCharacter == def.startCharacter)
            {
                std::string rhsType = ResolveExpressionTypeAt(dec.valueOperand.bytes, ctx);
                if (!IsValidDeducedRhsType(rhsType))
                {
                    return def.typeName;
                }
                std::string finalType = FormatDeducedType(def, std::move(rhsType));
                const_cast<LocalDefinition&>(def).typeName = finalType;
                return finalType;
            }
        }
    }
    return def.typeName;
}

std::string ResolveLocalDefinitionType(const LocalDefinition& def, const ExpressionTypeContext& ctx)
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

    if (s_activeLocalDefs.size() > 8 || IsLocalDefActive(def))
    {
        return "";
    }

    s_activeLocalDefs.push_back({def.name, def.startLine, def.startCharacter});
    ActiveLocalDefGuard guard{true};

    return TryDeduceAutoDefinition(def, ctx);
}

} // namespace angel_lsp::analysis
