#include "analysis/resolution/ExpressionTypeResolver.h"

#include "analysis/index/CallArgumentFacts.h"
#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/NodeRecoveryInternal.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeBinary.h"
#include "analysis/resolution/ExpressionTypeCall.h"
#include "analysis/resolution/ExpressionTypeIdentifiers.h"
#include "analysis/resolution/ExpressionTypeIndex.h"
#include "analysis/resolution/ExpressionTypeLiterals.h"
#include "analysis/resolution/ExpressionTypeMember.h"
#include "analysis/resolution/ExpressionTypeTernary.h"
#include "analysis/resolution/ExpressionTypeUnary.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static std::string ResolveExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx);

static TSNode RecoverNodeFromContext(const ExpressionTypeContext& ctx, uint32_t startByte, uint32_t endByte) noexcept
{
    if (ctx.nodeIndex)
    {
        return RecoverNodeAt(*ctx.nodeIndex, startByte, endByte);
    }
    return TSNode{};
}

static std::vector<ContainerInfo> ResolveEnclosingContainers(TSNode exprNode, std::string_view sourceCode,
                                                             const NodeIndex* nodeIndex)
{
    if (nodeIndex && !ts_node_is_null(exprNode))
    {
        return GetEnclosingContainers(
            IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)}, *nodeIndex, sourceCode);
    }
    return {};
}

static std::vector<Symbol> ResolveScopeSymbols(std::string_view name, TSNode exprNode, std::string_view sourceCode,
                                               const SymbolTable& symbolTable, const NodeIndex* nodeIndex)
{
    if (name.empty())
    {
        return {};
    }
    const uint32_t byteOffset = !ts_node_is_null(exprNode) ? ts_node_start_byte(exprNode) : 0;
    return FindSymbolsInScope(name, byteOffset, sourceCode, symbolTable, nodeIndex);
}



static std::string GetTrimmedNodeText(TSNode node, std::string_view sourceCode)
{
    return std::string(utils::TrimView(parser::NodeText(node, sourceCode)));
}

/**
 * @brief Resolves literal expression type.
 */
static std::string ResolveLiteralExpr(TSSymbol nodeSym, TSNode exprNode, std::string_view sourceCode,
                                      std::string_view stringTypeName)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (nodeSym == gs.symStringLiteral)
    {
        return std::string(stringTypeName);
    }
    if (nodeSym == gs.symBooleanLiteral)
    {
        return "bool";
    }
    if (nodeSym == gs.symNullLiteral)
    {
        return "null";
    }
    if (nodeSym == gs.symNumberLiteral)
    {
        return ResolveNumberLiteral(std::string(parser::NodeText(exprNode, sourceCode)));
    }
    return "";
}

/**
 * @brief Resolves parenthesized expression type.
 */
static std::string ResolveParenthesizedExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode inner = UnwrapIndexedParentheses(exprNode, ctx.nodeIndex);
    if (!ts_node_is_null(inner) && ts_node_symbol(inner) != gs.symParenthesizedExpression)
    {
        return ResolveExpressionType(inner, ctx);
    }
    return "";
}

/**
 * @brief Resolves 'this' expression type based on enclosing container hierarchy or mixin host.
 */
static std::string ResolveThisExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const bool isVirtual = ctx.uri.starts_with("angelscript-virtual:") || ctx.uri.starts_with("angelscript-virtual://");
    std::string hostClass = isVirtual ? SymbolTable::ExtractVirtualHostClass(ctx.uri) : "";
    if (hostClass.empty())
    {
        hostClass =
            InferHostClassFromEnclosingMixin(IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)},
                                             ctx.nodeIndex, ctx.sourceCode, ctx.symbolTable);
    }
    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    return ResolveThisType(hostClass, containers);
}

static std::string ResolveScopedIdentifierExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto text = parser::NodeText(exprNode, ctx.sourceCode);
    const auto [qualView, mem] = utils::SplitScopedName(utils::StripGlobalPrefix(text));
    if (!qualView.empty())
    {
        auto wholeSyms = ResolveScopeSymbols(text, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        auto qualSyms = ResolveScopeSymbols(qualView, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        return ResolveScopedIdentifierType(text, wholeSyms, qualSyms, ctx.symbolTable);
    }

    TSNode lastIdentifier = SyntaxOperands(exprNode, ctx.nodeIndex).LastNamed(parser::GetGrammarSymbols().symIdentifier);
    return ts_node_is_null(lastIdentifier) ? std::string() : ResolveExpressionType(lastIdentifier, ctx);
}

/**
 * @brief Resolves member symbols or accessors from the enclosing class hierarchy.
 * @param[in] name Identifier name.
 * @param[in] exprNode AST syntax node.
 * @param[in] ctx Expression type resolution context.
 * @return Matching member symbols or property accessors, if found.
 */
static std::vector<Symbol> ResolveEnclosingContainerMemberSymbols(const std::string& name, TSNode exprNode,
                                                                  const ExpressionTypeContext& ctx)
{
    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    return ResolveEnclosingContainerMemberSymbols(name, containers, ctx.symbolTable, ctx.cache);
}

/**
 * @brief Finds symbol matches for a bare identifier.
 * @param[in] name Identifier name.
 * @param[in] exprNode AST syntax node.
 * @param[in] ctx Expression type resolution context.
 * @return Candidate symbols in lexical, member, or global scope.
 */
static std::vector<Symbol> ResolveIdentifierSymbols(const std::string& name, TSNode exprNode,
                                                    const ExpressionTypeContext& ctx)
{
    auto syms = ResolveScopeSymbols(name, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    if (!syms.empty())
    {
        return syms;
    }

    syms = ResolveEnclosingContainerMemberSymbols(name, exprNode, ctx);
    if (!syms.empty())
    {
        return syms;
    }

    return ctx.symbolTable.FindSymbols(name);
}

/**
 * @brief Host class fallback resolution for identifiers in mixin classes and virtual documents.
 */
static std::string ResolveIdentifierHostFallback(const std::string& name, TSNode exprNode,
                                                 const ExpressionTypeContext& ctx)
{
    std::string hostClass;
    if (ctx.uri.starts_with("angelscript-virtual:") || ctx.uri.starts_with("angelscript-virtual://"))
    {
        hostClass = SymbolTable::ExtractVirtualHostClass(ctx.uri);
    }
    else
    {
        hostClass =
            InferHostClassFromEnclosingMixin(IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)},
                                             ctx.nodeIndex, ctx.sourceCode, ctx.symbolTable);
    }
    return ResolveIdentifierHostFallbackFromHierarchy(name, hostClass, ctx.symbolTable, ctx.cache);
}

static std::string ResolveIdentifierDelegateProperty(std::string_view name, TSNode exprNode,
                                                     const ExpressionTypeContext& ctx)
{
    if (!IsBaseClassDelegateProperty(name, ctx.symbolTable))
    {
        return "";
    }
    const bool isVirtual = ctx.uri.starts_with("angelscript-virtual:") || ctx.uri.starts_with("angelscript-virtual://");
    std::string hostClass = isVirtual ? SymbolTable::ExtractVirtualHostClass(ctx.uri) : "";
    std::string baseClass = ResolveDelegateTargetBaseClass(
        {IndexedByteRange{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode)}, ctx.nodeIndex, ctx.sourceCode,
         ctx.symbolTable, hostClass, nullptr});
    if (!baseClass.empty())
    {
        return MemberOwnerType(baseClass, ctx.arrayTypeName);
    }
    return "";
}

/**
 * @brief Resolves bare identifier expression type.
 */
