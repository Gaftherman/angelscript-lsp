#pragma once

#include "analysis/index/CallBindings.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/EngineRuleConfig.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Semantic input parameters for a definition or type definition query.
 */
struct DefinitionQuery
{
    std::string_view uri{};
    std::string_view sourceCode{};
    const SymbolTable& symbolTable;
    const ScopeIndex& scopeIndex;
    uint32_t line = 0;
    uint32_t character = 0;
    std::string_view predefinedExtension = ".as.predefined";
    const config::EngineRuleConfig* engineRules = nullptr;
    std::optional<CallBindingRevision> callBindingRevision = std::nullopt;

    /**
     * @brief Pre-extracted identifier token text under cursor (e.g. "MyVar" or "Foo").
     */
    std::string_view identifier{};

    /**
     * @brief Pre-extracted scoped identifier text under cursor if applicable (e.g. "Outer::Inner::MyVar").
     */
    std::string_view scopedIdentifier{};
};

/**
 * @brief Unified semantic target result from a Go-To-Definition lookup.
 */
struct DefinitionTargetResult
{
    std::vector<Symbol> symbols;
    std::optional<SourceRange> localRange = std::nullopt;
    std::string_view localUri{};
    std::optional<CallDeclaration> callDeclaration = std::nullopt;

    [[nodiscard]] bool HasResult() const noexcept
    {
        return !symbols.empty() || localRange.has_value() || callDeclaration.has_value();
    }
};

/**
 * @brief Finds the innermost local reference at the specified line and character position.
 * @param[in] scope Lexical scope to inspect.
 * @param[in] line Document line coordinate (0-based).
 * @param[in] character Document column coordinate (0-based).
 * @return Pointer to matching LocalReference, or nullptr if none.
 */
[[nodiscard]] const LocalReference* FindReferenceAt(const Scope* scope, uint32_t line, uint32_t character) noexcept;

/**
 * @brief Mapped location target including destination URI and range.
 */
struct MappedLocationTarget
{
    std::string_view targetUri{};
    SourceRange range{};
};

/**
 * @brief Resolves the destination URI and source range of a symbol's declaration.
 * @param[in] sym Target symbol.
 * @param[in] requestUri Document URI being queried; used when the symbol has no file URI.
 * @return Mapped target URI and source range, preferring the symbol's full range.
 */
[[nodiscard]] MappedLocationTarget MapSymbolLocation(const Symbol& sym, std::string_view requestUri);

/**
 * @brief Resolves definition targets for the symbol at the queried position.
 * @param[in] query Semantic definition query parameters.
 * @return Populated DefinitionTargetResult with resolved symbols, local definitions, or call declarations.
 */
[[nodiscard]] DefinitionTargetResult ResolveDefinitionTarget(const DefinitionQuery& query);

/**
 * @brief Resolves type definition targets for the symbol at the queried position.
 * @param[in] query Semantic definition query parameters.
 * @return List of matching user-defined type symbols.
 */
[[nodiscard]] std::vector<Symbol> ResolveTypeDefinitionTarget(const DefinitionQuery& query);

/**
 * @brief Finds the tightest namespace declared in a document whose range covers a line.
 * @param[in] uri Document URI; symbols stored under the equivalent file path also match.
 * @param[in] line 0-based document line.
 * @param[in] table Symbol table providing the document's namespace declarations.
 * @return Qualified name of the innermost namespace, or an empty string at global scope.
 */
[[nodiscard]] std::string FindEnclosingNamespace(std::string_view uri, uint32_t line, const SymbolTable& table);

} // namespace angel_lsp::analysis
