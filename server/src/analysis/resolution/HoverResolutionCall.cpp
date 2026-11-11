#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CallArgumentFacts.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/index/HoverFactsInternal.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ConversionRankingEngine.h"
#include "analysis/resolution/HoverResolution.h"
#include "analysis/resolution/AstRecoveryInternal.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/TargetResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "config/ServerConfig.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

std::string ResolveReceiverTypeName(IndexedByteRange objectRange, const ExpressionTypeContext& ctx,
                                    std::string_view virtualHostClass, const config::ServerConfig* config)
{
    TSNode objectNode = RecoverNodeAt(ctx, objectRange.startByte, objectRange.endByte);
    if (ts_node_is_null(objectNode))
    {
        return "";
    }
    const config::EngineRuleConfig* engineRules = config ? &config->engineRules : nullptr;
    std::string receiverTypeName = analysis::ResolveReceiverType(
        objectNode, ctx.sourceCode, ctx.symbolTable,
        {ctx.scope, virtualHostClass, ctx.uri, ctx.symbolTable.GetDefaultArrayType(), nullptr, engineRules});
    if (!receiverTypeName.empty() && config && !config->types.arrayTypeName.empty())
    {
        receiverTypeName = analysis::MemberOwnerType(receiverTypeName, config->types.arrayTypeName);
    }

    if (!receiverTypeName.empty())
    {
        receiverTypeName = ctx.symbolTable.QualifyShortTypeName(receiverTypeName);
    }
    return receiverTypeName;
}

std::vector<Symbol> CollectReceiverMemberSymbols(std::string_view receiverTypeName, std::string_view memberName,
                                                 const SymbolTable& symbolTable)
{
    std::vector<Symbol> memberSymbols;
    auto hierarchy = analysis::GetInheritedTypeHierarchy(std::string(receiverTypeName), symbolTable);
    for (const auto& typeName : hierarchy)
    {
        std::string qualifiedMember = utils::JoinScopedName(typeName, memberName);
        auto found = symbolTable.FindSymbolsPtr(qualifiedMember);
        if (found)
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function)
                {
                    bool overriddenLower =
                        std::any_of(memberSymbols.begin(), memberSymbols.end(),
                                    [&](const Symbol& kept) { return analysis::HasSameParameterList(kept, sym); });
                    if (!overriddenLower)
                    {
                        memberSymbols.push_back(sym);
                    }
                }
                else
                {
                    memberSymbols.push_back(sym);
                }
            }
        }
    }
    std::stable_partition(memberSymbols.begin(), memberSymbols.end(),
                          [](const Symbol& s)
                          {
                              if (s.type == SymbolType::Variable &&
                                  std::holds_alternative<VariableSignature>(s.signature))
                              {
                                  return !s.GetVariable().isEnumConstant;
                              }
                              return true;
                          });
    return memberSymbols;
}

std::vector<Symbol> ResolveNamedArgCalleeCandidates(IndexedByteRange callRange, const ExpressionTypeContext& ctx,
                                                    std::string_view virtualHostClass,
                                                    const config::ServerConfig* config)
{
    TSNode callNode = RecoverNodeAt(ctx, callRange.startByte, callRange.endByte);
    if (ts_node_is_null(callNode))
    {
        return {};
    }
    TSNode funcNode = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldFunction);
    if (ts_node_is_null(funcNode) && ts_node_child_count(callNode) > 0)
    {
        funcNode = ts_node_child(callNode, 0);
    }
    if (ts_node_is_null(funcNode))
    {
        return {};
    }

    if (ts_node_symbol(funcNode) == parser::GetGrammarSymbols().symMemberExpression)
    {
        TSNode objectNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldObject);
        TSNode memberNode = parser::GetChildByField(funcNode, parser::GetGrammarSymbols().fieldMember);
        if (!ts_node_is_null(objectNode) && !ts_node_is_null(memberNode))
        {
            std::string receiverTypeName =
                ResolveReceiverTypeName(IndexedByteRange{ts_node_start_byte(objectNode), ts_node_end_byte(objectNode)},
                                        ctx, virtualHostClass, config);
            std::string memberName(parser::NodeText(memberNode, ctx.sourceCode));
            return CollectReceiverMemberSymbols(receiverTypeName, memberName, ctx.symbolTable);
        }
    }
    else
    {
        std::string funcName(parser::NodeText(funcNode, ctx.sourceCode));
        if (const auto found = ctx.symbolTable.FindSymbolsPtr(funcName))
        {
            return *found;
        }
    }
    return {};
}

