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
#include "analysis/resolution/ExpressionTypeLiterals.h"
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

static uint32_t FindLambdaParamIndex(TSNode paramsNode, std::string_view paramName, std::string_view sourceCode)
{
    auto astParams = ExtractLambdaParameters(paramsNode, sourceCode);
    for (const auto& p : astParams)
    {
        if (p.name == paramName)
        {
            return p.index;
        }
    }
    return UINT32_MAX;
}

static std::string InferLambdaParamType(TSNode nodeInLambda, std::string_view paramName, const SymbolTable& symbolTable,
                                        std::string_view sourceCode)
{
    if (ts_node_is_null(nodeInLambda) || paramName.empty())
    {
        return "";
    }

    TSNode lambdaNode = parser::FindAncestorWithSymbol(nodeInLambda, parser::GetGrammarSymbols().symLambdaExpression);
    if (ts_node_is_null(lambdaNode))
    {
        return "";
    }

    TSNode paramsNode = parser::GetChildByField(lambdaNode, parser::GetGrammarSymbols().fieldParameters);
    if (ts_node_is_null(paramsNode))
    {
        return "";
    }

    uint32_t paramIdx = FindLambdaParamIndex(paramsNode, paramName, sourceCode);
    if (paramIdx == UINT32_MAX)
    {
        return "";
    }

    auto targetFuncdef = FuncdefTargetOfLambda(ReadLambdaTarget(lambdaNode, sourceCode), symbolTable);
    if (!targetFuncdef)
    {
        return "";
    }

    if (std::holds_alternative<FuncdefSignature>(targetFuncdef->signature))
    {
        const auto& params = targetFuncdef->GetFuncdef().parameters;
        return paramIdx < params.size() ? MemberOwnerType(params[paramIdx].typeName, symbolTable.GetDefaultArrayType())
                                        : "";
    }
    if (std::holds_alternative<FunctionSignature>(targetFuncdef->signature))
    {
        const auto& params = targetFuncdef->GetFunction().parameters;
        return paramIdx < params.size() ? MemberOwnerType(params[paramIdx].typeName, symbolTable.GetDefaultArrayType())
                                        : "";
    }
    return "";
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
 * @brief Resolves 'this' expression type based on enclosing container hierarchy.
 */
static std::string ResolveThisIdentifier(TSNode exprNode, const ExpressionTypeContext& ctx,
                                         const std::string& hostClass)
{
    if (!hostClass.empty())
    {
        return CleanExpressionType(hostClass);
    }
    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            return CleanExpressionType(c.qualifiedName.empty() ? c.name : c.qualifiedName);
        }
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
    return ResolveThisIdentifier(exprNode, ctx, hostClass);
}

/**
 * @brief Resolves scoped identifier expression type.
 */
static std::string ResolveScopedEnumMember(std::string_view whole, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto [qualView, mem] = utils::SplitScopedName(whole);
    if (qualView.empty())
    {
        return "";
    }
    std::string qual(qualView);
    auto qSyms = ctx.symbolTable.FindSymbols(qual);
    if (qSyms.empty())
    {
        qSyms = ResolveScopeSymbols(qual, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    }
    for (const auto& qSym : qSyms)
    {
        if (qSym.type == SymbolType::Enum && std::holds_alternative<EnumSignature>(qSym.signature))
        {
            for (const auto& m : qSym.GetEnum().members)
            {
                if (m.name == mem)
                {
                    return qSym.qualifiedName.empty() ? qSym.name : qSym.qualifiedName;
                }
            }
        }
    }
    return "";
}

/**
 * @brief Resolves the type name of the first variable or property symbol found in a span of candidates.
 * @param[in] syms Span of matching candidate symbols to inspect.
 * @param[in] table Symbol table containing container type information.
 * @return Type name string of the resolved variable or property, or empty string if none match.
 */
static std::string ExtractVariableOrPropertyType(std::span<const Symbol> syms, const SymbolTable& table)
{
    for (const auto& sym : syms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            !sym.GetVariable().typeName.empty())
        {
            std::string type = sym.GetVariable().typeName;
            if (!sym.containerName.empty())
            {
                type = QualifyTypeInContainer(type, sym.containerName, table);
            }
            return CleanExpressionType(type);
        }
    }
    return {};
}

