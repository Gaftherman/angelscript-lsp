#pragma once

#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{

struct ExtractMethodStatements
{
    TSNode fnNode{};
    TSNode classNode{};
    std::vector<TSNode> selectedStmts{};
    uint32_t startByte = 0;
    uint32_t endByte = 0;
    TSPoint firstStart{};
    TSPoint lastEnd{};
    std::string selectedCode{};
};

struct VarInfo
{
    std::string name{};
    std::string typeName{};
    bool declaredInside = false;

    VarInfo() = default;
    VarInfo(std::string n, std::string t, bool d = false)
        : name(std::move(n)), typeName(std::move(t)), declaredInside(d)
    {
    }
};

struct ExtractedMethodVariables
{
    std::vector<VarInfo> inputParams;
    std::vector<VarInfo> outputVars;
};

struct ExtractedMethodPlan
{
    std::string returnType;
    std::string paramsStr;
    std::string argsStr;
    std::string callSiteText;
    std::string extractedBody;
};

std::optional<ExtractMethodStatements> FindSelectedStatements(TSNode rootNode, const CodeActionRequest& request);

std::vector<VarInfo> CollectMethodInputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope);

ankerl::unordered_dense::set<std::string> CollectMutatedVariables(const std::vector<TSNode>& stmts,
                                                                  std::string_view sourceCode);

std::vector<VarInfo> CollectMethodOutputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                          const ankerl::unordered_dense::set<std::string>& mutatedVars,
                                          const analysis::Scope* stmtScope);

ExtractedMethodPlan DeduceExtractedMethodSignature(const ExtractedMethodVariables& vars,
                                                   const ExtractMethodStatements& stmts,
                                                   const CodeActionRequest& request);

} // namespace angel_lsp::features
