/**
 * @file CodeActionExtractMethodSignature.cpp
 * @brief Signature deduction and call-site plan generation for extracted method.
 */

#include "features/code_action/CodeActionExtractMethodAnalysis.h"

namespace angel_lsp::features
{
namespace
{

void BuildMethodParamsAndArgs(const std::vector<VarInfo>& inputParams, const std::vector<VarInfo>& outputVars,
                              std::string& paramsStr, std::string& argsStr)
{
    std::vector<VarInfo> effectiveInputs;
    for (const auto& inp : inputParams)
    {
        bool isOutParam = false;
        for (size_t k = 1; k < outputVars.size(); ++k)
        {
            if (outputVars[k].name == inp.name)
            {
                isOutParam = true;
                break;
            }
        }
        if (!isOutParam)
        {
            effectiveInputs.push_back(inp);
        }
    }

    for (size_t i = 0; i < effectiveInputs.size(); ++i)
    {
        if (i > 0)
        {
            paramsStr += ", ";
            argsStr += ", ";
        }
        paramsStr += effectiveInputs[i].typeName + " " + effectiveInputs[i].name;
        argsStr += effectiveInputs[i].name;
    }
}

void DeduceReturnAndCallSite(const ExtractedMethodVariables& vars, const ExtractMethodStatements& stmts,
                             const CodeActionRequest& request, ExtractedMethodPlan& plan)
{
    const std::string methodName = "NewMethod";
    plan.returnType = "void";

    if (vars.outputVars.size() == 1)
    {
        plan.returnType = vars.outputVars[0].typeName;
        if (plan.extractedBody.find("return " + vars.outputVars[0].name) == std::string::npos &&
            !plan.extractedBody.ends_with("return " + vars.outputVars[0].name + ";"))
        {
            plan.extractedBody += "\n    return " + vars.outputVars[0].name + ";";
        }
        if (vars.outputVars[0].declaredInside)
        {
            plan.callSiteText =
                plan.returnType + " " + vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
        else
        {
            plan.callSiteText = vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
    }
    else if (vars.outputVars.empty())
    {
        TSNode lastNode = stmts.selectedStmts.back();
        if (std::string_view(ts_node_type(lastNode)) == "return_statement")
        {
            TSNode retTypeNode = parser::GetChildByField(stmts.fnNode, parser::fields::Type);
            if (!ts_node_is_null(retTypeNode))
            {
                plan.returnType = GetNodeText(retTypeNode, request.sourceCode);
            }
        }
        plan.callSiteText = methodName + "(" + plan.argsStr + ");";
    }
    else
    {
        plan.returnType = vars.outputVars[0].typeName;
        for (size_t i = 1; i < vars.outputVars.size(); ++i)
        {
            if (!plan.paramsStr.empty())
                plan.paramsStr += ", ";
            if (!plan.argsStr.empty())
                plan.argsStr += ", ";
            plan.paramsStr += vars.outputVars[i].typeName + " &out " + vars.outputVars[i].name;
            plan.argsStr += vars.outputVars[i].name;
        }
        if (plan.extractedBody.find("return " + vars.outputVars[0].name) == std::string::npos &&
            !plan.extractedBody.ends_with("return " + vars.outputVars[0].name + ";"))
        {
            plan.extractedBody += "\n    return " + vars.outputVars[0].name + ";";
        }
        if (vars.outputVars[0].declaredInside)
        {
            plan.callSiteText =
                plan.returnType + " " + vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
        else
        {
            plan.callSiteText = vars.outputVars[0].name + " = " + methodName + "(" + plan.argsStr + ");";
        }
    }
}

} // namespace

ExtractedMethodPlan DeduceExtractedMethodSignature(const ExtractedMethodVariables& vars,
                                                   const ExtractMethodStatements& stmts,
                                                   const CodeActionRequest& request)
{
    ExtractedMethodPlan plan;
    plan.extractedBody = stmts.selectedCode;
    BuildMethodParamsAndArgs(vars.inputParams, vars.outputVars, plan.paramsStr, plan.argsStr);
    DeduceReturnAndCallSite(vars, stmts, request, plan);
    return plan;
}

} // namespace angel_lsp::features