static std::string ResolveGlobalScopedExpr(TSNode exprNode, std::string_view rootPath, const ExpressionTypeContext& ctx)
{
    auto syms = ctx.symbolTable.FindSymbols(rootPath);
    if (std::string type = ExtractVariableOrPropertyType(syms, ctx.symbolTable); !type.empty())
    {
        return type;
    }
    return ResolveScopedEnumMember(std::string(rootPath), exprNode, ctx);
}

static std::string ResolveScopedIdentifierExpr(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    const auto text = parser::NodeText(exprNode, ctx.sourceCode);
    if (utils::HasLeadingGlobalPrefix(text))
    {
        return ResolveGlobalScopedExpr(exprNode, utils::StripGlobalPrefix(text), ctx);
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode lastIdentifier{};
    bool qualified = false;
    SyntaxOperands(exprNode, ctx.nodeIndex)
        .ForEachNamed(
            [&](TSNode child)
            {
                if (ts_node_symbol(child) == gs.symIdentifier)
                {
                    qualified = !ts_node_is_null(lastIdentifier);
                    lastIdentifier = child;
                }
                return true;
            });

    if (qualified)
    {
        const std::string whole(parser::NodeText(exprNode, ctx.sourceCode));
        auto wholeSyms = ctx.symbolTable.FindSymbols(whole);
        if (wholeSyms.empty())
        {
            wholeSyms = ResolveScopeSymbols(whole, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
        }
        if (std::string type = ExtractVariableOrPropertyType(wholeSyms, ctx.symbolTable); !type.empty())
        {
            return type;
        }
        return ResolveScopedEnumMember(whole, exprNode, ctx);
    }

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
    for (const auto& c : containers)
    {
        if (c.kind != ContainerKind::Class && c.kind != ContainerKind::Interface)
        {
            continue;
        }
        const std::string& typeName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
        auto hierarchy = GetInheritedTypeHierarchy(typeName, ctx.symbolTable, ctx.cache);
        ExpandMixinHostHierarchies(typeName, ctx.symbolTable, hierarchy, ctx.cache);
        for (const auto& cls : hierarchy)
        {
            auto found = FilterSymbolsByContainerKind(ctx.symbolTable.FindMemberSymbols(cls, name), c.kind);
            if (!found.empty())
            {
                return found;
            }
            auto accessors = FindPropertyAccessors(cls, name, ctx.symbolTable, false);
            if (!accessors.empty())
            {
                return accessors;
            }
        }
        break;
    }
    return {};
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
    if (hostClass.empty())
    {
        return "";
    }
    auto hierarchy = GetInheritedTypeHierarchy(hostClass, ctx.symbolTable, ctx.cache);
    for (const auto& cls : hierarchy)
    {
        auto found = ctx.symbolTable.FindMemberSymbols(cls, name);
        if (!found.empty())
        {
            for (const auto& sym : found)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                    !sym.GetVariable().typeName.empty())
                {
                    std::string type = sym.GetVariable().typeName;
                    if (!sym.containerName.empty())
                    {
                        type = QualifyTypeInContainer(type, sym.containerName, ctx.symbolTable);
                    }
                    return CleanExpressionType(type);
                }
            }
            break;
        }
    }
    for (const auto& cls : hierarchy)
    {
        auto accessors = FindPropertyAccessors(cls, name, ctx.symbolTable, false);
        if (!accessors.empty())
        {
            std::string pType = PropertyTypeFromAccessors(accessors);
            if (!pType.empty())
            {
                return CleanExpressionType(pType);
            }
        }
    }
    return "";
}

/**
 * @brief Fallback lookup for global accessor functions (get_X, set_X).
 */
