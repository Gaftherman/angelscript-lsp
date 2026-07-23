/**
 * @file ValidationUtils.h
 * @brief Helper utility functions for AST traversal, position conversion, and type validation.
 * @ingroup Analysis
 */

#pragma once

#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include "ValidationContext.h"
#include "IValidationRule.h"

namespace analysis::validation
{
    namespace ValidationUtils
    {
        /**
         * @brief Converts a Tree-Sitter TSPoint into an LSP Position object.
         */
        lsPosition tsPointToLsPosition(TSPoint point);

        /**
         * @brief Converts a Tree-Sitter TSNode range into an LSP Range object.
         */
        lsRange tsNodeToLsRange(TSNode node);

        /**
         * @brief Extracts the raw source text substring corresponding to a TSNode.
         */
        std::string getNodeText(TSNode node, const Document &doc);

        /**
         * @brief Finds all direct child nodes matching a specific Tree-Sitter node type.
         */
        std::vector<TSNode> findChildrenByType(TSNode parent, const std::string &typeName);

        /**
         * @brief Retrieves a child node by its field name (returns a null node if not found).
         */
        TSNode getChildByFieldName(TSNode parent, const std::string &fieldName);

        /**
         * @brief Checks if a given type name is a valid primitive or registered symbol type in the current context.
         */
        bool isValidType(const std::string &typeName, const ValidationContext &ctx);

        /**
         * @brief Constructs an LSP Diagnostic object with the specified range, message, and severity.
         */
        lsDiagnostic createDiagnostic(lsRange range, const std::string &message, lsDiagnosticSeverity severity = lsDiagnosticSeverity::Error);
    }
}
