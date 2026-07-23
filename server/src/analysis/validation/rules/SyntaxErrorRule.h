/**
 * @file SyntaxErrorRule.h
 * @brief Rule for scanning Tree-Sitter AST syntax errors (ERROR and MISSING nodes).
 * @ingroup Analysis
 */

#pragma once

#include "analysis/validation/IValidationRule.h"

namespace analysis::validation
{
    class SyntaxErrorRule : public IValidationRule
    {
    public:
        std::string getName() const override { return "SyntaxErrorRule"; }

        std::vector<lsDiagnostic> run(const ValidationContext &ctx) override;

    private:
        void TraverseNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags);
    };
}