static std::string ResolveIdentifierAccessorFallback(const std::string& name, const SymbolTable& symbolTable)
{
    for (const auto& prefix : {"get_", "set_"})
    {
        auto accessorSyms = symbolTable.FindSymbols(std::string(prefix) + name);
        for (const auto& sym : accessorSyms)
        {
            if (sym.type == SymbolType::Function && sym.containerName.empty() &&
                std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                const bool isIndexed = (sym.name.starts_with("get_") && !fn.parameters.empty()) ||
                                       (sym.name.starts_with("set_") && fn.parameters.size() > 1);
                if (isIndexed)
                {
                    continue;
                }
                if (!fn.returnType.empty() && fn.returnType != "void")
                {
                    return CleanExpressionType(fn.returnType);
                }
                if (!fn.parameters.empty())
                {
                    return CleanExpressionType(fn.parameters.front().typeName);
                }
            }
        }
    }
    return "";
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
    for (const auto& sym : syms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            !sym.GetVariable().typeName.empty())
        {
            std::string type = sym.GetVariable().typeName;
            if (!sym.containerName.empty())
            {
                type = QualifyTypeInContainer(type, sym.containerName, ctx.symbolTable);
            }
            return CleanExpressionType(type);
        }
    }
    if (std::string pType = PropertyTypeFromAccessors(syms); !pType.empty())
    {
        return CleanExpressionType(pType);
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
 * @brief Resolves ternary branches using inheritance hierarchies.
 */
static std::string ResolveTernaryHierarchy(const TernaryBranchTypes& types, const SymbolTable& symbolTable)
{
    auto h1 = GetInheritedTypeHierarchy(types.c1, symbolTable);
    for (const auto& b : h1)
    {
        if (CanonicalizeType(CleanBaseType(b)) == types.c2)
        {
            return types.t2;
        }
    }
    auto h2 = GetInheritedTypeHierarchy(types.c2, symbolTable);
    for (const auto& b : h2)
    {
        if (CanonicalizeType(CleanBaseType(b)) == types.c1)
        {
            return types.t1;
        }
    }
    if (HasConversionMethod(types.c1, types.c2, symbolTable))
    {
        return types.t2;
    }
    if (HasConversionMethod(types.c2, types.c1, symbolTable))
    {
        return types.t1;
    }
    return "";
}

/**
 * @brief Determines the common promoted numeric type between two numeric operand types in a ternary expression.
 * @param[in] c1 Cleaned type name of the true-branch expression.
 * @param[in] c2 Cleaned type name of the false-branch expression.
 * @return Promoted common numeric type name (e.g. \"double\", \"float\", \"int\").
 */
static std::string ResolveTernaryNumericPromotion(const std::string& c1, const std::string& c2)
{
    if (c1 == "double" || c2 == "double")
        return "double";
    if (c1 == "float" || c2 == "float")
        return "float";
    if (c1 == "int64" || c2 == "int64")
        return "int64";
    if (c1 == "uint64" || c2 == "uint64")
        return "uint64";
    if (c1 == "uint" || c2 == "uint")
        return "uint";
    return "int";
}

static std::string ResolveTernaryEnumPromotion(const std::string& c1, const std::string& c2,
                                               const SymbolTable& symbolTable)
{
    if (ResolvesToEnum(c1, symbolTable) && IsIntegerPrimitive(c2))
        return c2;
    if (ResolvesToEnum(c2, symbolTable) && IsIntegerPrimitive(c1))
        return c1;
    return "";
}

/**
 * @brief Resolves ternary expression type when one or both branches are null.
 * @param[in] t1 Consequence type.
 * @param[in] t2 Alternative type.
 * @param[in] c1 Cleaned consequence base type.
 * @param[in] c2 Cleaned alternative base type.
 * @return Resolved handle type or "null", or empty string if not applicable.
 */
static std::string ResolveTernaryNullBranch(const std::string& t1, const std::string& t2, const std::string& c1,
                                            const std::string& c2)
{
    if (c1 == "null" && IsHandleTypeView(t2))
    {
        return t2;
    }
    if (c2 == "null" && IsHandleTypeView(t1))
    {
        return t1;
    }
    return (c1 == "null" && c2 == "null") ? "null" : "";
}

/**
 * @brief Resolves result type between ternary consequence and alternative branch types.
 */
static std::string ResolveTernaryBranchTypes(std::string_view t1, std::string_view t2, const SymbolTable& symbolTable)
{
    std::string clean1 = CleanExpressionType(t1);
    std::string clean2 = CleanExpressionType(t2);
    if (!clean1.empty() && clean1 == clean2)
    {
        return clean1;
    }

    std::string c1 = CanonicalizeType(CleanBaseType(t1));
    std::string c2 = CanonicalizeType(CleanBaseType(t2));
    if (!c1.empty() && c1 == c2)
    {
        return (IsHandleTypeView(t1) && IsHandleTypeView(t2)) ? c1 + "@" : c1;
    }
    if (auto nullRes = ResolveTernaryNullBranch(clean1, clean2, c1, c2); !nullRes.empty())
    {
        return nullRes;
    }
    if (IsNumericPrimitive(c1) && IsNumericPrimitive(c2))
    {
        return ResolveTernaryNumericPromotion(c1, c2);
    }
    if (auto enumRes = ResolveTernaryEnumPromotion(c1, c2, symbolTable); !enumRes.empty())
    {
        return enumRes;
    }
    return ResolveTernaryHierarchy(TernaryBranchTypes{std::string(t1), std::string(t2), c1, c2}, symbolTable);
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
 * @brief Builds the property search order for member expressions, prioritizing non-mixins.
 */
static std::vector<std::string> OrderMemberPropertySearchHierarchy(const std::string& cleanObj,
                                                                   const SymbolTable& symbolTable)
{
    auto hierarchy = GetInheritedTypeHierarchy(cleanObj, symbolTable);
    if (hierarchy.empty() && !cleanObj.empty())
    {
        hierarchy.push_back(cleanObj);
    }
    std::vector<std::string> propSearchOrder;
    if (!hierarchy.empty())
    {
        propSearchOrder.push_back(hierarchy[0]);
        for (size_t i = 1; i < hierarchy.size(); ++i)
        {
            if (!IsMixinClass(hierarchy[i], symbolTable))
            {
                propSearchOrder.push_back(hierarchy[i]);
            }
        }
        for (size_t i = 1; i < hierarchy.size(); ++i)
        {
            if (IsMixinClass(hierarchy[i], symbolTable))
            {
                propSearchOrder.push_back(hierarchy[i]);
            }
        }
    }
    return propSearchOrder;
}

/**
 * @brief Checks if a symbol is an instance member rather than an enum constant.
 */
static bool IsNonEnumSymbol(const Symbol& s)
{
    if (s.type == SymbolType::Variable && std::holds_alternative<VariableSignature>(s.signature))
    {
        return !s.GetVariable().isEnumConstant;
    }
    return true;
}

/**
 * @brief Finds candidate member symbols in a specific type.
 */
static std::vector<Symbol> FindMemberSymbolsInType(const std::string& typeName, const std::string& memName,
                                                   const SymbolTable& symbolTable)
{
    auto found = symbolTable.FindMemberSymbols(typeName, memName);
    const bool hasInstance = std::any_of(found.begin(), found.end(), IsNonEnumSymbol);
    if (!hasInstance)
    {
        auto getters = symbolTable.FindMemberSymbols(typeName, "get_" + memName);
        auto setters = symbolTable.FindMemberSymbols(typeName, "set_" + memName);
        if (!getters.empty() || !setters.empty())
        {
            found.insert(found.begin(), setters.begin(), setters.end());
            found.insert(found.begin(), getters.begin(), getters.end());
        }
    }
    if (found.empty())
    {
        auto allSymbols = symbolTable.FindSymbols(memName);
        for (const auto& sSym : allSymbols)
        {
            if (sSym.containerName == typeName)
            {
                found.push_back(sSym);
            }
        }
    }
    std::stable_partition(found.begin(), found.end(), IsNonEnumSymbol);
    return found;
}

/**
 * @brief Extracts the resolved type of an accessor function member.
 * @param[in] sym Accessor function symbol to inspect.
 * @return Cleaned property type, or empty string if indexed or invalid.
 */
static std::string ExtractAccessorMemberType(const Symbol& sym)
{
    if (!std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return "";
    }
    const auto& fn = sym.GetFunction();
    const bool isGet = sym.name.starts_with("get_");
    const bool isSet = sym.name.starts_with("set_");
    if (!isGet && !isSet && !fn.modifiers.isProperty)
    {
        return "";
    }
    const bool isIndexed = (isGet && !fn.parameters.empty()) || (isSet && fn.parameters.size() > 1);
    if (isIndexed)
    {
        return "";
    }
    if (!fn.returnType.empty() && fn.returnType != "void")
    {
        return CleanExpressionType(fn.returnType);
    }
    if (!fn.parameters.empty())
    {
        return CleanExpressionType(fn.parameters.back().typeName);
    }
    return "";
}

/**
 * @brief Extracts the resolved type of a member symbol.
 * @param[in] sym Member symbol to inspect.
 * @return Cleaned member type, or empty string if unresolved.
 */
static std::string ExtractMemberSymbolType(const Symbol& sym, const SymbolTable& table)
{
    if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) && !sym.GetVariable().typeName.empty())
    {
        std::string type = sym.GetVariable().typeName;
        if (!sym.containerName.empty())
        {
            type = QualifyTypeInContainer(type, sym.containerName, table);
        }
        return CleanExpressionType(type);
    }
    if (sym.type == SymbolType::Function)
    {
        std::string type = ExtractAccessorMemberType(sym);
        if (!sym.containerName.empty())
        {
            type = QualifyTypeInContainer(type, sym.containerName, table);
        }
        return type;
    }
    return "";
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
 * @brief Resolves known container method return types (length, size, isEmpty).
 * @param[in] memName Member name invoked.
 * @param[in] templateInfo Parsed template info of the receiver.
 * @param[in] objType Raw object type name.
 * @return Fallback method return type, or empty string.
 */
static std::string ResolveContainerMethodFallback(std::string_view memName, const TemplateTypeInfo& templateInfo,
                                                  std::string_view objType, std::string_view arrayTypeName)
{
    const std::string_view effectiveArrayName = arrayTypeName;
    if ((templateInfo.containerName == effectiveArrayName && !templateInfo.templateArgs.empty()) ||
        objType.ends_with("[]"))
    {
        if (memName == "length" || memName == "size")
        {
            return "uint";
        }
        if (memName == "isEmpty")
        {
            return "bool";
        }
    }
    return "";
}

/**
 * @brief Resolves return type from candidate method overloads, substituting template arguments.
 * @param[in] candidates Candidate method symbols.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] objType Object receiver type.
 * @param[in] symbolTable Symbol table for template binding.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveCandidateMethodReturn(const std::vector<Symbol>& candidates,
                                                const std::vector<std::string>& argTypes, const std::string& objType,
                                                const SymbolTable& symbolTable)
{
    if (candidates.empty())
    {
        return "";
    }
    auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
    const Symbol* chosen = match.bestCandidate ? match.bestCandidate : &candidates[0];
    if (chosen && std::holds_alternative<FunctionSignature>(chosen->signature))
    {
        std::string ret = chosen->GetFunction().returnType;
        auto binding = BindTemplateArguments(objType, symbolTable);
        if (binding.usable)
        {
            ret = SubstituteTemplateParameters(ret, binding);
        }
        else
        {
            auto templateInfo = ParseTemplateType(objType);
            if (!templateInfo.templateArgs.empty())
            {
                ret = SubstituteTypeParam(ret, "T", templateInfo.templateArgs[0]);
            }
        }
        return CleanExpressionType(ret);
    }
    return CleanExpressionType(candidates[0].GetFunction().returnType);
}
/**
 * @brief Appends candidate function if not already overridden by signature.
 * @param[in,out] candidates Accumulating list of candidate symbols.
 * @param[in] sym Candidate symbol to check and potentially append.
 */
static void AppendCandidateIfUnique(std::vector<Symbol>& candidates, const Symbol& sym)
{
    if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
    {
        bool overridden = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const Symbol& kept) { return HasSameParameterList(kept, sym); });
        if (!overridden)
        {
            candidates.push_back(sym);
        }
    }
}

