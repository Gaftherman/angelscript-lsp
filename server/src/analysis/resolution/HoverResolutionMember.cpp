#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/index/ReturnSiteFacts.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
std::optional<HoverCandidateSymbols> ResolveMemberAccess(IndexedByteRange receiverRange, std::string_view memberName,
                                                         const ExpressionTypeContext& exprCtx,
                                                         std::string_view virtualHostClass,
                                                         const config::ServerConfig* config)
{
    std::string receiverTypeName = ResolveReceiverTypeName(receiverRange, exprCtx, virtualHostClass, config);
    if (receiverTypeName.empty())
    {
        return std::nullopt;
    }

    std::vector<Symbol> memberSymbols = CollectReceiverMemberSymbols(receiverTypeName, memberName, exprCtx.symbolTable);

    std::string accessorPropertyType;
    if (memberSymbols.empty())
    {
        const int accessorMode = config ? config->engine.propertyAccessorMode : 2;
        if (accessorMode >= 2)
        {
            memberSymbols = FindPropertyAccessors(receiverTypeName, std::string(memberName), exprCtx.symbolTable,
                                                  accessorMode == 3);
            accessorPropertyType = PropertyTypeFromAccessors(memberSymbols);
        }
    }

    if (memberSymbols.empty())
    {
        return std::nullopt;
    }

    return HoverCandidateSymbols{std::move(memberSymbols), std::move(accessorPropertyType)};
}

std::string ResolveAutoHoverMarkdown(IndexedByteRange autoDeclRange, IndexedByteRange hoverNode, bool isAutoHandle,
                                     const ExpressionTypeContext& exprCtx)
{
    std::string deduced;
    if (exprCtx.nodeIndex)
    {
        const auto declFacts = BuildDeclarationSiteFacts(*exprCtx.nodeIndex, exprCtx.sourceCode);
        for (const auto& decl : declFacts.variableDeclarations)
        {
            if (decl.bytes.startByte == autoDeclRange.startByte && decl.bytes.endByte == autoDeclRange.endByte)
            {
                for (uint32_t i = 0; i < decl.declaratorCount; ++i)
                {
                    const auto& d = declFacts.declarators[decl.declaratorBegin + i];
                    deduced = DeduceAutoDeclaratorType(d.bytes, hoverNode, exprCtx);
                    if (!deduced.empty())
                    {
                        break;
                    }
                }
                break;
            }
        }
    }

    if (!deduced.empty() && deduced != "auto")
    {
        if (isAutoHandle && !IsHandleTypeView(deduced))
        {
            deduced += '@';
        }
        return "```angelscript\n" + std::string(isAutoHandle ? "auto@" : "auto") + ": " + deduced + "\n```";
    }
    return "";
}

std::string FormatLambdaHover(IndexedByteRange lambdaRange, const ExpressionTypeContext& exprCtx,
                              const HoverDocLookupContext& docCtx)
{
    std::ostringstream oss;
    oss << "```angelscript\n";

    std::optional<Symbol> targetFuncdef;
    if (exprCtx.nodeIndex)
    {
        const auto returnFacts = BuildReturnSiteFacts(*exprCtx.nodeIndex, exprCtx.sourceCode);
        for (const auto& ret : returnFacts.returns)
        {
            if (ret.owner == ReturnOwnerKind::Lambda && ret.bytes.startByte >= lambdaRange.startByte &&
                ret.bytes.endByte <= lambdaRange.endByte)
            {
                targetFuncdef = FuncdefTargetOfLambda(ret.lambdaTarget, exprCtx.symbolTable);
                if (targetFuncdef)
                {
                    break;
                }
            }
        }
    }

    if (targetFuncdef)
    {
        oss << "(anonymous function) -> " << targetFuncdef->name << "\n";
        oss << FormatFunctionDeclaration(*targetFuncdef);
    }
    else
    {
        oss << FormatLambdaSignature(lambdaRange.startByte, lambdaRange.endByte, exprCtx);
    }
    oss << "\n```";

    if (targetFuncdef)
    {
        std::string doc = DocCommentForSymbol(docCtx, *targetFuncdef);
        if (!doc.empty())
        {
            oss << "\n\n---\n\n" << doc;
        }
    }
    return oss.str();
}

} // namespace angel_lsp::analysis
