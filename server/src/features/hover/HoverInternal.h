#pragma once

#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "features/hover/HoverHandler.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"
#include "utils/Timer.h"

#include <cstdint>
#include <lsp/types.h>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Per-request latency trace, logged once when the hover finishes (on destruction at the latest).
 * @warning The text fields are views emitted after every hover local is gone; point them only at request
 *          source text or string literals.
 */
struct HoverProfiler
{
    utils::HighResTimer totalTimer{};                ///< Started when the request begins.
    angel_lsp::utils::LspLogger* m_logger = nullptr; ///< LSP log sink; null disables the LSP line.
    uint32_t line = 0;                               ///< Requested line.
    uint32_t character = 0;                          ///< Requested column.
    std::string_view nodeType{};                     ///< Hover path label, a literal.
    std::string_view pathText{};                     ///< Hovered or scoped text, a view into the source.
    std::string_view symbolName{};                   ///< Hovered name, a view into the source or a literal.
    double nodeMs = 0.0;                             ///< Time spent locating the hover site.
    double symMs = 0.0;                              ///< Time spent resolving symbols.
    double fmtMs = 0.0;                              ///< Time spent building markdown.
    bool emitted = false;                            ///< Set once the trace has been logged.

    /** @brief Tells whether any log sink would record the trace. @return True when a sink is attached. */
    [[nodiscard]] bool IsActive() const noexcept;
    /** @brief Logs the trace to the LSP logger and the hover file log, once. */
    void Emit();
    /** @brief Emits the trace if no earlier path did. */
    ~HoverProfiler();
};

/**
 * @brief Everything one hover request needs after the site is located: request, site facts, scopes, mixin host.
 */
struct HoverQueryContext
{
    const HoverRequest& request;                        ///< Originating request.
    HoverProfiler& profiler;                            ///< Latency trace for this request.
    const analysis::IndexedHoverSite& site;             ///< Pass-1 facts about the hovered node.
    std::string_view nodeText{};                        ///< Hovered node text, a view into the source.
    lsp::Range range{};                                 ///< Range of the hovered node.
    std::shared_ptr<const analysis::Scope> rootScope{}; ///< Root scope of the hovered document.
    uint32_t queryLine = 0;                             ///< Request line in the hovered document.
    const analysis::Scope* scope = nullptr;             ///< Innermost scope at the request position.
};

/**
 * @brief Builds the documentation lookup inputs (sources, URI, text, table, config, i18n) from the request.
 * @param[in] ctx Hover query context.
 * @return Lookup context viewing request-owned data; valid while the request lives.
 */
[[nodiscard]] analysis::HoverDocLookupContext BuildDocLookupContext(const HoverQueryContext& ctx);

/**
 * @brief Hover for a primitive type keyword such as `int` (`auto` excluded).
 * @param[in] nodeText Hovered text.
 * @param[in] range Hover range.
 * @param[in] i18n Localizer for the type description.
 * @param[in,out] profiler Latency trace.
 * @return Primitive hover, or nullopt when the text is not a primitive.
 */
std::optional<lsp::Hover> TryHoverPrimitiveType(std::string_view nodeText, const lsp::Range& range,
                                                const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler);

/**
 * @brief Hover for the `?` wildcard type.
 * @param[in] nodeText Hovered text.
 * @param[in] range Hover range.
 * @param[in] i18n Localizer for the description.
 * @param[in,out] profiler Latency trace.
 * @return Wildcard hover, or nullopt when the text is not `?`.
 */
std::optional<lsp::Hover> TryHoverWildcardType(std::string_view nodeText, const lsp::Range& range,
                                               const angel_lsp::i18n::I18n* i18n, HoverProfiler& profiler);

/**
 * @brief Hover for `this`: the mixin host class, or the innermost enclosing class or interface.
 * @param[in] ctx Hover query context.
 * @return `(self)` hover, or nullopt outside a class.
 */
std::optional<lsp::Hover> TryHoverThis(const HoverQueryContext& ctx);

/**
 * @brief Hover for the `function` keyword of a lambda: its target funcdef, or its own parameter list.
 * @param[in] ctx Hover query context.
 * @return Lambda hover, or nullopt when the keyword is not inside a lambda.
 */
std::optional<lsp::Hover> TryHoverLambda(const HoverQueryContext& ctx);

/**
 * @brief Hover for `auto` or `auto@`, showing the type deduced from the first declarator.
 * @param[in] ctx Hover query context.
 * @return Deduced type hover, or nullopt when nothing could be deduced.
 */
std::optional<lsp::Hover> TryHoverAutoType(const HoverQueryContext& ctx);

/**
 * @brief Hover for a quoted string: a resolved asset path, or the literal with its length.
 * @param[in] ctx Hover query context.
 * @return String hover; nullopt for a whole `+` expression whose value does not resolve to a file.
 */
std::optional<lsp::Hover> TryHoverStringLiteral(const HoverQueryContext& ctx);

/**
 * @brief Hover for a string inside a constant `+` chain whose folded value resolves to a file.
 * @param[in] ctx Hover query context.
 * @param[in] showLength Whether the character count line is appended.
 * @return Asset hover, or nullopt when there is no chain or it does not resolve.
 */