static std::string ResolveIdentifierExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    std::string name(parser::NodeText(exprNode, ctx.sourceCode));
    if (name == "this")
    {
        return ResolveThisExpr(exprNode, ctx);
    }
    if (ctx.scope)
    {
        const LocalDefinition* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(exprNode));
        if (def)
        {
            std::string dType = ResolveLocalDefinitionType(*def, ctx);
            if (!dType.empty())
            {
                return CleanExpressionType(dType);
            }
        }
    }
    auto syms = ResolveIdentifierSymbols(name, exprNode, ctx);
    if (syms.empty())
    {
        if (auto hostRes = ResolveIdentifierHostFallback(name, exprNode, ctx); !hostRes.empty())
        {
            return hostRes;
        }
    }
    if (std::string firstType = ExtractFirstSymbolTypeOrAccessor(syms, ctx.symbolTable); !firstType.empty())
    {
        return firstType;
    }
    if (auto accessorRes = ResolveIdentifierAccessorFallback(name, ctx.symbolTable); !accessorRes.empty())
    {
        return accessorRes;
    }
    return ResolveIdentifierDelegateProperty(name, exprNode, ctx);
}

/**
 * @brief Resolves binary expression type.
 * @param[in] exprNode Binary expression AST node.
 * @param[in] ctx Expression typing context.
 * @return Resolved expression type string or poison type.
 */
static std::string ResolveBinaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<SyntaxOperands> chain;
    TSNode curr = exprNode;
    while (!ts_node_is_null(curr) && ts_node_symbol(curr) == gs.symBinaryExpression)
    {
        chain.emplace_back(curr, ctx.nodeIndex);
        curr = chain.back().Field(gs.fieldLeft);
    }

    if (chain.empty() || ts_node_is_null(curr))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }

    std::string currentType = ResolveExpressionType(curr, ctx);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        const TSNode opNode = it->Field(gs.fieldOperator);
        const TSNode right = it->Field(gs.fieldRight);
        if (ts_node_is_null(opNode) || ts_node_is_null(right))
        {
            return std::string(ExpressionTypeCache::k_poisonType);
        }

        std::string rightType = ResolveExpressionType(right, ctx);
        if (ExpressionTypeCache::IsPoisonType(currentType) || ExpressionTypeCache::IsPoisonType(rightType))
        {
            return std::string(ExpressionTypeCache::k_poisonType);
        }

        const std::string_view op = parser::OperatorSpelling(opNode);
        currentType = EvaluateBinaryResultType(op, currentType, rightType, ctx);
        if (currentType.empty())
        {
            break;
        }
    }
    return currentType;
}

/**
 * @brief Resolves ternary expression type.
 */
static std::string ResolveTernaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode consequence = syntax.Field(parser::GetGrammarSymbols().fieldConsequence);
    const TSNode alternative = syntax.Field(parser::GetGrammarSymbols().fieldAlternative);
    if (ts_node_is_null(consequence) || ts_node_is_null(alternative))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    std::string t1 = ResolveExpressionType(consequence, ctx);
    std::string t2 = ResolveExpressionType(alternative, ctx);
    if (ExpressionTypeCache::IsPoisonType(t1) || ExpressionTypeCache::IsPoisonType(t2))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (t1.empty())
        return t2;
    if (t2.empty())
        return t1;
    if (t1 == "void" || t2 == "void")
        return "";

    return ResolveTernaryBranchTypes(t1, t2, ctx.symbolTable);
}



/**
 * @brief Resolves member expression type.
 * @param[in] exprNode Member expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved member type, or empty string if unresolved.
 */
static std::string ResolveMemberExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode objNode = syntax.Field(parser::GetGrammarSymbols().fieldObject);
    const TSNode memNode = syntax.Field(parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }

    std::string objType = ResolveExpressionType(objNode, ctx);
    if (ExpressionTypeCache::IsPoisonType(objType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty())
    {
        cleanObj = GetTrimmedNodeText(objNode, ctx.sourceCode);
    }
    if (cleanObj.empty())
    {
        return "";
    }

    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);
    auto propSearchOrder = OrderMemberPropertySearchHierarchy(cleanObj, ctx.symbolTable);
    for (const auto& typeName : propSearchOrder)
    {
        auto found = FindMemberSymbolsInType(typeName, memName, ctx.symbolTable);
        for (const auto& sym : found)
        {
            if (auto resolved = ExtractMemberSymbolType(sym, ctx.symbolTable); !resolved.empty())
            {
                return resolved;
            }
        }
    }
    auto accessors = FindPropertyAccessors(cleanObj, memName, ctx.symbolTable, false);
    if (!accessors.empty())
    {
        std::string pType = PropertyTypeFromAccessors(accessors);
        if (!pType.empty())
        {
            return pType;
        }
    }
    return "";
}