/**
 * @brief Collects candidate member methods along the type inheritance hierarchy.
 * @param[in] ownerType Owning type name.
 * @param[in] memName Member function name.
 * @param[in] symbolTable Symbol table to query.
 * @return List of matching function candidate symbols.
 */
static std::vector<Symbol> CollectMemberMethodCandidates(const std::string& ownerType, const std::string& memName,
                                                         const SymbolTable& symbolTable)
{
    std::vector<Symbol> candidates;
    auto hierarchy = GetInheritedTypeHierarchy(ownerType, symbolTable);
    if (hierarchy.empty() && !ownerType.empty())
    {
        hierarchy.emplace_back(ownerType);
    }
    for (const auto& typeName : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbols(typeName, memName);
        for (const auto& sym : found)
        {
            AppendCandidateIfUnique(candidates, sym);
        }
    }
    return candidates;
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

    std::string objType = CanonicalizeArrayType(ResolveExpressionType(objNode, ctx), ctx.arrayTypeName);
    std::string memName = GetTrimmedNodeText(memNode, ctx.sourceCode);

    std::string ownerType = MemberOwnerType(objType, ctx.arrayTypeName);
    if (ownerType.empty())
    {
        ownerType = CleanExpressionType(objType);
    }
    if (ownerType.empty())
    {
        ownerType = GetTrimmedNodeText(objNode, ctx.sourceCode);
    }

    auto candidates = CollectMemberMethodCandidates(ownerType, memName, ctx.symbolTable);
    if (!candidates.empty())
    {
        return ResolveCandidateMethodReturn(candidates, argTypes, objType, ctx.symbolTable);
    }

    auto templateInfo = ParseTemplateType(objType);
    return ResolveContainerMethodFallback(memName, templateInfo, objType, ctx.arrayTypeName);
}

