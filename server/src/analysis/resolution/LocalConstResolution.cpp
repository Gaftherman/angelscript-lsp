#include "analysis/resolution/LocalConstResolution.h"

#include "analysis/resolution/CallCandidates.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "parser/Keywords.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"

#include <algorithm>
#include <span>
#include <vector>

namespace angel_lsp::analysis
{

/** @brief Checks operator targets against the selected local's binding. */
class LocalConstUseCheck
{
  public:
    const SemanticMediator& mediator;  ///< Current semantic context.
    const LocalDefinition& definition; ///< Binding of the declaration receiving the edit.
    const Scope& owner;                ///< Scope that owns the binding.

    /**
     * @brief Resolves an unqualified identifier to the selected local at the point of use.
     * @param[in] name Identifier spelled at the use site.
     * @param[in] byte Byte offset of the use.
     * @param[in] point Line and byte column of the use.
     * @return The lexical scope of a matching later use, or nullptr.
     */
    [[nodiscard]] const Scope* ScopeOfUse(std::string_view name, uint32_t byte, IndexedSourcePoint point) const
    {
        if (byte <= definition.startByte || name != definition.name ||
            !ScopeContains(owner, point.line, point.character))
        {
            return nullptr;
        }
        const Scope* scope = mediator.ScopeAtPoint(point.line, point.character);
        return ResolveInScopeAt(scope, name, byte) == &definition ? scope : nullptr;
    }

