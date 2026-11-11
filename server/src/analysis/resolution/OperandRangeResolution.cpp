#include "analysis/resolution/OperandRangeResolution.h"

#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves the type of the expression that spans the given byte range.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context used for resolution.
 * @return Resolved type spelling, or an empty string when recovery fails.
 */
std::string ResolveExpressionType(IndexedByteRange bytes, const ExpressionTypeContext& ctx)
{
    return ResolveExpressionTypeAt(bytes, ctx);
}

/**
 * @brief Reports the data type an expression names instead of a value.
 * @param[in] bytes Byte span of the expression.
 * @param[in] ctx Expression context used for resolution.
 * @return Data type name, or nullopt when the expression is a value or recovery fails.
 */
std::optional<std::string> IsBareDataType(IndexedByteRange bytes, const ExpressionTypeContext& ctx)
{
    if (bytes.startByte >= ctx.sourceCode.size() || bytes.endByte > ctx.sourceCode.size() ||
        bytes.startByte >= bytes.endByte)
    {
        return std::nullopt;
    }
    std::string_view text = ctx.sourceCode.substr(bytes.startByte, bytes.endByte - bytes.startByte);
    TrimTypeWhitespace(text);
    return IsBareDataType(text, bytes.startByte, ctx);
}

static bool HasFunctionSymbolOnly(const std::vector<Symbol>& syms)
{
    bool hasFunc = false;
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Function)
        {
            hasFunc = true;
        }
        else if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
        {
            return false;
        }
    }
    return hasFunc;
}

static bool IsLocalVariableOrFieldInScope(const Scope* scope, std::string_view name, uint32_t refByte)
{
    if (scope && !utils::ContainsScopeSeparator(name))
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

static bool IsMemberMethodReference(std::string_view objText, std::string_view memName, IndexedByteRange receiverRange,
                                    const ExpressionTypeContext& ctx)
{
    ReceiverTypeContext recvCtx{ctx.scope, "", ctx.uri};
    recvCtx.nodeIndex = ctx.nodeIndex;
    std::string objType = ResolveReceiverType(receiverRange, ctx.sourceCode, ctx.symbolTable, recvCtx);
    const std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty())
    {
        if (const auto syms = ctx.symbolTable.FindMemberSymbolPtr(std::string(objText), memName))
        {
            return HasFunctionSymbolOnly(*syms);
        }
        return false;
    }
    for (const auto& cls : GetInheritedTypeHierarchy(cleanObj, ctx.symbolTable))
    {
        if (const auto syms = ctx.symbolTable.FindMemberSymbolPtr(cls, memName))
        {
            if (HasFunctionSymbolOnly(*syms))
            {
                return true;
            }
        }
    }
    return false;
}

bool IsFunctionReference(IndexedByteRange bytes, const ExpressionTypeContext& ctx)
{
    if (bytes.startByte >= ctx.sourceCode.size() || bytes.endByte > ctx.sourceCode.size() ||
        bytes.startByte >= bytes.endByte)
    {
        return false;
    }
    std::string_view text = ctx.sourceCode.substr(bytes.startByte, bytes.endByte - bytes.startByte);
    TrimTypeWhitespace(text);
    while (text.starts_with('(') && text.ends_with(')'))
    {
        text.remove_prefix(1);
        text.remove_suffix(1);
        TrimTypeWhitespace(text);
    }
    if (text.empty() || text.ends_with(')'))
    {
        return false;
    }

    const size_t dotPos = text.rfind('.');
    if (dotPos != std::string_view::npos)
    {
        std::string_view objText = text.substr(0, dotPos);
        std::string_view memText = text.substr(dotPos + 1);
        TrimTypeWhitespace(objText);
        TrimTypeWhitespace(memText);
        IndexedByteRange receiverRange{bytes.startByte, static_cast<uint32_t>(bytes.startByte + dotPos)};
        return IsMemberMethodReference(objText, memText, receiverRange, ctx);
    }

    if (IsLocalVariableOrFieldInScope(ctx.scope, text, bytes.startByte))
    {
        return false;
    }
    return HasGlobalFunctionSymbol(text, ctx.symbolTable);
}

} // namespace angel_lsp::analysis