/**
 * @brief Appends call candidates from mixins included in a class.
 * @param[in] containerName Enclosing container name.
 * @param[in] funcName Function name being called.
 * @param[in] symbolTable Symbol table to query.
 * @param[in,out] candidates Accumulating candidate symbols.
 */
static void CollectMixinCallCandidates(std::string_view containerName, const std::string& funcName,
                                       const SymbolTable& symbolTable, std::vector<Symbol>& candidates)
{
    auto classSyms = symbolTable.FindSymbols(containerName);
    for (const auto& cs : classSyms)
    {
        if (cs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(cs.signature))
        {
            for (const auto& mixin : cs.GetClass().includedMixins)
            {
                auto mixinFound = symbolTable.FindMemberSymbols(mixin, funcName);
                for (const auto& sym : mixinFound)
                {
                    AppendCandidateIfUnique(candidates, sym);
                }
            }
        }
    }
}

/**
 * @brief Collects candidate function symbols from enclosing class/interface hierarchy.
 * @param[in] exprNode Expression AST node inside enclosing container.
 * @param[in] funcName Function name being called.
 * @param[in] ctx Expression type resolution context.
 * @return Vector of candidate function symbols.
 */
static std::vector<Symbol> CollectEnclosingCallCandidates(TSNode exprNode, const std::string& funcName,
                                                          const ExpressionTypeContext& ctx)
{
    std::vector<Symbol> candidates;
    auto containers = ResolveEnclosingContainers(exprNode, ctx.sourceCode, ctx.nodeIndex);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            std::string containerName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
            auto hierarchy = GetInheritedTypeHierarchy(containerName, ctx.symbolTable, ctx.cache);
            for (const auto& cls : hierarchy)
            {
                auto found = ctx.symbolTable.FindMemberSymbols(cls, funcName);
                for (const auto& sym : found)
                {
                    AppendCandidateIfUnique(candidates, sym);
                }
            }
            CollectMixinCallCandidates(containerName, funcName, ctx.symbolTable, candidates);
            break;
        }
    }
    return candidates;
}

