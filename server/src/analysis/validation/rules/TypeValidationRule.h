/**
 * @file TypeValidationRule.h
 * @brief Rule for validating that types in function, variable, and property declarations exist.
 * @ingroup Analysis
 */

#pragma once

#include "analysis/validation/IValidationRule.h"

namespace analysis::validation
{
    class TypeValidationRule : public IValidationRule
    {
    public:
        std::string getName() const override { return "TypeValidationRule"; }

        std::vector<lsDiagnostic> run(const ValidationContext &ctx) override;

    private:
        void InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
        void CheckTypeNode(TSNode typeNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
    };
}
