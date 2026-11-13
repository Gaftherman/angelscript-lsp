#pragma once

#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/code_action/CodeActionTypes.h"
#include "analysis/index/ExtractMethodFacts.h"
#include "utils/TransparentHash.h"

namespace angel_lsp::analysis::code_action
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

/**
 * @brief Identifies candidate statements within the selected query range.
 * @param[in] query Incoming code action query context.
 * @return Extracted statement metadata if selection is valid, std::nullopt otherwise.
 */
std::optional<ExtractMethodStatements> FindSelectedStatements(const CodeActionQuery& query);

std::vector<VarInfo> CollectMethodInputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope);

TransparentStringSet CollectMutatedVariables(const ExtractMethodStatements& stmts, std::string_view sourceCode);

std::vector<VarInfo> CollectMethodOutputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                          const TransparentStringSet& mutatedVars, const analysis::Scope* stmtScope);

/**
 * @brief Deduces method signature and call-site plan for extracted statements.
 * @param[in] vars Discovered input and output variables.
 * @param[in] stmts Extracted statement metadata.
 * @param[in] query Incoming code action query context.
 * @return Extracted method signature and call-site plan.
 */
ExtractedMethodPlan DeduceExtractedMethodSignature(const ExtractedMethodVariables& vars,
                                                   const ExtractMethodStatements& stmts, const CodeActionQuery& query);

} // namespace angel_lsp::analysis::code_action
