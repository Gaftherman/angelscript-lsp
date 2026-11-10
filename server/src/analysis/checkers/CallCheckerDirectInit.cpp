#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

static TSNode FindVariableTypeNode(TSNode varDeclNode)
{
    TSNode varTypeNode = parser::GetChildByField(varDeclNode, parser::GetGrammarSymbols().fieldVarType);
    if (!ts_node_is_null(varTypeNode))
    {
        return varTypeNode;
    }
    varTypeNode = parser::GetChildByField(varDeclNode, parser::GetGrammarSymbols().fieldType);
    if (!ts_node_is_null(varTypeNode))
    {
        return varTypeNode;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode foundType{};
    parser::ForEachNamedChildNode(varDeclNode,
                                  [&](TSNode child)
                                  {
                                      if (ts_node_is_null(foundType) && ts_node_symbol(child) == gs.symType)
                                      {
                                          foundType = child;
                                      }
                                  });
    if (!ts_node_is_null(foundType))
    {
        return foundType;
    }
    return varTypeNode;
}

static void CheckPrimitiveDirectInit(TSNode argListNode, const std::vector<std::string>& argTypes,
                                     const VarInitContext& vctx)
{
    if (argTypes.size() != 1)
    {
        const TSPoint aStart = ts_node_start_point(argListNode);
        const TSPoint aEnd = ts_node_end_point(argListNode);
        vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                             FormatSignatureAttempt(vctx.declaredType, argTypes));
    }
    else
    {
        ParameterInformation dummyParam;
        dummyParam.name = vctx.baseName;
        dummyParam.typeName = vctx.baseName;
        dummyParam.baseTypeName = vctx.baseName;
        const auto conv = EvaluateArgumentConversion(argTypes[0], dummyParam, vctx.ctx.request.symbolTable);
        if (!conv.IsViable())
        {
            const TSPoint aStart = ts_node_start_point(argListNode);
            const TSPoint aEnd = ts_node_end_point(argListNode);
            vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-implicit-conversion",
                                 argTypes[0], vctx.baseName);
        }
    }
}

static void CheckDeclaratorDirectInit(TSNode declarator, const VarInitContext& vctx)
{
    TSNode argListNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldArguments);
    if (ts_node_is_null(argListNode) || ts_node_has_error(argListNode))
    {
        return;
    }

    const TSPoint start = ts_node_start_point(declarator);
    const Scope* scope = FindInnermostScope(vctx.request.scopeRoot, start.row, start.column);

    const std::vector<TSNode> argNodes = GetArgumentNodes(argListNode);
    std::vector<std::string> argTypes;
    for (TSNode argNode : argNodes)
    {
        argTypes.push_back(ResolveExpressionType(argNode, ExpressionTypeContext(scope, vctx.ctx)));
    }
    if (std::any_of(argTypes.begin(), argTypes.end(),
                    [](const std::string& t) { return ExpressionTypeCache::IsPoisonType(t); }))
    {
        return;
    }

    if (IsCorePrimitive(vctx.baseName))
    {
        CheckPrimitiveDirectInit(argListNode, argTypes, vctx);
        return;
    }

    const std::vector<Symbol> rawConstructors = LookupRawConstructors(vctx.baseName, vctx.ctx.request.symbolTable);
    if (rawConstructors.empty())
    {
        if (argTypes.empty())
        {
            return;
        }
        if (IsClassDeclarationVisible(vctx.baseName, vctx.ctx.request.symbolTable,
                                      vctx.ctx.request.predefinedFileExtension))
        {
            const TSPoint aStart = ts_node_start_point(argListNode);
            const TSPoint aEnd = ts_node_end_point(argListNode);
            vctx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-matching-constructor",
                                 FormatSignatureAttempt(vctx.declaredType, argTypes));
        }
        return;
    }

    const std::vector<Symbol> candidates =
        SpecializeConstructors(rawConstructors, vctx.tmplInfo, vctx.baseName, vctx.ctx.request.symbolTable);
    CheckConstructorOverload(argListNode, candidates, argTypes, vctx);
}

void CheckVariableDirectInitialization(TSNode varDeclNode, const AstCheckContext& request, DiagnosticContext& ctx)
{
    TSNode varTypeNode = FindVariableTypeNode(varDeclNode);
    if (ts_node_is_null(varTypeNode))
    {
        return;
    }

    const std::string rawTypeStr(parser::NodeText(varTypeNode, request.sourceCode));
    const std::string declaredType = CleanExpressionType(rawTypeStr);
    if (declaredType.empty() || declaredType == "auto" || rawTypeStr.find('[') != std::string::npos)
    {
        return;
    }

    const TemplateTypeInfo tmplInfo = ParseTemplateType(declaredType);
    std::string baseName = tmplInfo.containerName.empty() ? declaredType : tmplInfo.containerName;
    baseName = CleanBaseType(baseName);

    const VarInitContext vctx{declaredType, baseName, tmplInfo, request, ctx};
    const auto& gs = parser::GetGrammarSymbols();
    parser::ForEachNamedChildNode(varDeclNode,
                                  [&](TSNode declarator)
                                  {
                                      if (ts_node_symbol(declarator) == gs.symVariableDeclarator)
                                      {
                                          CheckDeclaratorDirectInit(declarator, vctx);
                                      }
                                  });
}

} // namespace angel_lsp::analysis