/**
 * @brief Resolves return type from call candidates given argument types.
 * @param[in] candidates Function overload candidates.
 * @param[in] argTypes Evaluated argument types.
 * @param[in] symbolTable Symbol table to query.
 * @return Resolved return type, or empty string.
 */
static std::string ResolveCallOverloadReturn(const std::vector<Symbol>& candidates,
                                             const std::vector<std::string>& argTypes, const SymbolTable& symbolTable)
{
    if (candidates.empty())
    {
        return "";
    }
    auto match = ResolveBestOverload(candidates, argTypes, symbolTable);
    const Symbol* chosen = match.bestCandidate ? match.bestCandidate : &candidates[0];
    if (chosen && std::holds_alternative<FunctionSignature>(chosen->signature))
    {
        std::string ret = CleanExpressionType(chosen->GetFunction().returnType);
        if (ret.empty() &&
            (chosen->name == chosen->containerName || LastScopeSegment(chosen->containerName) == chosen->name))
        {
            return chosen->containerName;
        }
        return ret;
    }
    std::string ret = CleanExpressionType(candidates[0].GetFunction().returnType);
    if (ret.empty() && (candidates[0].name == candidates[0].containerName ||
                        LastScopeSegment(candidates[0].containerName) == candidates[0].name))
    {
        return candidates[0].containerName;
    }
    return ret;
}

