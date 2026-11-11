#include "analysis/index/InlayHintFacts.h"
#include "analysis/index/InlayHintFactsInternal.h"
#include "analysis/index/CallArgumentFacts.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Records auto-typed declarators and direct constructor-init declarators of one variable_declaration.
 * @param[in] node variable_declaration node visited by the pass-1 walk.
 * @param[in] sourceCode Document text the recorded views point into.
 * @param[in] gs Precompiled grammar symbol and field IDs.
 * @param[in,out] sites Flat site container receiving autoVars, ctorInits and their callArguments.
 */
void ProcessVariableDeclarationNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs,
                                    InlayHintSites& sites)
{
    TSNode varTypeNode = parser::GetChildByField(node, gs.fieldVarType);
    if (ts_node_is_null(varTypeNode))
    {
        parser::ForEachChildNodeUntil(node,
                                      [&](TSNode child)
                                      {
                                          if (ts_node_symbol(child) == gs.symType)
                                          {
                                              varTypeNode = child;
                                              return false;
                                          }
                                          return true;
                                      });
    }
    if (ts_node_is_null(varTypeNode))
    {
        return;
    }

    std::string_view typeText =
        utils::SliceSource(sourceCode, ts_node_start_byte(varTypeNode), ts_node_end_byte(varTypeNode));
    const bool isAuto = (CleanBaseTypeView(typeText) == "auto");
    const bool isHandle = IsHandleTypeView(typeText);

    parser::ForEachChildNode(node,
                             [&](TSNode child)
                             {
                                 if (ts_node_symbol(child) != gs.symVariableDeclarator)
                                 {
                                     return;
                                 }
                                 if (isAuto)
                                 {
                                     TSNode nameNode = parser::GetChildByField(child, gs.fieldName);
                                     TSNode initExpr = parser::GetChildByField(child, gs.fieldValue);
                                     if (ts_node_is_null(initExpr))
                                     {
                                         initExpr = parser::GetChildByField(child, gs.fieldArguments);
                                     }
                                     if (!ts_node_is_null(nameNode) && !ts_node_is_null(initExpr))
                                     {
                                         IndexedAutoDeclarator decl;
                                         decl.nameRange = ToSourceRange(nameNode);
                                         decl.startByte = ts_node_start_byte(child);
                                         decl.endByte = ts_node_end_byte(child);
                                         const TSPoint nameEnd = ts_node_end_point(nameNode);
                                         decl.nameEndLine = nameEnd.row;
                                         decl.nameEndCharacter = nameEnd.column;
                                         decl.initExprStartByte = ts_node_start_byte(initExpr);
                                         decl.initExprEndByte = ts_node_end_byte(initExpr);
                                         decl.isHandle = isHandle;
                                         sites.autoVars.push_back(decl);
                                     }
                                 }
                                 else if (!typeText.empty())
                                 {
                                     TSNode argList = parser::GetChildByField(child, gs.fieldArguments);
                                     if (ts_node_is_null(argList))
                                     {
                                         parser::ForEachChildNodeUntil(child,
                                                                       [&](TSNode ch)
                                                                       {
                                                                           if (ts_node_symbol(ch) == gs.symArgumentList)
                                                                           {
                                                                               argList = ch;
                                                                               return false;
                                                                           }
                                                                           return true;
                                                                       });
                                     }
                                     if (!ts_node_is_null(argList))
                                     {
                                         IndexedConstructorInitDeclarator ctor;
                                         ctor.declRange = ToSourceRange(child);
                                         ctor.declaredTypeName = typeText;
                                         ctor.startByte = ts_node_start_byte(child);
                                         ctor.endByte = ts_node_end_byte(child);
                                         ctor.hasArgumentList = true;
                                         PopulateArgListCoords(argList, ctor.emptyArgsLine, ctor.emptyArgsCharacter,
                                                               ctor.closeParenLine, ctor.closeParenCharacter);
                                         ctor.firstArgument = static_cast<uint32_t>(sites.callArguments.size());
                                         ctor.argumentCount = ExtractArguments(argList, sourceCode, sites);
                                         sites.ctorInits.push_back(ctor);
                                     }
                                 }
                             });
}

void ProcessLambdaNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs,
                       InlayHintSites& sites)
{
    TSNode params = parser::GetChildByField(node, gs.fieldParameters);
    if (ts_node_is_null(params))
    {
        for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
        {
            TSNode ch = ts_node_named_child(node, i);
            if (ts_node_symbol(ch) == gs.symLambdaParameterList)
            {
                params = ch;
                break;
            }
        }
    }
    if (ts_node_is_null(params))
    {
        return;
    }

    IndexedLambdaSite site;
    site.lambdaRange = ToSourceRange(node);
    site.startByte = ts_node_start_byte(node);
    site.endByte = ts_node_end_byte(node);
    const TSPoint endPt = ts_node_end_point(params);
    site.returnTypeHintLine = endPt.row;
    site.returnTypeHintCharacter = endPt.column;
    site.hasExplicitReturnType = !ts_node_is_null(parser::GetChildByField(node, gs.fieldType));

    site.firstParam = static_cast<uint32_t>(sites.lambdaParams.size());
    for (const auto& p : ExtractLambdaParameters(params, sourceCode))
    {
        if (!p.hasWrittenType && !ts_node_is_null(p.nameNode))
        {
            const TSPoint pt = ts_node_start_point(p.nameNode);
            const uint32_t sB = ts_node_start_byte(p.nameNode);
            const uint32_t eB = ts_node_end_byte(p.nameNode);
            sites.lambdaParams.push_back(
                IndexedLambdaParam{utils::SliceSource(sourceCode, sB, eB), sB, eB, pt.row, pt.column});
        }
    }
    site.paramCount = static_cast<uint32_t>(sites.lambdaParams.size()) - site.firstParam;

    TSNode body = parser::GetChildByField(node, gs.fieldBody);
    if (ts_node_is_null(body))
    {
        for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
        {
            TSNode ch = ts_node_named_child(node, i);
            if (ts_node_symbol(ch) == gs.symStatementBlock)
            {
                body = ch;
                break;
            }
        }
    }
    if (!ts_node_is_null(body))
    {
        for (uint32_t i = 0; i < ts_node_named_child_count(body); ++i)
        {
            TSNode stmt = ts_node_named_child(body, i);
            if (ts_node_symbol(stmt) == gs.symReturnStatement)
            {
                site.hasReturnStatement = true;
                TSNode retVal = parser::GetChildByField(stmt, gs.fieldValue);
                if (ts_node_is_null(retVal) && ts_node_named_child_count(stmt) > 0)
                {
                    retVal = ts_node_named_child(stmt, 0);
                }
                if (!ts_node_is_null(retVal))
                {
                    site.returnExprStartByte = ts_node_start_byte(retVal);
                    site.returnExprEndByte = ts_node_end_byte(retVal);
                }
                break;
            }
        }
    }

    sites.lambdas.push_back(site);
}

void ProcessEnumNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs, InlayHintSites& sites)
{
    IndexedEnumDeclaration decl;
    decl.enumRange = ToSourceRange(node);
    decl.startByte = ts_node_start_byte(node);
    decl.endByte = ts_node_end_byte(node);
    decl.firstValue = static_cast<uint32_t>(sites.enumValues.size());

    int64_t nextValue = 0;
    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(node, i);
        if (ts_node_symbol(child) != gs.symEnumMember)
        {
            continue;
        }

        TSNode valNode = parser::GetChildByField(child, gs.fieldValue);
        TSNode nameNode = parser::GetChildByField(child, gs.fieldName);
        if (ts_node_is_null(nameNode) && ts_node_named_child_count(child) > 0)
        {
            nameNode = ts_node_named_child(child, 0);
        }

        if (!ts_node_is_null(valNode))
        {
            std::string_view valText =
                utils::SliceSource(sourceCode, ts_node_start_byte(valNode), ts_node_end_byte(valNode));
            std::optional<int64_t> parsedVal;
            if (!valText.empty())
            {
                try
                {
                    int64_t parsed = std::stoll(std::string(valText), nullptr, 0);
                    parsedVal = parsed;
                    nextValue = parsed + 1;
                }
                catch (...)
                {
                    nextValue++;
                }
            }
            else
            {
                nextValue++;
            }

            if (!ts_node_is_null(nameNode))
            {
                IndexedEnumValue val;
                val.startByte = ts_node_start_byte(child);
                val.endByte = ts_node_end_byte(child);
                val.memberName =
                    utils::SliceSource(sourceCode, ts_node_start_byte(nameNode), ts_node_end_byte(nameNode));
                const TSPoint endPt = ts_node_end_point(nameNode);
                val.hintLine = endPt.row;
                val.hintCharacter = endPt.column;
                val.hasExplicitValue = true;
                val.explicitValue = parsedVal;
                val.implicitValue = parsedVal.value_or(nextValue - 1);
                sites.enumValues.push_back(val);
            }
        }
        else if (!ts_node_is_null(nameNode))
        {
            IndexedEnumValue val;
            val.startByte = ts_node_start_byte(child);
            val.endByte = ts_node_end_byte(child);
            val.memberName = utils::SliceSource(sourceCode, ts_node_start_byte(nameNode), ts_node_end_byte(nameNode));
            const TSPoint endPt = ts_node_end_point(nameNode);
            val.hintLine = endPt.row;
            val.hintCharacter = endPt.column;
            val.hasExplicitValue = false;
            val.implicitValue = nextValue;
            sites.enumValues.push_back(val);
            nextValue++;
        }
    }
    decl.valueCount = static_cast<uint32_t>(sites.enumValues.size()) - decl.firstValue;
    sites.enumDeclarations.push_back(decl);
}

} // namespace angel_lsp::analysis