/**
 * @brief Resolves member method call expression type.
 * @param[in] funcNode Function AST node (member_expression).
 * @param[in] argTypes Evaluated argument types.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveMemberCallExpr(TSNode funcNode, const std::vector<std::string>& argTypes,
                                         const ExpressionTypeContext& ctx)
{
    const SyntaxOperands syntax(funcNode, ctx.nodeIndex);
    const TSNode objNode = syntax.Field(parser::GetGrammarSymbols().fieldObject);
    const TSNode memNode = syntax.Field(parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return "";
    }

    std::string objType = ResolveExpressionType(objNode, ctx);
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);
    std::string fallbackObj = GetTrimmedNodeText(objNode, ctx.sourceCode);

    return ResolveMemberCallReturn(
        MemberCallResolutionRequest{objType, memName, fallbackObj, ctx.arrayTypeName},
        argTypes, ctx.symbolTable);
}

/**
 * @brief Resolves non-member or bare function calls.
 * @param[in] exprNode Call expression AST node.
 * @param[in] funcNode Callee AST node.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveBareCallExpr(TSNode exprNode, TSNode funcNode, const std::vector<std::string>& argTypes,
                                       const ExpressionTypeContext& ctx)
{
    std::string funcName = GetTrimmedNodeText(funcNode, ctx.sourceCode);
    if (argTypes.size() == 1 && IsCorePrimitive(funcName))
    {
        return funcName;
    }
    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    std::vector<Symbol> candidates = CollectEnclosingCallCandidates(containers, funcName, ctx.symbolTable, ctx.cache);
    if (candidates.empty())
    {
        std::vector<Symbol> inScope =
            ResolveScopeSymbols(funcName, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        CollectScopeAndGlobalFunctions(funcName, inScope, ctx.symbolTable, candidates);
    }
    if (!candidates.empty())
    {
        return ResolveCallOverloadReturn(candidates, argTypes, ctx.symbolTable);
    }
    return FindClassOrInterfaceName(funcName, ctx.symbolTable);
}

/**
 * @brief Resolves call expression type.
 */
static std::string ResolveCallExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode funcNode = syntax.Callee();
    if (ts_node_is_null(funcNode))
    {
        return "";
    }

    std::vector<std::string> argTypes;
    const TSNode argsNode = syntax.Field(gs.fieldArguments);
    SyntaxOperands(argsNode, ctx.nodeIndex)
        .ForEachNamed(
            [&](TSNode arg)
            {
                argTypes.push_back(ResolveExpressionType(arg, ctx));
                return true;
            });

    if (ts_node_symbol(funcNode) == gs.symMemberExpression)
    {
        if (auto res = ResolveMemberCallExpr(funcNode, argTypes, ctx); !res.empty())
        {
            return res;
        }
    }
    else
    {
        if (auto res = ResolveBareCallExpr(exprNode, funcNode, argTypes, ctx); !res.empty())
        {
            return res;
        }
    }

    std::string calleeType = ResolveExpressionType(funcNode, ctx);
    return ResolveCalleeFuncdefReturn(calleeType, ctx.symbolTable);
}

/**
 * @brief Resolves unary expression type.
 */