/**
 * @brief Collects function symbols from scope and global table matching funcName.
 * @param[in] funcName Target function name.
 * @param[in] exprNode Call expression AST node.
 * @param[in] ctx Expression type resolution context.
 * @param[in,out] candidates Accumulating candidate symbols.
 */
static void CollectScopeAndGlobalFunctions(const std::string& funcName, TSNode exprNode,
                                           const ExpressionTypeContext& ctx, std::vector<Symbol>& candidates)
{
    std::vector<Symbol> inScope =
        ResolveScopeSymbols(funcName, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    for (const auto& sym : inScope)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            candidates.push_back(sym);
        }
    }
    if (!candidates.empty())
    {
        return;
    }
    auto globalFound = ctx.symbolTable.FindSymbols(funcName);
    for (const auto& sym : globalFound)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            candidates.push_back(sym);
        }
    }
}

/**
 * @brief Checks if funcName refers to a class or interface type (for constructor-like bare calls).
 * @param[in] funcName Candidate type name.
 * @param[in] symbolTable Symbol table to query.
 * @return Matched type name, or empty string.
 */
static std::string FindClassOrInterfaceName(const std::string& funcName, const SymbolTable& symbolTable)
{
    auto globalFound = symbolTable.FindSymbols(funcName);
    for (const auto& sym : globalFound)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
        {
            if (!sym.qualifiedName.empty())
            {
                return sym.qualifiedName;
            }
            if (!sym.containerName.empty())
            {
                return utils::JoinScopedName(sym.containerName, sym.name);
            }
            return sym.name;
        }
    }
    return "";
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
    std::vector<Symbol> candidates = CollectEnclosingCallCandidates(exprNode, funcName, ctx);
    if (candidates.empty())
    {
        CollectScopeAndGlobalFunctions(funcName, exprNode, ctx, candidates);
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
    std::string cleanCallee = CleanBaseType(calleeType);
    if (!cleanCallee.empty())
    {
        if (auto funcdef = FindFuncdefSymbol(cleanCallee, ctx.symbolTable))
        {
            return CleanExpressionType(funcdef->GetFuncdef().returnType);
        }
    }
    return calleeType;
}

/**
 * @brief Checks if an expression is a function handle target for unary '@'.
 */
static bool IsFunctionHandleTarget(TSNode target, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (ts_node_is_null(target))
    {
        return false;
    }
    std::string targetName = GetTrimmedNodeText(target, ctx.sourceCode);
    if (targetName.empty() || targetName.find('(') != std::string::npos)
    {
        return false;
    }
    if (const auto bucket = ctx.symbolTable.FindSymbolsPtr(targetName))
    {
        for (const auto& candidate : *bucket)
        {
            if (candidate.type == SymbolType::Function)
                return true;
        }
    }
    auto syms = ResolveScopeSymbols(targetName, exprNode, ctx.sourceCode, ctx.symbolTable, ctx.nodeIndex);
    if (syms.empty())
    {
        syms = ctx.symbolTable.FindSymbols(targetName);
    }
    for (const auto& candidate : syms)
    {
        if (candidate.type == SymbolType::Function)
            return true;
    }
    return false;
}

