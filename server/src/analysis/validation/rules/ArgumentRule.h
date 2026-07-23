/**
 * @file ArgumentRule.h
 * @brief Rule for validating function parameter types and detecting duplicate parameter names.
 * @ingroup Analysis
 */

#pragma once

#include "analysis/validation/IValidationRule.h"

namespace analysis::validation
{
    class ArgumentRule : public IValidationRule
    {
    public:
        std::string getName() const override { return "ArgumentRule"; }

        std::vector<lsDiagnostic> run(const ValidationContext &ctx) override;

    private:
        void InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
        void ValidateParameterList(TSNode paramsNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
    };
}
