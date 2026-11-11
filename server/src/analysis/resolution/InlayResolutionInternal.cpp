#include "analysis/resolution/InlayResolutionInternal.h"

#include "analysis/documentation/SignatureFormatter.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

const std::vector<ParameterInformation>* GetParametersIfCallable(const Symbol& sym) noexcept
{
    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return &sym.GetFunction().parameters;
    }
    if (sym.type == SymbolType::Funcdef && std::holds_alternative<FuncdefSignature>(sym.signature))
    {
        return &sym.GetFuncdef().parameters;
    }
    return nullptr;
}

size_t CountRequiredParameters(const std::vector<ParameterInformation>& parameters) noexcept
{
    size_t minRequired = 0;
    for (const auto& p : parameters)
    {
        if (p.defaultValue.empty() && !IsVarargParameter(p))
        {
            minRequired++;
        }
    }
    return minRequired;
}

ResolvedCallee ExtractCalleeFromSymbol(const Symbol* sym)
{
    if (!sym)
    {
        return {};
    }
    const auto* params = GetParametersIfCallable(*sym);
    std::optional<SourceRange> selRange;
    if (sym->selectionRange.startLine != sym->selectionRange.endLine ||
        sym->selectionRange.startCharacter != sym->selectionRange.endCharacter)
    {
        selRange = sym->selectionRange;
    }
    else if (sym->startLine != sym->endLine || sym->startCharacter != sym->endCharacter)
    {
        selRange = SourceRange{sym->startLine, sym->startCharacter, sym->endLine, sym->endCharacter};
    }
    return {sym->fileUri, sym->name, params ? *params : std::vector<ParameterInformation>{}, selRange};
}

const Symbol* SelectCandidateByArgCount(const std::vector<Symbol>& candidateSymbols, size_t numArgs) noexcept
{
    const Symbol* bestSym = nullptr;
    for (const auto& sym : candidateSymbols)
    {
        const auto* params = GetParametersIfCallable(sym);
        if (!params)
        {
            continue;
        }
        size_t minRequiredArgs = CountRequiredParameters(*params);
        if (numArgs >= minRequiredArgs && numArgs <= params->size())
        {
            if (!bestSym || params->size() == numArgs ||
                (GetParametersIfCallable(*bestSym) && GetParametersIfCallable(*bestSym)->size() < numArgs))
            {
                bestSym = &sym;
                if (params->size() == numArgs)
                {
                    break;
                }
            }
        }
    }
    return bestSym;
}

const Symbol* SelectFallbackCandidate(const std::vector<Symbol>& candidateSymbols) noexcept
{
    const Symbol* bestSym = nullptr;
    size_t maxParams = 0;
    for (const auto& sym : candidateSymbols)
    {
        const auto* params = GetParametersIfCallable(sym);
        size_t pCount = params ? params->size() : 0;
        if (!bestSym || pCount > maxParams)
        {
            bestSym = &sym;
            maxParams = pCount;
        }
    }
    return bestSym;
}

} // namespace angel_lsp::analysis
