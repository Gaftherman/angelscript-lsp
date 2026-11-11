#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/Constants.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

std::string InferTypeFromAst(uint32_t nodeStartByte, uint32_t nodeEndByte, const ExpressionTypeContext& ctx)
{
    if (ctx.nodeIndex)
    {
        const auto declFacts = BuildDeclarationSiteFacts(*ctx.nodeIndex, ctx.sourceCode);
        for (const auto& decl : declFacts.variableDeclarations)
        {
            if (decl.bytes.startByte <= nodeStartByte && nodeEndByte <= decl.bytes.endByte)
            {
                return std::string(decl.typeText);
            }
        }
    }
    return "";
}

std::optional<std::string> ResolveIdentifierDefaultValue(std::string_view name, uint32_t nodeStartByte,
                                                         uint32_t nodeEndByte, const ExpressionTypeContext& ctx)
{
    if (ctx.scope)
    {
        const LocalDefinition* def = analysis::ResolveInScopeAt(ctx.scope, name, nodeStartByte);
        if (def && !def->defaultValue.empty())
        {
            return std::string(parser::UnquoteStringView(def->defaultValue));
        }
    }
    auto candidates =
        analysis::FindSymbolsInScope(name, nodeStartByte, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    for (const auto& sym : candidates)
    {
        if (sym.type == SymbolType::Variable && !sym.GetVariable().defaultValue.empty())
        {
            return std::string(parser::UnquoteStringView(sym.GetVariable().defaultValue));
        }
    }
    if (const auto globals = ctx.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& sym : *globals)
        {
            if (sym.type == SymbolType::Variable && !sym.GetVariable().defaultValue.empty())
            {
                return std::string(parser::UnquoteStringView(sym.GetVariable().defaultValue));
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> EvaluateConstantStringExpr(uint32_t nodeStartByte, uint32_t nodeEndByte,
                                                      const ExpressionTypeContext& ctx)
{
    if (nodeStartByte >= ctx.sourceCode.size() || nodeEndByte > ctx.sourceCode.size() || nodeStartByte >= nodeEndByte)
    {
        return std::nullopt;
    }
    std::string_view text = ctx.sourceCode.substr(nodeStartByte, nodeEndByte - nodeStartByte);
    if (text.starts_with('"') && text.ends_with('"'))
    {
        return std::string(parser::UnquoteStringView(text));
    }

    if (ctx.nodeIndex)
    {
        const auto binFacts = BuildBinaryExpressionFacts(*ctx.nodeIndex, ctx.sourceCode);
        const auto* bin = binFacts.Find(IndexedByteRange{nodeStartByte, nodeEndByte});
        if (bin && bin->op == "+")
        {
            auto left = EvaluateConstantStringExpr(bin->left.bytes.startByte, bin->left.bytes.endByte, ctx);
            auto right = EvaluateConstantStringExpr(bin->right.bytes.startByte, bin->right.bytes.endByte, ctx);
            if (left && right)
            {
                return *left + *right;
            }
        }
    }

    return ResolveIdentifierDefaultValue(text, nodeStartByte, nodeEndByte, ctx);
}

std::string FormatLambdaSignature(uint32_t lambdaStartByte, uint32_t lambdaEndByte, const ExpressionTypeContext& ctx)
{
    if (lambdaStartByte < ctx.sourceCode.size() && lambdaEndByte <= ctx.sourceCode.size())
    {
        std::string_view span = ctx.sourceCode.substr(lambdaStartByte, lambdaEndByte - lambdaStartByte);
        size_t openParen = span.find('(');
        size_t closeParen = span.find(')', openParen);
        if (openParen != std::string_view::npos && closeParen != std::string_view::npos && closeParen >= openParen)
        {
            return "(anonymous function) function" + std::string(span.substr(openParen, closeParen - openParen + 1));
        }
    }
    return "(anonymous function) function()";
}

std::string DeduceLocalDefinitionHoverType(const LocalDefinition& def, IndexedByteRange hoverNode,
                                           const ExpressionTypeContext& ctx)
{
    if (!def.typeName.empty() && analysis::CleanBaseTypeView(def.typeName) != "auto")
    {
        return def.typeName;
    }
    std::string inferred = analysis::ResolveLocalDefinitionType(def, ctx);
    if (inferred.empty())
    {
        inferred = InferTypeFromAst(hoverNode.startByte, hoverNode.endByte, ctx);
    }
    return !inferred.empty() ? inferred : def.typeName;
}

std::string DeduceAutoDeclaratorType(IndexedByteRange declaratorRange, IndexedByteRange hoverNode,
                                     const ExpressionTypeContext& ctx)
{
    if (ctx.nodeIndex)
    {
        const auto declFacts = BuildDeclarationSiteFacts(*ctx.nodeIndex, ctx.sourceCode);
        for (const auto& d : declFacts.declarators)
        {
            if (d.bytes.startByte == declaratorRange.startByte && d.bytes.endByte == declaratorRange.endByte)
            {
                if (d.hasValue)
                {
                    std::string res = ResolveExpressionTypeAt(d.valueOperand.bytes, ctx);
                    if (!res.empty() && res != "void" && res != "null" &&
                        res != constants::k_initializerListTypeSentinel &&
                        !analysis::ExpressionTypeCache::IsPoisonType(res))
                    {
                        return res;
                    }
                }
                if (d.hasArguments && d.argument.present)
                {
                    std::string res = ResolveExpressionTypeAt(d.argument.operand.bytes, ctx);
                    if (!res.empty() && res != "void" && res != "null")
                    {
                        return res;
                    }
                }
                break;
            }
        }
    }
    return "";
}

} // namespace angel_lsp::analysis
