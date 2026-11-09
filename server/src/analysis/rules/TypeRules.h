#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Validates a typedef declaration: its base type must exist and be primitive.
 */
void ValidateTypedef(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Validates a funcdef declaration: name, return type and parameter shape.
 */
void ValidateFuncdef(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Validates an enum declaration: name, body and member initializers.
 */
void ValidateEnum(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Validates what an interface may declare, beyond the inheritance rules ClassRules owns.
 */
void ValidateInterfaceMembers(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Reports declarations that collide under one qualified name.
 *        Takes the whole overload bucket rather than one symbol: whether a redeclaration is legal is a
 * @param symbols Every symbol registered under one qualified name.
 */
void ValidateDuplicates(const std::vector<Symbol>& symbols, const DiagnosticContext& ctx);
/**
 * @brief True when the name collides with a keyword or a built-in type name.
 */
bool IsUnusableName(std::string_view name, const DiagnosticContext& ctx);

/**
 * @brief Formats a symbol type into the noun the compiler uses in as-err-name-conflict.
 */
std::string_view KindWord(SymbolType type);

} // namespace angel_lsp::analysis::rules
