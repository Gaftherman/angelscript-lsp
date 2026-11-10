#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsDeclaredTypeUsable(const std::string& baseName, const DiagnosticContext& ctx)
{
    if (IsBuiltInValueType(baseName, ctx) || ResolvesToEnum(baseName, ctx.request.symbolTable))
    {
        return true;
    }

    const TypeDeclarationInfo declaration = FindTypeDeclaration(baseName, ctx.request.symbolTable);
    if (!declaration.found || !declaration.isClass || declaration.isTemplate)
    {
        return false;
    }
    return !ResolvesToNonClassDeclaration(baseName, ctx.request.symbolTable);
}

DeclaredType ReadDeclaredType(TSNode typeNode, const DiagnosticContext& ctx)
{
    DeclaredType result;
    if (ts_node_is_null(typeNode))
    {
        return result;
    }

    const std::string raw(parser::NodeText(typeNode, ctx.request.sourceCode));
    if (raw.empty())
    {
        return result;
    }

    result.qualifiers.isHandle = IsHandleTypeView(raw);
    result.qualifiers.isConst = IsConstTypeView(raw);
    result.baseName = CleanBaseType(raw);
    if (result.baseName.empty() || ctx.request.IsRegisteredSymbol(result.baseName))
    {
        return result;
    }

    if (raw.find('<') != std::string::npos || raw.find('[') != std::string::npos)
    {
        result.usable = true;
        result.isTemplateOrArray = true;
        return result;
    }

    result.usable = IsDeclaredTypeUsable(result.baseName, ctx);
    return result;
}

static void CheckUnknownInitializerSource(TSNode valueNode, const DeclaredType& declared, const Scope* scope,
                                          DiagnosticContext& ctx)
{
    const std::string identName = std::string(TrimString(parser::NodeText(valueNode, ctx.request.sourceCode)));
    if (identName.empty())
    {
        return;
    }

    bool isTypeOrTemplate = false;
    bool isNamespace = false;
    ForEachSymbolNamed(identName, ctx.request.symbolTable,
                       [&](const Symbol& s) -> bool
                       {
                           if (s.type == SymbolType::Class || s.type == SymbolType::Interface ||
                               s.type == SymbolType::Typedef || s.type == SymbolType::Enum)
                           {
                               isTypeOrTemplate = true;
                               return false;
                           }
                           if (s.type == SymbolType::Namespace)
                           {
                               isNamespace = true;
                               return false;
                           }
                           return true;
                       });
    if (isTypeOrTemplate)
    {
        EmitExpressionIsDataType(valueNode, ctx, identName);
        return;
    }
    if (isNamespace)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(valueNode), "as-err-undefined-identifier", identName);
        return;
    }

    if (!FindFuncdef(declared.baseName, ctx.request.symbolTable) &&
        IsFunctionReference(valueNode, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(valueNode), "as-err-no-implicit-conversion",
                            {identName, declared.baseName});
    }
}

static void CheckHandleInitializer(TSNode valueNode, const ExpressionType& source, const DeclaredType& declared,
                                   DiagnosticContext& ctx)
{
    if (source.baseName == "null")
    {
        return;
    }
    if (source.isLiteral)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(valueNode), "as-err-no-implicit-conversion",
                            {source.baseName, declared.baseName});
        return;
    }

    if (source.isConst && !declared.qualifiers.isConst)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(valueNode), "as-err-no-implicit-conversion",
                            {"const " + source.baseName + "@", declared.baseName + "@"});
        return;
    }

    if (IsSameType(source.baseName, declared.baseName))
    {
        return;
    }

    if (ctx.request.symbolTable.HasSymbolAnywhere(source.baseName) &&
        ctx.request.symbolTable.HasSymbolAnywhere(declared.baseName))
    {
        const auto hierarchy = GetInheritedTypeHierarchy(declared.baseName, ctx.request.symbolTable);
        if (std::find(hierarchy.begin(), hierarchy.end(), source.baseName) != hierarchy.end())
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(valueNode), "as-err-no-implicit-conversion",
                                {source.baseName + "@", declared.baseName + "@"});
        }
    }
}

void CheckInitializer(TSNode declaratorNode, const DeclaredType& declared, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode valueNode =
        SyntaxOperands(declaratorNode, ctx.request.nodeIndex).Field(parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(valueNode) || ts_node_symbol(valueNode) == parser::GetGrammarSymbols().symInitializerList)
    {
        return;
    }

    if (const auto dataTypeName = IsBareDataType(valueNode, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(valueNode, ctx, *dataTypeName);
        return;
    }

    const ExpressionType source = ResolveValueType(valueNode, scope, ctx);
    if (ExpressionTypeCache::IsPoisonType(source.baseName))
    {
        return;
    }
    if (!source.known || source.baseName.empty())
    {
        CheckUnknownInitializerSource(valueNode, declared, scope, ctx);
        return;
    }

    if (!declared.qualifiers.isHandle)
    {
        CheckFloatTruncation(valueNode, {source.baseName, declared.baseName}, scope, ctx);
    }
    else
    {
        CheckHandleInitializer(valueNode, source, declared, ctx);
        return;
    }

    if (!IsConvertible(source.baseName, declared.baseName, ctx))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(valueNode), "as-err-no-implicit-conversion",
                            {source.baseName, declared.baseName});
    }
}

