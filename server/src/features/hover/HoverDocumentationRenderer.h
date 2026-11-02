#pragma once

#include "features/hover/HoverHandler.h"
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Retrieves the doc comment attached to a symbol declaration, inspecting own, companion, or ancestor files.
 * @param[in] request Hover request context.
 * @param[in] symbol Symbol being hovered.
 * @return Extracted doc comment string, or empty if none found.
 */
std::string DocCommentForSymbol(const HoverRequest& request, const analysis::Symbol& symbol);

/**
 * @brief Retrieves doc comment specifically from the declaring file of a symbol.
 * @param[in] request Hover request context.
 * @param[in] symbol Symbol descriptor.
 * @return Doc comment string, or empty if none found.
 */
std::string OwnDocComment(const HoverRequest& request, const analysis::Symbol& symbol);

/**
 * @brief Gathers and deduplicates doc comments for overload sets, prioritizing resolved call overloads.
 * @param[in] request Hover request context.
 * @param[in] symbols Deduplicated candidate symbols list.
 * @param[in] best Optional best-matched call overload symbol.
 * @param[out] docs Output vector receiving collected markdown doc blocks.
 */
void CollectOverloadDocs(const HoverRequest& request, const std::vector<analysis::Symbol>& symbols,
                         const std::optional<analysis::Symbol>& best, std::vector<std::string>& docs);

/**
 * @brief Appends underlying primitive type description and range for typedef symbols.
 * @param[in] request Hover request context.
 * @param[in] sym Candidate symbol.
 * @param[out] docs Accumulated documentation blocks.
 */
void AppendTypedefDoc(const HoverRequest& request, const analysis::Symbol& sym, std::vector<std::string>& docs);

/**
 * @brief Appends doc comments for local definition or its constructor/funcdef type.
 * @param[in] def Local definition descriptor.
 * @param[in] typeName Deduced type name string view.
 * @param[in] ctx Hover query context.
 * @param[in,out] md Output markdown string.
 */
void AppendLocalDoc(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                    std::string& md);

/**
 * @brief Appends funcdef signature declaration to markdown hover text if typeName is a funcdef.
 * @param[in] typeName Base or qualified type name.
 * @param[in] table Symbol table.
 * @param[in,out] md Destination markdown string.
 */
void TryAppendFuncdefSignature(std::string_view typeName, const analysis::SymbolTable& table, std::string& md);

/**
 * @brief Appends mixin members section if the symbol represents a mixin class.
 * @param[in] sym Symbol descriptor.
 * @param[in] ctx Hover query context.
 * @param[in,out] oss Output stream accumulating markdown text.
 */
void AppendMixinMembersSection(const analysis::Symbol& sym, const HoverQueryContext& ctx, std::ostringstream& oss);

/**
 * @brief Checks whether a symbol represents a callable routine (function, method, constructor).
 * @param[in] sym Symbol to test.
 * @return True if callable routine, false otherwise.
 */
bool IsCallableSymbol(const analysis::Symbol& sym);

/**
 * @brief Counts additional callable overloads sharing the primary symbol's name.
 * @param[in] symbols Deduplicated candidate symbols list.
 * @return Count of other overloads with the same name.
 */
size_t CountMoreOverloads(const std::vector<analysis::Symbol>& symbols);

/**
 * @brief Formats the localized overload section and peek command link for additional overloads.
 * @param[in] symbols Deduplicated candidate symbols list.
 * @param[in] count Number of additional overloads (must be >= 1).
 * @param[in] ctx Hover query context.
 * @return Localized overload block and clickable peek link.
 */
std::string FormatMoreOverloads(const std::vector<analysis::Symbol>& symbols, size_t count,
                                const HoverQueryContext& ctx);

/**
 * @brief Builds a clickable command link that opens VS Code Peek Locations for all overloads.
 * @param[in] symbols Candidate overload symbols.
 * @param[in] ctx Hover query context.
 * @return Markdown link invoking angelscript.peekOverloads.
 */
std::string BuildPeekOverloadsLink(const std::vector<analysis::Symbol>& symbols, const HoverQueryContext& ctx);

/**
 * @brief Percent-encodes a JSON string for use inside a VS Code command URI query parameter.
 * @param[in] input Raw JSON text.
 * @return Percent-encoded string safe for Markdown command links.
 */
std::string UrlEncodeCommandArgs(std::string_view input);
} // namespace angel_lsp::features
