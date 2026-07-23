/**
 * @file FunctionDeclRule.h
 * @brief Rule for validating function/method return values and signature duplicates.
 * @ingroup Analysis
 */

#pragma once

#include "analysis/validation/IValidationRule.h"
#include <set>
#include <string>

namespace analysis::validation
{
    class FunctionDeclRule : public IValidationRule
    {
    public:
        std::string getName() const override { return "FunctionDeclRule"; }

        std::vector<lsDiagnostic> run(const ValidationContext &ctx) override;

    private:
        void InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags, std::set<std::string> &seenSignatures);
        void CheckVoidReturns(TSNode bodyNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
    };
}