static std::string ResolveUnaryExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode operandNode = syntax.Field(gs.fieldOperand);
    if (ts_node_is_null(operandNode) || ts_node_is_error(operandNode) || ts_node_is_missing(operandNode))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    const TSNode operatorNode = syntax.Field(gs.fieldOperator);
    const TSSymbol opSym = !ts_node_is_null(operatorNode) ? ts_node_symbol(operatorNode) : 0;
    if (opSym == gs.tokBang || opSym == gs.tokNot)
    {
        return "bool";
    }
    if (opSym == gs.tokAt)
    {
        std::string targetName = GetTrimmedNodeText(operandNode, ctx.sourceCode);
        std::vector<Symbol> inScope =
            ResolveScopeSymbols(targetName, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        bool isFuncTarget = IsFunctionHandleTargetName(targetName, inScope, ctx.symbolTable);
        std::string operandType = ResolveExpressionType(operandNode, ctx);
        return ResolveUnaryHandle(isFuncTarget, operandType);
    }
    std::string operandType = ResolveExpressionType(operandNode, ctx);
    if (ExpressionTypeCache::IsPoisonType(operandType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (opSym == gs.tokDash)
    {
        return ResolveUnaryMinus(operandType);
    }
    return operandType;
}

/**
 * @brief Resolves constructor call expression type.
 */
static std::string ResolveConstructCallExpr(const SyntaxOperands& syntax, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode typeNode = syntax.Field(gs.fieldType);
    if (ts_node_is_null(typeNode))
    {
        return "";
    }
    std::string written(parser::NodeText(typeNode, sourceCode));
    const TSNode templates = syntax.FirstNamed(gs.symTemplateTypeList);
    if (!ts_node_is_null(templates))
    {
        written += std::string(parser::NodeText(templates, sourceCode));
    }
    return CleanExpressionType(written);
}

/**
 * @brief Resolves casts and constructor calls from their written type operands.
 */
static std::optional<std::string> ResolveCastOrConstructExpr(TSSymbol sym, TSNode exprNode,
                                                             const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symCastExpression || sym == gs.symFunctionalCastExpression)
    {
        const TSNode typeNode = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldType);
        return ts_node_is_null(typeNode) ? ""
                                         : CleanExpressionType(std::string(parser::NodeText(typeNode, ctx.sourceCode)));
    }
    if (sym == gs.symConstructCallExpression)
    {
        return ResolveConstructCallExpr(SyntaxOperands(exprNode, ctx.nodeIndex), ctx.sourceCode);
    }
    return std::nullopt;
}

/**
 * @brief Resolves initializer list or lambda expression types.
 * @param[in] sym Tree-sitter AST grammar symbol.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved expression type string if handled, std::nullopt otherwise.
 */
static std::optional<std::string> ResolveLambdaOrInitExpr(TSSymbol sym, TSNode exprNode,
                                                          const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symInitializerList)
    {
        return std::string(constants::k_initializerListTypeSentinel);
    }
    if (sym == gs.symLambdaExpression)
    {
        if (auto target = FuncdefTargetOfLambda(ReadLambdaTarget(exprNode, ctx.sourceCode), ctx.symbolTable))
        {
            return target->qualifiedName.empty() ? target->name : target->qualifiedName;
        }
        return "";
    }
    return std::nullopt;
}