    /**
     * @brief Tests an operator operand for storage rooted in the selected local.
     * @param[in] sites Indexed operator expression graph.
     * @param[in] target Expression ID of the operand.
     * @return True when the operand selects the local or storage reached through it.
     */
    [[nodiscard]] bool ReachesLocal(const IndexedOperatorSites& sites, uint32_t target) const
    {
        std::vector<uint32_t> pending{target};
        while (!pending.empty())
        {
            const uint32_t id = pending.back();
            pending.pop_back();
            if (id >= sites.exprs.size())
            {
                continue;
            }
            const auto& expr = sites.exprs[id];
            switch (expr.kind)
            {
            case IndexedExprKind::Identifier:
                if (!expr.isScoped && ScopeOfUse(expr.text, expr.startByte, {expr.line, expr.character}))
                {
                    return true;
                }
                break;
            case IndexedExprKind::Member:
            case IndexedExprKind::Index:
                pending.push_back(expr.left);
                break;
            case IndexedExprKind::Unary:
                if (expr.isHandle)
                {
                    pending.push_back(expr.left);
                }
                break;
            case IndexedExprKind::Ternary:
                pending.push_back(expr.extra);
                pending.push_back(expr.right);
                break;
            default:
                break;
            }
        }
        return false;
    }
};

/** @brief Checks whether a call parameter permits qualification of its argument. */
static bool ParameterAcceptsConst(const ParameterInformation& parameter) noexcept
{
    return !IsOutParameter(parameter) && (parameter.isConst || (!parameter.isReference && !parameter.isHandle));
}

/** @brief Resolves the callable visible at the use, including lexical namespaces and function variables. */
static std::vector<Symbol> FindConstUseCandidates(const IndexedCallContext& call, const Scope* scope,
                                                  const SemanticMediator& mediator)
{
    const auto source = mediator.GetSourceCode();
    const auto& symbols = mediator.GetSymbolTable();
    if (call.kind == CallContextKind::Direct)
    {
        const auto callee = utils::SliceSource(source, call.calleeStartByte, call.calleeEndByte);
        const auto* local = ResolveInScopeAt(scope, callee, call.calleeStartByte);
        if (!local || local->kind == LocalDefinitionKind::Function || local->kind == LocalDefinitionKind::Method)
        {
            auto visible = FindSymbolsInScope(callee, call.calleeStartByte, source, symbols, mediator.GetNodeIndex());
            if (std::ranges::any_of(
                    visible, [](const Symbol& symbol)
                    { return symbol.type == SymbolType::Function || symbol.type == SymbolType::Funcdef; }))
            {
                return visible;
            }
        }
    }
    return ResolveCallCandidates(call, source, scope, symbols);
}

/** @brief Checks every resolved candidate when a call may require mutable storage. */
static bool CallAcceptsConst(const IndexedLocalConstUse& use, const Scope* scope, const SemanticMediator& mediator)
{
    const auto source = mediator.GetSourceCode();
    const auto candidates = FindConstUseCandidates(use.call, scope, mediator);
    const auto parameterName = utils::SliceSource(source, use.parameterName.startByte, use.parameterName.endByte);
    bool hasMatchingParameter = false;
    for (const auto& candidate : candidates)
    {
        if (use.isReceiver)
        {
            if (candidate.type != SymbolType::Function || !candidate.GetFunction().modifiers.isConst)
            {
                return false;
            }
            hasMatchingParameter = true;
            continue;
        }
        std::span<const ParameterInformation> parameters;
        if (candidate.type == SymbolType::Function)
        {
            parameters = candidate.GetFunction().parameters;
        }
        else if (candidate.type == SymbolType::Funcdef)
        {
            parameters = candidate.GetFuncdef().parameters;
        }

        auto parameter = parameters.end();
        if (!parameterName.empty())
        {
            parameter = std::ranges::find_if(parameters, [parameterName](const ParameterInformation& info)
                                             { return info.name == parameterName; });
        }
        else if (use.argumentIndex < parameters.size())
        {
            parameter = parameters.begin() + use.argumentIndex;
        }
        if (parameter != parameters.end())
        {
            if (!ParameterAcceptsConst(*parameter))
            {
                return false;
            }
            hasMatchingParameter = true;
        }
    }
    return hasMatchingParameter;
}

bool CanQualifyLocalConst(const IndexedLocalConstDeclaration& declaration, const LocalConstFacts& facts,
                          const SemanticMediator& mediator)
{
    const auto source = mediator.GetSourceCode();
    const auto& name = declaration.name;
    const auto nameText = utils::SliceSource(source, name.bytes.startByte, name.bytes.endByte);
    const auto* scope = mediator.ScopeAtPoint(name.range.startLine, name.range.startCharacter);
    const Scope* owner = nullptr;
    const auto* definition = ResolveInScopeAt(scope, nameText, name.bytes.startByte, &owner);
    if (!definition || definition->startByte != name.bytes.startByte ||
        definition->kind != LocalDefinitionKind::Variable || definition->isConst || !IsDeclaredInFunctionScope(owner))
    {
        return false;
    }

    const auto* index = mediator.GetNodeIndex();
    if (index && !index->ErrorRanges().empty())
    {
        const auto& lines = index->Lines();
        const size_t end = lines.starts.empty() ? utils::PositionToOffset(source, owner->endLine, owner->endCharacter,
                                                                          utils::PositionEncoding::Utf8)
                                                : lines.StartOffset(source, owner->endLine) + owner->endCharacter;
        if (index->SpanHasSyntaxError(name.bytes.endByte, static_cast<uint32_t>(end)))
        {
            return false;
        }
    }

    const LocalConstUseCheck check{mediator, *definition, *owner};
    if (const auto* operators = mediator.GetScopeRoot()->GetOperatorSites())
    {
        for (const auto& site : operators->sites)
        {
            if (!ScopeContains(*owner, site.scopeLine, site.scopeCharacter))
            {
                continue;
            }
            const bool isAssignment = site.kind == OperatorSiteKind::Assignment;
            const bool isStep =
                site.kind == OperatorSiteKind::Unary && (site.op == parser::keywords::operators::PlusPlus ||
                                                         site.op == parser::keywords::operators::MinusMinus);
            const bool initializesHandle = site.kind == OperatorSiteKind::ImplicitConversion && site.isTargetHandle;
            if (((isAssignment || isStep || initializesHandle) && check.ReachesLocal(*operators, site.exprId)) ||
                (isAssignment && site.leftIsHandle && check.ReachesLocal(*operators, site.rhsExprId)))
            {
                return false;
            }
        }
    }

    for (const auto& use : facts.Uses())
    {
        const auto& storage = use.storage;
        const auto useName = utils::SliceSource(source, storage.bytes.startByte, storage.bytes.endByte);
        if (const auto* useScope = check.ScopeOfUse(useName, storage.bytes.startByte,
                                                    {storage.range.startLine, storage.range.startCharacter});
            useScope && (use.takesAddress || !CallAcceptsConst(use, useScope, mediator)))
        {
            return false;
        }
    }
    return true;
}

} // namespace angel_lsp::analysis