std::optional<Symbol> ResolveConstructorForDeclarator(uint32_t declStartByte, uint32_t declEndByte,
                                                      std::string_view typeName, const ExpressionTypeContext& ctx)
{
    TSNode declarator = RecoverNodeAt(ctx, declStartByte, declEndByte);
    if (!ts_node_is_null(declarator) && ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
    {
        declarator = parser::GetParent(declarator);
    }
    if (ts_node_is_null(declarator) || ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
    {
        return std::nullopt;
    }

    TSNode argList = analysis::ResolveArgumentListNode(declarator);
    if (ts_node_is_null(argList))
    {
        return std::nullopt;
    }

    std::string_view baseName = analysis::CleanBaseTypeView(typeName);
    if (baseName.empty() || parser::primitives::IsPrimitive(baseName))
    {
        return std::nullopt;
    }

    auto candidates =
        analysis::CollectConstructorCandidates(std::string(baseName), declarator, ctx.sourceCode, ctx.symbolTable);
    if (candidates.empty())
    {
        return std::nullopt;
    }

    if (candidates.size() == 1)
    {
        return candidates[0];
    }

    auto argTypes = analysis::ExtractCallArgumentTypes(argList, {ctx.scope, ctx.symbolTable, ctx.sourceCode, ctx.uri});
    auto match = analysis::ResolveBestOverload(candidates, argTypes, ctx.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }
    if (match.priorityCandidate != nullptr)
    {
        return *match.priorityCandidate;
    }

    const Symbol* fallback = analysis::FindBestFallbackOverload(candidates, argTypes, ctx.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }

    return candidates[0];
}

std::string ResolveConstructorSignature(uint32_t nodeStartByte, uint32_t nodeEndByte, std::string_view typeName,
                                        const ExpressionTypeContext& ctx)
{
    if (auto ctorSym = ResolveConstructorForDeclarator(nodeStartByte, nodeEndByte, typeName, ctx))
    {
        return analysis::FormatFunctionDeclaration(*ctorSym);
    }
    return "";
}

std::optional<Symbol> ResolveCallOverload(uint32_t startByte, uint32_t endByte, const std::vector<Symbol>& candidates,
                                          const ExpressionTypeContext& ctx)
{
    if (candidates.empty())
    {
        return std::nullopt;
    }
    if (candidates.size() == 1)
    {
        return candidates.front();
    }

    TSNode node = RecoverNodeAt(ctx, startByte, endByte);
    TSNode callNode = FindEnclosingCallNode(node);
    if (ts_node_is_null(callNode))
    {
        return std::nullopt;
    }

    std::vector<std::string> argTypes =
        analysis::ExtractCallArgumentTypes(callNode, {ctx.scope, ctx.symbolTable, ctx.sourceCode, ctx.uri});

    auto match = analysis::ResolveBestOverload(candidates, argTypes, ctx.symbolTable);
    if (match.bestCandidate != nullptr)
    {
        return *match.bestCandidate;
    }
    if (match.priorityCandidate != nullptr)
    {
        return *match.priorityCandidate;
    }

    const Symbol* fallback = analysis::FindBestFallbackOverload(candidates, argTypes, ctx.symbolTable);
    if (fallback != nullptr)
    {
        return *fallback;
    }
    if (!candidates.empty())
    {
        return candidates.front();
    }
    return std::nullopt;
}

} // namespace angel_lsp::analysis
