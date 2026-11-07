#include "analysis/resolution/OccurrenceAccess.h"

#include "analysis/resolution/DefinitionResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace angel_lsp::analysis
{

OccurrenceAccessClassifier::OccurrenceAccessClassifier(std::string_view uri, std::string_view sourceCode,
                                                       const SymbolTable& symbolTable, const ScopeIndex& scopeIndex)
    : m_uri(uri), m_sourceCode(sourceCode), m_symbolTable(symbolTable), m_rootScope(scopeIndex.GetRoot(uri))
{
}

/**
 * @brief Classifies an occurrence that sits on the target's own declaration.
 * @param[in] target Resolved target symbol.
 * @param[in] range Occurrence range.
 * @param[in] symbolTable Workspace symbol table holding global and member declarations.
 * @param[in] uri Document the occurrence belongs to.
 * @return `Write` for a declared variable or parameter, `Text` for a declared callable or type, or `std::nullopt`
 *         when the occurrence is not a declaration.
 */
static std::optional<OccurrenceAccess> CheckDeclaration(const TargetDescriptor& target, const SourceRange& range,
                                                        const SymbolTable& symbolTable, std::string_view uri)
{
    if (target.kind == TargetKind::Local)
    {
        if (range.startLine == target.localDef.startLine && range.startCharacter == target.localDef.startCharacter)
        {
            return (target.localDef.kind == LocalDefinitionKind::Variable ||
                    target.localDef.kind == LocalDefinitionKind::Parameter)
                       ? OccurrenceAccess::Write
                       : OccurrenceAccess::Text;
        }
    }

    std::shared_ptr<const std::vector<Symbol>> symsPtr;
    if (target.kind == TargetKind::ClassMember && !target.declaringClass.empty())
    {
        symsPtr = symbolTable.FindMemberSymbolPtr(target.declaringClass, target.name);
    }
    else if (!target.qualifiedName.empty())
    {
        symsPtr = symbolTable.FindSymbolsPtr(target.qualifiedName);
        if (!symsPtr || symsPtr->empty())
        {
            symsPtr = symbolTable.FindSymbolsPtr(target.name);
        }
    }
    else
    {
        symsPtr = symbolTable.FindSymbolsPtr(target.name);
    }

    if (symsPtr)
    {
        for (const auto& sym : *symsPtr)
        {
            if (sym.fileUri == uri &&
                (sym.selectionRange.startLine == range.startLine || sym.startLine == range.startLine))
            {
                if (sym.type == SymbolType::Variable)
                {
                    return OccurrenceAccess::Write;
                }
                if (sym.type == SymbolType::Function || sym.type == SymbolType::Funcdef ||
                    sym.type == SymbolType::Class || sym.type == SymbolType::Interface ||
                    sym.type == SymbolType::Typedef || sym.type == SymbolType::Enum)
                {
                    return OccurrenceAccess::Text;
                }
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Reports whether a parameter lets the callee write back into the caller's argument.
 * @param[in] param Declared parameter.
 * @return True for `&out`, `&inout`, and non-const references not marked `&in`.
 */
static bool IsMutableParam(const ParameterInformation& param) noexcept
{
    return param.modifier == ParameterModifier::Out || param.modifier == ParameterModifier::InOut ||
           (param.isReference && !param.isConst && param.modifier != ParameterModifier::In);
}

bool OccurrenceAccessClassifier::IsMutatingArgument(const IndexedCallArgumentRef& argument, const Scope* scope) const
{
    const std::string_view callee = utils::SliceSource(m_sourceCode, argument.calleeStartByte, argument.calleeEndByte);
    if (callee.empty())
    {
        return false;
    }
    const std::string_view receiver =
        utils::SliceSource(m_sourceCode, argument.receiverStartByte, argument.receiverEndByte);
    std::string_view receiverType;
    std::shared_ptr<const std::vector<Symbol>> receiverSymbols;
    const LocalDefinition* receiverDef = (scope && !receiver.empty()) ? ResolveInScope(scope, receiver) : nullptr;
    if (receiverDef && !receiverDef->typeName.empty())
    {
        receiverType = CleanBaseTypeView(receiverDef->typeName);
    }
    else if (!receiver.empty())
    {
        receiverSymbols = m_symbolTable.FindSymbolsPtr(receiver);
        const auto variable =
            receiverSymbols
                ? std::find_if(receiverSymbols->begin(), receiverSymbols->end(), [](const Symbol& sym)
                               { return sym.type == SymbolType::Variable && !sym.GetVariable().typeName.empty(); })
                : std::vector<Symbol>::const_iterator{};
        if (receiverSymbols && variable != receiverSymbols->end())
        {
            receiverType = CleanBaseTypeView(variable->GetVariable().typeName);
        }
    }

    std::shared_ptr<const std::vector<Symbol>> candidates;
    if (!receiverType.empty())
    {
        candidates = m_symbolTable.FindMemberSymbolPtr(receiverType, callee);
    }
    if (!candidates || candidates->empty())
    {
        candidates = m_symbolTable.FindSymbolsPtr(callee);
    }
    if (!candidates || candidates->empty())
    {
        candidates = m_symbolTable.FindSymbolsPtr(utils::LastScopeSegment(callee));
    }
    return candidates && std::any_of(candidates->begin(), candidates->end(),
                                     [&argument](const Symbol& sym)
                                     {
                                         if (sym.type != SymbolType::Function)
                                         {
                                             return false;
                                         }
                                         const auto& parameters = sym.GetFunction().parameters;
                                         return argument.argumentIndex < parameters.size() &&
                                                IsMutableParam(parameters[argument.argumentIndex]);
                                     });
}

OccurrenceAccess OccurrenceAccessClassifier::Classify(const TargetDescriptor& target, const SourceRange& range) const
{
    if (auto declKind = CheckDeclaration(target, range, m_symbolTable, m_uri))
    {
        return *declKind;
    }

    auto typeOrNsSyms = m_symbolTable.FindSymbolsPtr(target.name);
    if (typeOrNsSyms && !typeOrNsSyms->empty() &&
        std::all_of(typeOrNsSyms->begin(), typeOrNsSyms->end(),
                    [](const Symbol& s)
                    {
                        return s.type == SymbolType::Class || s.type == SymbolType::Interface ||
                               s.type == SymbolType::Typedef || s.type == SymbolType::Namespace ||
                               s.type == SymbolType::Enum;
                    }))
    {
        return OccurrenceAccess::Text;
    }

    if (target.isFunction)
    {
        return OccurrenceAccess::Read;
    }

    const Scope* scope =
        m_rootScope ? FindInnermostScope(m_rootScope.get(), range.startLine, range.startCharacter) : nullptr;
    const LocalReference* reference = nullptr;
    for (const Scope* cur = scope; cur && !reference; cur = cur->parent)
    {
        reference = FindReferenceAt(cur, range.startLine, range.startCharacter);
    }
    if (!reference)
    {
        return OccurrenceAccess::Read;
    }
    if (reference->isWriteTarget)
    {
        return OccurrenceAccess::Write;
    }

    const IndexedReferenceReceivers* receivers =
        m_rootScope->enclosingContext ? m_rootScope->enclosingContext->receivers.get() : nullptr;
    if (!receivers)
    {
        return OccurrenceAccess::Read;
    }
    // A member tail is passed as its whole `a.b` expression; any other reference is passed as itself, so in
    // `f(x.b)` only `b` can match the argument span.
    const bool isMemberTail = reference->isMemberAccess && reference->qualifierStartByte < reference->qualifierEndByte;
    const uint32_t exprStart = isMemberTail ? reference->qualifierStartByte : reference->startByte;
    const uint32_t exprEnd = isMemberTail ? reference->qualifierEndByte : reference->endByte;
    const auto& arguments = receivers->callArguments;
    for (auto it = std::lower_bound(arguments.begin(), arguments.end(), exprStart,
                                    [](const IndexedCallArgumentRef& arg, uint32_t start)
                                    { return arg.argStartByte < start; });
         it != arguments.end() && it->argStartByte == exprStart; ++it)
    {
        if (it->argEndByte == exprEnd && IsMutatingArgument(*it, scope))
        {
            return OccurrenceAccess::Write;
        }
    }
    return OccurrenceAccess::Read;
}

} // namespace angel_lsp::analysis