/**
 * @brief Resolves the resulting type of a unary handle take expression ('@expr').
 * @param[in] operandNode AST node representing the inner operand expression.
 * @param[in] exprNode AST node representing the unary '@' expression.
 * @param[in] ctx Diagnostic collection and expression evaluation context.
 * @return Resolved handle type name ending with '@', or empty string if unresolved.
 */
static std::string ResolveUnaryHandle(TSNode operandNode, TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (IsFunctionHandleTarget(operandNode, exprNode, ctx))
    {
        return {};
    }
    std::string operandType = ResolveExpressionType(operandNode, ctx);
    if (ExpressionTypeCache::IsPoisonType(operandType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (operandType.empty())
    {
        return {};
    }
    std::string clean = CleanExpressionType(operandType);
    return IsHandleTypeView(clean) ? clean : clean + "@";
}

static std::string ResolveUnaryMinus(const std::string& operandType)
{
    std::string clean = CleanExpressionType(operandType);
    if (clean == "uint" || clean == "uint32")
    {
        return "int";
    }
    if (clean == "uint8")
    {
        return "int8";
    }
    if (clean == "uint16")
    {
        return "int16";
    }
    if (clean == "uint64")
    {
        return "int64";
    }
    return operandType;
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
        return ResolveUnaryHandle(operandNode, exprNode, ctx);
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
 * @brief Searches a collection of accessor symbols for an indexed getter return type.
 * @param[in] accessors Vector of accessor symbols.
 * @param[in] symbolTable Global symbol table pointer for overload resolution.
 * @param[in] indexArgType Deduced type of the index argument.
 * @return Return type of the indexed getter, or empty string.
 */
static std::string FindIndexedGetterType(const std::vector<Symbol>& accessors, const SymbolTable* symbolTable = nullptr,
                                         std::string_view indexArgType = "")
{
    std::vector<Symbol> getters;
    for (const auto& sym : accessors)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto& fn = sym.GetFunction();
            if (sym.name.starts_with("get_") && !fn.parameters.empty())
            {
                getters.push_back(sym);
            }
        }
    }
    if (getters.empty())
    {
        return "";
    }
    if (symbolTable && !indexArgType.empty())
    {
        auto match = ResolveBestOverload(getters, {std::string(indexArgType)}, *symbolTable);
        if (match.bestCandidate)
        {
            return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
        }
    }
    return CleanExpressionType(getters.front().GetFunction().returnType);
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
    if (recType.empty() || memName.empty())
    {
        return "";
    }
    return FindIndexedGetterType(FindPropertyAccessors(recType, memName, ctx.symbolTable, false), &ctx.symbolTable,
                                 indexArgType);
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
        if (!scopeName.empty())
        {
            return FindIndexedGetterType(
                FindPropertyAccessors(std::string(scopeName), std::string(name), ctx.symbolTable, false),
                &ctx.symbolTable, indexArgType);
        }
        return "";
    }

    std::string_view name = parser::NodeText(objNode, ctx.sourceCode);
    if (name.empty())
    {
        return "";
    }
    if (std::string hit = FindIndexedGetterType(FindGlobalPropertyAccessors(std::string(name), ctx.symbolTable, false),
                                                &ctx.symbolTable, indexArgType);
        !hit.empty())
    {
        return hit;
    }
    if (!ctx.uri.empty())
    {
        std::string enclosing = ctx.symbolTable.FindEnclosingClassName(ctx.uri, ts_node_start_point(objNode).row);
        if (!enclosing.empty())
        {
            if (std::string hit =
                    FindIndexedGetterType(FindPropertyAccessors(enclosing, std::string(name), ctx.symbolTable, false),
                                          &ctx.symbolTable, indexArgType);
                !hit.empty())
            {
                return hit;
            }
        }
    }
    return "";
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
