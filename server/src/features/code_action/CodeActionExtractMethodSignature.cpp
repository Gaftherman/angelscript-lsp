/**
 * @file CodeActionExtractMethodSignature.cpp
 * @brief Signature deduction and call-site plan generation for extracted method.
 */

#include "features/code_action/CodeActionExtractMethodAnalysis.h"
#include "utils/Utils.h"

namespace angel_lsp::features
{

static void BuildMethodParamsAndArgs(const std::vector<VarInfo>& inputParams, const std::vector<VarInfo>& outputVars,
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

/**
 * @brief Deduces method return type and call-site invocation text.
 * @param[in] vars Discovered input and output variables.
 * @param[in] stmts Extracted statement metadata.
 * @param[in] query Incoming code action query context.
 * @param[in,out] plan Extracted method plan populated with return type and call site.
 */
static void DeduceReturnAndCallSite(const ExtractedMethodVariables& vars, const ExtractMethodStatements& stmts,
                                    const analysis::code_action::CodeActionQuery& query, ExtractedMethodPlan& plan)
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
        if (stmts.selection.endsWithReturn)
        {
            const std::string_view retType =
                utils::SliceSource(query.sourceCode, stmts.selection.functionReturnType.startByte,
                                   stmts.selection.functionReturnType.endByte);
            if (!retType.empty())
            {
                plan.returnType = std::string(retType);
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

/**
 * @brief Deduces method signature and call-site plan for extracted statements.
 * @param[in] vars Discovered input and output variables.
 * @param[in] stmts Extracted statement metadata.
 * @param[in] query Incoming code action query context.
 * @return Extracted method signature and call-site plan.
 */
ExtractedMethodPlan DeduceExtractedMethodSignature(const ExtractedMethodVariables& vars,
                                                   const ExtractMethodStatements& stmts,
                                                   const analysis::code_action::CodeActionQuery& query)
{
    ExtractedMethodPlan plan;
    plan.extractedBody = stmts.selectedCode;
    BuildMethodParamsAndArgs(vars.inputParams, vars.outputVars, plan.paramsStr, plan.argsStr);
    DeduceReturnAndCallSite(vars, stmts, query, plan);
    return plan;
}

} // namespace angel_lsp::features
