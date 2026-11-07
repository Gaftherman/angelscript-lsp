#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/InlayResolutionInternal.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

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
    TSNode declNode = RecoverNodeAt(exprCtx, autoDeclRange.startByte, autoDeclRange.endByte);
    if (ts_node_is_null(declNode))
    {
        return "";
    }

    std::string deduced;
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(declNode) == gs.symVariableDeclaration)
    {
        parser::ForEachChildNodeUntil(declNode,
                                      [&](TSNode child)
                                      {
                                          if (ts_node_symbol(child) == gs.symVariableDeclarator)
                                          {
                                              deduced = DeduceAutoDeclaratorType(
                                                  IndexedByteRange{ts_node_start_byte(child), ts_node_end_byte(child)},
                                                  hoverNode, exprCtx);
                                              return false;
                                          }
                                          return true;
                                      });
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
    TSNode lambdaNode = RecoverNodeAt(exprCtx, lambdaRange.startByte, lambdaRange.endByte);

    std::ostringstream oss;
    oss << "```angelscript\n";

    auto targetFuncdef = !ts_node_is_null(lambdaNode)
                             ? FuncdefTargetOfLambda(lambdaNode, exprCtx.symbolTable, exprCtx.sourceCode)
                             : std::nullopt;
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