std::optional<lsp::Hover> TryHoverConcatenatedString(const HoverQueryContext& ctx, bool showLength);

/**
 * @brief First hover stage: string literals, then primitive types, then the wildcard type.
 * @param[in] ctx Hover query context.
 * @return Literal or type hover, or nullopt to continue with symbol hovers.
 */
std::optional<lsp::Hover> TryHoverLiteralOrType(const HoverQueryContext& ctx);

/**
 * @brief Hover for `obj.member`: members of the receiver type, or its `get_` and `set_` accessors.
 * @param[in] ctx Hover query context.
 * @return Member hover, or nullopt when the receiver type or member is unknown.
 */
std::optional<lsp::Hover> TryHoverMemberAccess(const HoverQueryContext& ctx);

/**
 * @brief Hover for a local variable or parameter found in the scope tree.
 * @param[in] ctx Hover query context.
 * @return Local hover, or nullopt when no local or parameter matches.
 */
std::optional<lsp::Hover> TryHoverLocalDefinition(const HoverQueryContext& ctx);

/**
 * @brief Hover for a parameter name, showing its declaration text with whitespace collapsed.
 * @param[in] ctx Hover query context.
 * @return Parameter hover, or nullopt when the target is not the name field of a parameter.
 */
std::optional<lsp::Hover> TryHoverParameterAst(const HoverQueryContext& ctx);

/**
 * @brief Hover for a named argument label (`f(name: value)`), showing the matching callee parameter.
 * @param[in] ctx Hover query context.
 * @return Parameter hover, or nullopt when no callee overload has that parameter.
 */
std::optional<lsp::Hover> TryHoverNamedArgument(const HoverQueryContext& ctx);

/**
 * @brief Second hover stage: `auto`, member access, locals, parameter names, then named arguments.
 * @param[in] ctx Hover query context.
 * @return First matching hover, or nullopt to continue with symbol candidates.
 */
std::optional<lsp::Hover> TryHoverExpressionOrLocal(const HoverQueryContext& ctx);

/**
 * @brief Picks the overload to document: the resolved call overload, else one declared on or around this line.
 * @param[in,out] symbols Candidates; the chosen one is rotated to the front.
 * @param[in] ctx Hover query context.
 * @return Chosen candidate, or nullopt for an empty list.
 */
std::optional<analysis::Symbol> ResolveOrMatchCandidate(std::vector<analysis::Symbol>& symbols,
                                                        const HoverQueryContext& ctx);

/**
 * @brief Moves the candidate with the same name and parameters as `best` to the front after deduplication.
 * @param[in,out] symbols Deduplicated candidates.
 * @param[in,out] best Chosen candidate, replaced by the front entry when found.
 */
void PromoteMatchedCandidate(std::vector<analysis::Symbol>& symbols, std::optional<analysis::Symbol>& best);

/**
 * @brief In a type position, keeps class symbols over same-named functions or namespaces.
 * @param[in] ctx Hover query context.
 * @param[in,out] symbols Candidates to narrow.
 */
void DisambiguateTypeContextSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols);

/**
 * @brief Chooses between a class and a namespace of the same name using the scoped member that follows.
 * @param[in] ctx Hover query context.
 * @param[in,out] symbols Candidates to narrow.
 */
void DisambiguateContainerSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols);

/**
 * @brief Inside a call, replaces class candidates with their constructors (the resolved one first).
 * @param[in] ctx Hover query context.
 * @param[in,out] symbols Candidates to extend or replace.
 */
void AppendConstructorSymbolsIfCall(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols);

/**
 * @brief For `::name`, keeps only symbols declared at global scope.
 * @param[in,out] symbols Candidates to narrow.
 * @param[in] isDirectRootGlobal True when the target is a direct child of a leading `::`.
 */
void FilterDirectRootGlobalSymbols(std::vector<analysis::Symbol>& symbols, bool isDirectRootGlobal);

/**
 * @brief Last-resort hover from a scope-tree definition when the symbol table has no match.
 * @param[in] ctx Hover query context.
 * @return Definition hover, or nullopt when the scope tree has none.
 */
std::optional<lsp::Hover> TryHoverScopeFallback(const HoverQueryContext& ctx);

/**
 * @brief Builds the symbol hover: declaration, more-overloads link, docs, mixin members and file footer.
 * @param[in,out] symbols Candidates, deduplicated and reordered in place.
 * @param[in] accessorPropertyType Property type when the candidates are accessors, else empty.
 * @param[in] ctx Hover query context.
 * @return Markdown hover for the front candidate.
 */
lsp::Hover FormatSymbolsHover(std::vector<analysis::Symbol>& symbols, std::string_view accessorPropertyType,
                              const HoverQueryContext& ctx);

/**
 * @brief Final hover stage: symbol table lookup with class, mixin-host, accessor and delegate fallbacks.
 * @param[in,out] ctx Hover query context; its profiler is updated.
 * @return Symbol hover, or nullopt when nothing matches.
 */
std::optional<lsp::Hover> TryHoverSymbolCandidates(HoverQueryContext& ctx);
} // namespace angel_lsp::features
