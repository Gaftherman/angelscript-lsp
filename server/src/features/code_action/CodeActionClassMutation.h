#pragma once

#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include <tree_sitter/api.h>
#include <string>
#include <string_view>

namespace angel_lsp::features
{

struct ClassMutationContext
{
    TSNode bodyNode;
    TSNode classNode;
    std::string_view sourceCode;
    const analysis::SymbolTable& table;
    const std::string& className;
    const analysis::Scope* scope = nullptr;
};

/**
 * @brief Checks if a method body mutates class fields or calls non-const methods on `this`.
 * @param[in] context Mutation evaluation context.
 * @return True if method body mutates class state.
 */
bool MethodBodyMutatesClassState(const ClassMutationContext& context);

} // namespace angel_lsp::features
