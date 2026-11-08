#pragma once

#include "analysis/index/ExtractMethodFacts.h"
#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{

/**
 * @brief Extracted statement metadata and source text for extract method refactoring.
 */
struct ExtractMethodStatements
{
    /// Extracted syntactic selection metadata.
    analysis::IndexedExtractMethodSelection selection{};
    /// Extracted statement block source text.
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

/**
 * @brief Context tracking extracted method output parameters, return values, and mutability flags.
 */
struct MethodOutputContext
{
    const ExtractMethodStatements& stmts;
    const analysis::Scope* fnScope;
    const analysis::Scope* stmtScope;
};

std::optional<ExtractMethodStatements> FindSelectedStatements(const CodeActionRequest& request);

std::vector<VarInfo> CollectMethodInputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope);

ankerl::unordered_dense::set<std::string> CollectMutatedVariables(const ExtractMethodStatements& stmts,
                                                                  std::string_view sourceCode);

std::vector<VarInfo> CollectMethodOutputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                          const ankerl::unordered_dense::set<std::string>& mutatedVars,
                                          const analysis::Scope* stmtScope);

ExtractedMethodPlan DeduceExtractedMethodSignature(const ExtractedMethodVariables& vars,
                                                   const ExtractMethodStatements& stmts,
                                                   const CodeActionRequest& request);

} // namespace angel_lsp::features