/**
 * @brief Resolves indexed property accessor type from member expression receiver.
 * @param[in] objNode Member expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
static std::string ResolveMemberIndexedProperty(TSNode objNode, const ExpressionTypeContext& ctx,
                                                std::string_view indexArgType = "")
{
    const SyntaxOperands syntax(objNode, ctx.nodeIndex);
    const TSNode recNode = syntax.Field(parser::GetGrammarSymbols().fieldObject);
    const TSNode memNode = syntax.Field(parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(recNode) || ts_node_is_null(memNode))
    {
        return "";
    }
    std::string recType = CleanBaseType(ResolveExpressionType(recNode, ctx));
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);
    return ResolveMemberIndexedPropertyType(recType, memName, ctx.symbolTable, indexArgType);
}

/**
 * @brief Resolves indexed property accessor type from scoped or bare identifier.
 * @param[in] objNode Scoped identifier or identifier AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
static std::string ResolveScopedOrBareIndexedProperty(TSNode objNode, const ExpressionTypeContext& ctx,
                                                      std::string_view indexArgType = "")
{
    const auto& gs = parser::GetGrammarSymbols();
    TSSymbol objSym = ts_node_symbol(objNode);
    if (objSym == gs.symScopedIdentifier)
    {
        std::string_view text = parser::NodeText(objNode, ctx.sourceCode);
        const auto [scopeName, name] = utils::SplitScopedName(text);
        return ResolveScopedIndexedPropertyType(scopeName, name, ctx.symbolTable, indexArgType);
    }

    std::string_view name = parser::NodeText(objNode, ctx.sourceCode);
    std::string enclosing;
    if (!ctx.uri.empty())
    {
        enclosing = ctx.symbolTable.FindEnclosingClassName(ctx.uri, ts_node_start_point(objNode).row);
    }
    return ResolveBareIndexedPropertyType(name, enclosing, ctx.symbolTable, indexArgType);
}

/**
 * @brief Resolves index expression types for containers and indexed property accessors.
 * @param[in] exprNode Index expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved indexed expression type, or empty string.
 */
static std::string ResolveIndexExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(exprNode, ctx.nodeIndex);
    const TSNode objNode = syntax.Field(gs.fieldObject);
    if (ts_node_is_null(objNode))
    {
        return "";
    }

    const TSNode indexNode = syntax.Field(gs.fieldIndex);
    std::string indexArgType;
    if (!ts_node_is_null(indexNode))
    {
        indexArgType = ResolveExpressionType(indexNode, ctx);
    }

    if (ts_node_symbol(objNode) == gs.symMemberAccess)
    {
        if (std::string propType = ResolveMemberIndexedProperty(objNode, ctx, indexArgType); !propType.empty())
        {
            return propType;
        }
    }
    else
    {
        if (std::string propType = ResolveScopedOrBareIndexedProperty(objNode, ctx, indexArgType); !propType.empty())
        {
            return propType;
        }
    }

    std::string objType = ResolveExpressionType(objNode, ctx);
    if (objType.empty())
    {
        return "";
    }
    return ResolveIndexedTypeWithArg(objType, ctx.symbolTable, ctx.arrayTypeName, indexArgType);
}

/**
 * @brief Resolves miscellaneous and secondary expression types.
 */
static std::string ResolveOtherExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (auto castOrConstruct = ResolveCastOrConstructExpr(sym, exprNode, ctx))
    {
        return *castOrConstruct;
    }
    if (auto lambdaOrInit = ResolveLambdaOrInitExpr(sym, exprNode, ctx))
    {
        return *lambdaOrInit;
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symAssignmentExpression)
    {
        TSNode left = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldLeft);
        return ts_node_is_null(left) ? "" : ResolveExpressionType(left, ctx);
    }
    if (sym == gs.symIndexExpression)
    {
        return ResolveIndexExpr(exprNode, ctx);
    }
    if (sym == gs.symUnaryExpression)
    {
        return ResolveUnaryExpr(exprNode, ctx);
    }
    if (sym == gs.symPostfixExpression)
    {
        TSNode operandNode = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldOperand);
        return ts_node_is_null(operandNode) ? "" : ResolveExpressionType(operandNode, ctx);
    }
    return "";
}

/**
 * @brief Resolves primary/atomic expression types (parentheses, this, literals, identifiers).
 * @param[in] sym Tree-sitter AST grammar symbol.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Optional containing resolved type if sym is primary, or std::nullopt.
 */
