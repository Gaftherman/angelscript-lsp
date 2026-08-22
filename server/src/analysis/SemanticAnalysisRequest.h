#pragma once

#include "analysis/SymbolTable.h"
#include "analysis/ScopeTree.h"
#include "analysis/Diagnostics.h"
#include "i18n/i18n.h"
#include "config/ServerConfig.h"

#include <memory>
#include <string>
#include <string_view>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::analysis
{
    /**
     * @brief Context and configuration passed into the semantic analysis process.
     */
    struct SemanticAnalysisRequest
    {
        const SymbolTable &symbolTable;
        std::string fileUri;
        std::string predefinedFileExtension;
        const i18n::I18n *i18n = nullptr;
        const config::TypeConfig *typeConfig = nullptr;
        const ankerl::unordered_dense::map<std::string, DiagnosticSeverity> *severityOverrides = nullptr;

        /** @brief Root of the document's lexical Scope tree (see ScopeTree.h), or nullptr if none was collected. */
        std::shared_ptr<const Scope> scopeRoot;

        /**
         * @brief Gets configured name for the string type or 'string' default.
         */
        std::string_view GetStringTypeName() const
        {
            return (typeConfig && !typeConfig->stringTypeName.empty()) ? std::string_view(typeConfig->stringTypeName) : std::string_view("string");
        }

        /**
         * @brief Gets configured name for the array type or 'array' default.
         */
        std::string_view GetArrayTypeName() const
        {
            return (typeConfig && !typeConfig->arrayTypeName.empty()) ? std::string_view(typeConfig->arrayTypeName) : std::string_view("array");
        }

        /**
         * @brief Checks if a symbol name is in the registered engine symbol allowlist.
         */
        bool IsRegisteredSymbol(const std::string &name) const
        {
            return typeConfig && typeConfig->registeredSymbols.contains(name);
        }
    };
}
