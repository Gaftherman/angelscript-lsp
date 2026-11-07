#include "analysis/resolution/InlayResolutionInternal.h"

#include "analysis/documentation/SignatureFormatter.h"
#include "parser/ASTUtils.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

static TSNode RecoverNodeFromRoot(TSNode root, uint32_t startByte, uint32_t endByte) noexcept
{
    if (ts_node_is_null(root))
    {
        return TSNode{};
    }
    TSNode node = ts_node_named_descendant_for_byte_range(root, startByte, endByte);
    if (ts_node_is_null(node))
    {
        node = ts_node_descendant_for_byte_range(root, startByte, endByte);
    }
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    TSNode match = TSNode{};
    parser::ForEachAncestor(node,
                            [&](TSNode ancestor)
                            {
                                if (ts_node_start_byte(ancestor) == startByte && ts_node_end_byte(ancestor) == endByte)
                                {
                                    if (ts_node_is_named(ancestor) || ts_node_is_null(match))
                                    {
                                        match = ancestor;
                                    }
                                    return true;
                                }
                                if (ts_node_start_byte(ancestor) < startByte || ts_node_end_byte(ancestor) > endByte)
                                {
                                    return false;
                                }
                                return true;
                            });
    return !ts_node_is_null(match) ? match : node;
}

TSNode RecoverNodeAt(const NodeIndex& index, uint32_t startByte, uint32_t endByte) noexcept
{
    return RecoverNodeFromRoot(index.Root(), startByte, endByte);
}

TSNode RecoverNodeAt(const ExpressionTypeContext& ctx, uint32_t startByte, uint32_t endByte) noexcept
{
    if (ctx.nodeIndex)
    {
        return RecoverNodeFromRoot(ctx.nodeIndex->Root(), startByte, endByte);
    }
    if (!ts_node_is_null(ctx.rootNode))
    {
        return RecoverNodeFromRoot(ctx.rootNode, startByte, endByte);
    }
    return TSNode{};
}

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
