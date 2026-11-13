#pragma once

#include "analysis/checkers/UndefinedIdentifierChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Collects the line ranges of all mixin classes declared in a file.
 * @param[in] symbolTable Workspace symbol table to query.
 * @param[in] fileUri URI of the file whose mixin declarations should be found.
 * @return Vector of pairs containing start and end line numbers for each mixin class.
 */
MixinRanges CollectMixinRanges(const SymbolTable& symbolTable, std::string_view fileUri);

/**
 * @brief Checks whether an identifier is declared as a member in any enclosing container or imported namespace.
 * @param[in] byteOffset Byte offset of the reference node in source code.
 * @param[in] refName Identifier name being inspected.
 * @param[in] usingNamespaces Active using-namespace import declarations for the document.
 * @param[in] ctx Diagnostic context containing request metadata, node index, and symbol table.
 * @return True if the identifier is a member of an enclosing container or imported namespace; false otherwise.
 */
bool IsEnclosingScopeMember(uint32_t byteOffset, std::string_view refName, std::span<const std::string> usingNamespaces,
                            const DiagnosticContext& ctx);

/**
 * @brief Checks whether an enum value can be named without qualification at a source position.
 * @param[in] byteOffset Byte offset of the reference node in source code.
 * @param[in] valueName Enum value name being inspected.
 * @param[in] usingNamespaces Active using-namespace import declarations for the document.
 * @param[in] ctx Diagnostic context containing request metadata, node index, and symbol table.
 * @return True when the value is global, or belongs to an enclosing namespace or class, or to an imported
 *         namespace; false when only an explicit qualifier would reach it.
 */
bool IsEnumValueReachable(uint32_t byteOffset, std::string_view valueName, std::span<const std::string> usingNamespaces,
                          const DiagnosticContext& ctx);

/**
 * @brief Checks whether a reference matches a reserved keyword, global accessor property, or container accessor.
 * @param[in] ref Local reference to validate.
 * @param[in] ctx Diagnostic context containing request metadata, rule index, and symbol table.
 * @return True if the reference corresponds to an accessor property or keyword; false otherwise.
 */
bool IsAccessorPropertyOrKeyword(const LocalReference& ref, const DiagnosticContext& ctx);

/**
 * @brief Iteratively validates all local references in the scope hierarchy against declarations and visible symbols.
 * @param[in] rootScope Root lexical scope to begin inspection from.
 * @param[in] uCtx Context bundling known global names, mixin ranges, using namespaces, and diagnostic sink.
 */
void CheckUndefinedIdentifiersIterative(const Scope* rootScope, const UndefinedIdentifierContext& uCtx);

} // namespace angel_lsp::analysis
