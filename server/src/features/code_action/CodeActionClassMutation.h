#pragma once

#include "analysis/index/ClassMutationFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"

#include <string>
#include <string_view>

namespace angel_lsp::features
{

/**
 * @brief Context for evaluating whether a method body mutates class state.
 */
struct ClassMutationContext
{
    /// Pre-indexed mutation facts and candidate sites for the method body.
    const analysis::IndexedMethodMutationFacts& facts;
    /// Document source text.
    std::string_view sourceCode;
    /// Workspace symbol table.
    const analysis::SymbolTable& table;
    /// Enclosing class name.
    std::string className;
    /// Enclosing lexical scope.
    const analysis::Scope* scope = nullptr;
};

/**
 * @brief Checks if a method body mutates class fields or calls non-const methods on `this`.
 * @param[in] context Mutation evaluation context.
 * @return True if method body mutates class state.
 */
bool MethodBodyMutatesClassState(const ClassMutationContext& context);

} // namespace angel_lsp::features