void CheckDeclaratorConstruction(TSNode child, const DeclaredType& declared, const Scope* scope, DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(child, ctx.request.nodeIndex);
    if (declared.isTemplateOrArray)
    {
        return;
    }

    const NonInstantiableKind nonInst = ClassifyNonInstantiable(declared.baseName, ctx.request.symbolTable);
    if (nonInst == NonInstantiableKind::Abstract)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(child), "as-err-abstract-instantiated",
                            {declared.baseName, declared.baseName});
        return;
    }

    TSNode argsNode = syntax.Field(parser::GetGrammarSymbols().fieldArguments);
    TSNode valNode = syntax.Field(parser::GetGrammarSymbols().fieldValue);
    if (ts_node_is_null(argsNode) && ts_node_is_null(valNode))
    {
        CheckDefaultConstructor(child, declared.baseName, ctx);
    }
    else if (!ts_node_is_null(argsNode))
    {
        CheckConstruction(argsNode, declared.baseName, scope, ctx);
    }
}

void ProcessDeclaredVariableDeclarators(TSNode node, TSNode typeNode, const TypeConversionCheckRequest& request,
                                        DiagnosticContext& ctx)
{
    const std::string fullType(parser::NodeText(typeNode, request.sourceCode));
    const std::string baseType = CleanBaseType(fullType);
    auto funcdefSym = FindFuncdef(baseType, ctx.request.symbolTable);

    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, ctx.request.nodeIndex);
    if (funcdefSym)
    {
        syntax.ForEachNamed(
            [&](TSNode child)
            {
                if (ts_node_symbol(child) == gs.symVariableDeclarator)
                {
                    TSNode valNode = SyntaxOperands(child, ctx.request.nodeIndex).Field(gs.fieldValue);
                    CheckFuncdefAssignment(child, funcdefSym->GetFuncdef(), valNode, ctx);
                }
                return true;
            });
    }

    const DeclaredType declared = ReadDeclaredType(typeNode, ctx);
    if (declared.usable && !IsMixinClass(declared.baseName, ctx.request.symbolTable))
    {
        const Scope* scope = ResolveNodeScope(node, request);
        syntax.ForEachNamed(
            [&](TSNode child)
            {
                if (ts_node_symbol(child) != gs.symVariableDeclarator)
                {
                    return true;
                }

                CheckInitializer(child, declared, scope, ctx);
                if (!declared.qualifiers.isHandle)
                {
                    CheckDeclaratorConstruction(child, declared, scope, ctx);
                }
                return true;
            });
    }
}

void ProcessVariableDeclarationNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(node, ctx.request.nodeIndex);
    TSNode typeNode = syntax.Field(parser::GetGrammarSymbols().fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = syntax.Field(parser::GetGrammarSymbols().fieldType);
    }

    const std::string rawType = CleanBaseType(parser::NodeText(typeNode, request.sourceCode));
    if (rawType == "auto")
    {
        const IndexedByteRange span{ts_node_start_byte(node), ts_node_end_byte(node)};
        if (const AutoDeclarationSiteFact* site = request.conversionSites->FindAutoDeclaration(span))
        {
            ProcessAutoDeclarationSite(*site, request, ctx);
        }
    }
    else
    {
        ProcessDeclaredVariableDeclarators(node, typeNode, request, ctx);
    }
}

/**
 * @brief Resolves the value type of an expression given as a syntax node by reading it into a fact first.
 * @param[in] node Expression node to resolve; a null node yields an unknown type.
 * @param[in] scope Lexical scope at the expression position.
 * @param[in] ctx Diagnostic context.
 * @return Resolved ExpressionType.
 */
ExpressionType ResolveValueType(TSNode node, const Scope* scope, const DiagnosticContext& ctx)
{
    return ResolveValueType(ReadValueExpression(node, ctx.request.nodeIndex, ctx.request.sourceCode), scope, ctx);
}

/**
 * @brief Validates a funcdef value given as syntax nodes by reading it into a fact first.
 * @param[in] targetNode Diagnostic anchor node; a node without a range ends the check.
 * @param[in] funcdefSig Expected funcdef signature.
 * @param[in] valueNode Assigned value node.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode valueNode,
                            DiagnosticContext& ctx)
{
    const auto range = ToOptionalSourceRange(targetNode);
    if (!range)
    {
        return;
    }
    CheckFuncdefAssignment(*range, funcdefSig,
                           ReadFuncdefValue(valueNode, *ctx.request.nodeIndex, ctx.request.sourceCode), ctx);
}

} // namespace angel_lsp::analysis