static std::optional<std::string> ResolvePrimaryExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symParenthesizedExpression)
    {
        return ResolveParenthesizedExpr(exprNode, ctx);
    }
    if (sym == gs.symTypedInitializerList)
    {
        TSNode typeNode = SyntaxOperands(exprNode, ctx.nodeIndex).Field(gs.fieldType);
        if (!ts_node_is_null(typeNode))
        {
            return std::string(parser::NodeText(typeNode, ctx.sourceCode));
        }
    }
    if (sym == gs.symThisExpression)
    {
        return ResolveThisExpr(exprNode, ctx);
    }
    if (auto lit = ResolveLiteralExpr(sym, exprNode, ctx.sourceCode, ctx.stringTypeName); !lit.empty())
    {
        return lit;
    }
    if (sym == gs.symScopedIdentifier)
    {
        return ResolveScopedIdentifierExpr(exprNode, ctx);
    }
    if (sym == gs.symIdentifier || sym == gs.tokFunction || parser::keywords::IsKeyword(parser::GetNodeType(exprNode)))
    {
        return ResolveIdentifierExpr(exprNode, ctx);
    }
    return std::nullopt;
}

/**
 * @brief Resolves composite expression types (binary, ternary, member, call).
 * @param[in] sym Tree-sitter AST grammar symbol.
 * @param[in] exprNode Expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @return Resolved type string, or empty string.
 */
static std::string ResolveCompositeExpr(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symBinaryExpression)
    {
        return ResolveBinaryExpr(exprNode, ctx);
    }
    if (sym == gs.symTernaryExpression)
    {
        return ResolveTernaryExpr(exprNode, ctx);
    }
    if (sym == gs.symMemberExpression)
    {
        return ResolveMemberExpr(exprNode, ctx);
    }
    if (sym == gs.symCallExpression)
    {
        return ResolveCallExpr(exprNode, ctx);
    }
    return ResolveOtherExpr(sym, exprNode, ctx);
}

static std::string ResolveExpressionTypeUncached(TSSymbol sym, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (auto primary = ResolvePrimaryExpr(sym, exprNode, ctx))
    {
        return *primary;
    }
    return ResolveCompositeExpr(sym, exprNode, ctx);
}

static std::string_view GetTrimmedNodeTextView(TSNode node, std::string_view sourceCode) noexcept
{
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return {};
    }
    std::string_view sv = sourceCode.substr(start, end - start);
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.front())))
    {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && isspace(static_cast<unsigned char>(sv.back())))
    {
        sv.remove_suffix(1);
    }
    return sv;
}

static std::string ResolveExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    exprNode = UnwrapIndexedParentheses(exprNode, ctx.nodeIndex);
    if (ts_node_is_null(exprNode) || ctx.sourceCode.empty())
    {
        return "";
    }
    if (ts_node_is_error(exprNode) || ts_node_is_missing(exprNode) ||
        (ctx.nodeIndex && ctx.nodeIndex->NodeHasSyntaxError(exprNode)))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }

    const TSSymbol sym = ts_node_symbol(exprNode);
    if (ctx.cache)
    {
        const ExpressionCacheKey key{ts_node_start_byte(exprNode), ts_node_end_byte(exprNode), sym};
        auto it = ctx.cache->types.find(key);
        if (it != ctx.cache->types.end())
        {
            return it->second;
        }

        std::string_view rawText = GetTrimmedNodeTextView(exprNode, ctx.sourceCode);
        if (rawText == "void")
        {
            return "void";
        }

        std::string resolved = ResolveExpressionTypeUncached(sym, exprNode, ctx);
        ctx.cache->types.emplace(key, resolved);
        return resolved;
    }

    std::string_view rawText = GetTrimmedNodeTextView(exprNode, ctx.sourceCode);
    if (rawText == "void")
    {
        return "void";
    }

    return ResolveExpressionTypeUncached(sym, exprNode, ctx);
}

std::string ResolveExpressionTypeAt(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx)
{
    const TSNode node = RecoverNodeFromContext(ctx, expressionRange.startByte, expressionRange.endByte);
    if (ts_node_is_null(node))
    {
        return "";
    }
    return ResolveExpressionType(node, ctx);
}

} // namespace angel_lsp::analysis
