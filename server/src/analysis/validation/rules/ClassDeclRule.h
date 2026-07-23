/**
 * @file ClassDeclRule.h
 * @brief Rule for validating class/struct declarations, base class inheritance, and duplicate members.
 * @ingroup Analysis
 */

#pragma once

#include "analysis/validation/IValidationRule.h"

namespace analysis::validation
{
    class ClassDeclRule : public IValidationRule
    {
    public:
        std::string getName() const override { return "ClassDeclRule"; }

        std::vector<lsDiagnostic> run(const ValidationContext &ctx) override;

    private:
        void InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
        void CheckClassInheritance(TSNode classNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
        void CheckClassBodyMembers(TSNode bodyNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
    };
}
