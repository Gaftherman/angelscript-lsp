#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Document text access and include path resolution callbacks for cross-file hover documentation.
 */
struct HoverDocumentSources
{
    /**
     * @brief Reads a document's full text by URI, or returns nullptr when the workspace does not hold it.
     */
    std::function<const std::string*(std::string_view uri)> readDocument;

    /**
     * @brief Resolves the target text inside an `#include` directive to an absolute file path.
     */
    std::function<std::string(std::string_view rawPath)> resolveInclude;
};

/**
 * @brief Contextual data and resolver callbacks for retrieving symbol documentation comments.
 */
struct HoverDocLookupContext
{
    /**
     * @brief Optional source access callbacks for cross-file documentation inspection.
     */
    const HoverDocumentSources* sources = nullptr;

    /**
     * @brief Document URI of the active document being queried.
     */
    std::string_view documentUri;

    /**
     * @brief Full source code of the active document.
     */
    std::string_view sourceCode;

    /**
     * @brief Workspace symbol table for cross-referencing types and companion members.
     */
    const SymbolTable* symbolTable = nullptr;

    /**
     * @brief Optional server configuration controlling engine rules and proxies.
     */
    const config::ServerConfig* config = nullptr;

    /**
     * @brief Optional localization dictionary for formatted documentation strings.
     */
    const angel_lsp::i18n::I18n* i18n = nullptr;
};

/**
 * @brief Context parameters for formatting VS Code peek-overloads command links.
 */
struct HoverPeekContext
{
    /**
     * @brief Document URI where the hover was requested.
     */
    std::string_view uri;

    /**
     * @brief 0-indexed line number of the hover query position.
     */
    uint32_t line = 0;

    /**
     * @brief 0-indexed character offset of the hover query position.
     */
    uint32_t character = 0;

    /**
     * @brief Optional localization dictionary pointer.
     */
    const angel_lsp::i18n::I18n* i18n = nullptr;
};

/**
 * @brief Squeezes runs of whitespace into single spaces so a declaration appears on one line.
 * @param[in] text Input string view.
 * @return Collapsed string with single spaces and trimmed margins.
 */
std::string CollapseWhitespace(std::string_view text);

/**
 * @brief Renders the single hover line describing a symbol of any kind.
 * @param[in] sym Target symbol descriptor.
 * @param[in] symbolTable Optional workspace symbol table for container resolution.
 * @return Formatted declaration string with metadata prefixes.
 */
std::string FormatDeclarationText(const Symbol& sym, const SymbolTable* symbolTable = nullptr);

/**
 * @brief Renders the hover line for a variable-like symbol, tagged with its role prefix.
 * @param[in] sym Symbol descriptor.
 * @param[in] role Prefix shown to user, e.g. "(property) ".
 * @return Formatted variable declaration string.
 */
std::string FormatVariableSignature(const Symbol& sym, const char* role);

/**
 * @brief Formats the signature declaration of a function or method symbol.
 * @param[in] sym Function or method symbol descriptor.
 * @return Formatted function signature string.
 */
std::string FormatFunctionSignature(const Symbol& sym);

/**
 * @brief Formats the signature declaration of a class, interface, or struct symbol.
 * @param[in] sym Type symbol descriptor.
 * @return Formatted type signature string.
 */
std::string FormatClassSignature(const Symbol& sym);

/**
 * @brief Formats the namespace declaration string for a namespace symbol.
 * @param[in] sym Namespace symbol descriptor.
 * @return Formatted namespace declaration string.
 */
std::string FormatNamespaceDeclaration(const Symbol& sym);

/**
 * @brief Drops duplicate symbol declarations from the candidate collection.
 * @param[in,out] symbols Candidate symbol vector to deduplicate.
 */
void RemoveDuplicateSymbols(std::vector<Symbol>& symbols);

/**
 * @brief Formats synthesized property header line if property accessor.
 * @param[in] symbols Candidate symbol list.
 * @param[in] accessorPropertyType Synthesized property accessor type.
 * @param[in] nodeText Syntax node text.
 * @return Formatted property header line, or empty string.
 */
std::string FormatSynthesizedPropertyHeader(const std::vector<Symbol>& symbols, std::string_view accessorPropertyType,
                                            std::string_view nodeText);

/**
 * @brief Appends file location footer to hover markdown when configured and available.
 * @param[in] symbols Candidate symbols.
 * @param[in] config Optional server configuration controlling location visibility.
 * @param[in,out] oss Markdown stream to append to.
 */
void AppendFileLocationFooter(const std::vector<Symbol>& symbols, const config::ServerConfig* config,
                              std::ostringstream& oss);

/**
 * @brief Formats parameter hover text from an AST LocalDefinition.
 * @param[in] def Local definition descriptor.
 * @param[in] typeName Deduced type name string view.
 * @param[in] symbolTable Symbol table for funcdef signature lookup.
 * @param[in,out] md Destination markdown string.
 */
void FormatParameterHover(const LocalDefinition& def, std::string_view typeName, const SymbolTable& symbolTable,
                          std::string& md);

/**
 * @brief Formats named argument parameter hover markdown from candidate functions.
 * @param[in] candidates Callee candidate symbols.
 * @param[in] nodeText Target parameter identifier name.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown string, or empty string if no parameter matched.
 */
std::string FormatNamedArgHover(const std::vector<Symbol>& candidates, std::string_view nodeText,
                                const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Formats variable hover text for a local definition resolved as a global symbol.
 * @param[in] def Local definition descriptor.
 * @param[in] typeName Deduced type name string view.
 * @param[in] globalSym Optional pointer to matching global symbol in symbol table.
 * @param[in,out] md Destination markdown string.
 */
void FormatGlobalVariableHover(const LocalDefinition& def, std::string_view typeName, const Symbol* globalSym,
                               std::string& md);

/**
 * @brief Formats primitive type documentation including localized description, values or range.
 * @param[in] typeName Primitive type name.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown string, or empty string if not a documented primitive.
 */
std::string FormatPrimitiveDoc(std::string_view typeName, const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Formats raw byte count into human-readable B, KB, or MB units.
 * @param[in] bytes File size in bytes.
 * @return Formatted size string.
 */
std::string FormatFileSize(uintmax_t bytes);

/**
 * @brief Formats asset metadata block including status, size, and normalized path.
 * @param[in] unquoted Unquoted string literal text.
 * @param[in] resolved Resolved absolute path to the asset file.
 * @param[in] showLength Whether to append character count.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown string.
 */
std::string FormatResolvedAssetMarkdown(std::string_view unquoted, std::string_view resolved, bool showLength,
                                        const angel_lsp::i18n::I18n* i18n = nullptr);

/**
 * @brief Retrieves doc comment specifically from the declaring file of a symbol.
 * @param[in] ctx Document lookup context.
 * @param[in] symbol Symbol descriptor.
 * @return Doc comment string, or empty if none found.
 */
std::string OwnDocComment(const HoverDocLookupContext& ctx, const Symbol& symbol);

/**
 * @brief Retrieves doc comment from a companion property or getter/setter.
 * @param[in] ctx Document lookup context.
 * @param[in] symbol Symbol being hovered.
 * @return Companion doc comment string, or empty if none found.
 */
std::string CompanionDocComment(const HoverDocLookupContext& ctx, const Symbol& symbol);

/**
 * @brief Retrieves doc comment from an overridden method in ancestor types.
 * @param[in] ctx Document lookup context.
 * @param[in] symbol Symbol being hovered.
 * @return Inherited doc comment string, or empty if none found.
 */
std::string InheritedDocComment(const HoverDocLookupContext& ctx, const Symbol& symbol);

/**
 * @brief Retrieves the doc comment attached to a symbol declaration, inspecting own, companion, or ancestor files.
 * @param[in] ctx Document lookup context.
 * @param[in] symbol Symbol being hovered.
 * @return Extracted doc comment string, or empty if none found.
 */
std::string DocCommentForSymbol(const HoverDocLookupContext& ctx, const Symbol& symbol);

/**
 * @brief Appends underlying primitive type description and range for typedef symbols.
 * @param[in] sym Candidate symbol.
 * @param[in] symbolTable Symbol table for typedef resolution.
 * @param[in] i18n Optional localization dictionary pointer.
 * @param[out] docs Accumulated documentation blocks.
 */
void AppendTypedefDoc(const Symbol& sym, const SymbolTable& symbolTable, const angel_lsp::i18n::I18n* i18n,
                      std::vector<std::string>& docs);

/**
 * @brief Gathers and deduplicates doc comments for overload sets, prioritizing resolved call overloads.
 * @param[in] ctx Document lookup context.
 * @param[in] symbols Deduplicated candidate symbols list.
 * @param[in] best Optional best-matched call overload symbol.
 * @param[out] docs Output vector receiving collected markdown doc blocks.
 */
void CollectOverloadDocs(const HoverDocLookupContext& ctx, const std::vector<Symbol>& symbols,
                         const std::optional<Symbol>& best, std::vector<std::string>& docs);

/**
 * @brief Appends funcdef signature declaration to markdown hover text if typeName is a funcdef.
 * @param[in] typeName Base or qualified type name.
 * @param[in] table Symbol table.
 * @param[in,out] md Destination markdown string.
 */
void TryAppendFuncdefSignature(std::string_view typeName, const SymbolTable& table, std::string& md);

/**
 * @brief Appends mixin members section if the symbol represents a mixin class.
 * @param[in] sym Symbol descriptor.
 * @param[in] symbolTable Symbol table with registered rule index.
 * @param[in] i18n Optional localization dictionary pointer.
 * @param[in,out] oss Output stream accumulating markdown text.
 */
void AppendMixinMembersSection(const Symbol& sym, const SymbolTable& symbolTable, const angel_lsp::i18n::I18n* i18n,
                               std::ostringstream& oss);

/**
 * @brief Checks whether a symbol represents a callable routine (function, method, constructor).
 * @param[in] sym Symbol to test.
 * @return True if callable routine, false otherwise.
 */
bool IsCallableSymbol(const Symbol& sym);

/**
 * @brief Counts additional callable overloads sharing the primary symbol's name.
 * @param[in] symbols Deduplicated candidate symbols list.
 * @return Count of other overloads with the same name.
 */
size_t CountMoreOverloads(const std::vector<Symbol>& symbols);

/**
 * @brief Percent-encodes a JSON string for use inside a VS Code command URI query parameter.
 * @param[in] input Raw JSON text.
 * @return Percent-encoded string safe for Markdown command links.
 */
std::string UrlEncodeCommandArgs(std::string_view input);

/**
 * @brief Builds a clickable command link that opens VS Code Peek Locations for all overloads.
 * @param[in] symbols Candidate overload symbols.
 * @param[in] context Context parameters for the peek link.
 * @return Markdown link invoking angelscript.peekOverloads.
 */
std::string BuildPeekOverloadsLink(const std::vector<Symbol>& symbols, const HoverPeekContext& context);

/**
 * @brief Formats the localized overload section and peek command link for additional overloads.
 * @param[in] symbols Deduplicated candidate symbols list.
 * @param[in] count Number of additional overloads (must be >= 1).
 * @param[in] context Context parameters for the peek link.
 * @return Localized overload block and clickable peek link.
 */
std::string FormatMoreOverloads(const std::vector<Symbol>& symbols, size_t count, const HoverPeekContext& context);

/**
 * @brief Formats markdown documentation block for an include directive.
 * @param[in] rawPath Include path text inside delimiters.
 * @param[in] sources Document sources containing include resolution callback.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown block.
 */
std::string FormatIncludeDirectiveHover(std::string_view rawPath, const HoverDocumentSources& sources,
                                        const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Formats primitive type hover block with description and values.
 * @param[in] nodeText Primitive type keyword.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown block.
 */
std::string FormatPrimitiveTypeHover(std::string_view nodeText, const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Formats wildcard type '?' hover documentation block.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown block.
 */
std::string FormatWildcardTypeHover(const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Formats 'this' pointer hover tooltip with enclosing host class name.
 * @param[in] className Enclosing host class name.
 * @param[in] nodeText Syntax node text ('this').
 * @return Formatted markdown block.
 */
std::string FormatThisHover(std::string_view className, std::string_view nodeText);

/**
 * @brief Formats string literal hover markdown with optional character length.
 * @param[in] txt Raw string literal token text.
 * @param[in] unquoted Unquoted string literal text.
 * @param[in] showLength Whether to append length.
 * @param[in] i18n Optional localization dictionary pointer.
 * @return Formatted markdown block.
 */
std::string FormatStringLiteralHover(std::string_view txt, std::string_view unquoted, bool showLength,
                                     const angel_lsp::i18n::I18n* i18n);

/**
 * @brief Formats parameter declaration markdown when recovered directly from AST.
 * @param[in] rawDecl Trimmed parameter declaration text.
 * @return Formatted markdown block.
 */
std::string FormatParameterAstHover(std::string_view rawDecl);

/**
 * @brief Formats scope fallback hover markdown for local definitions without full symbols.
 * @param[in] def Local definition descriptor.
 * @param[in] sourceCode Document source text for doc comment extraction.
 * @return Formatted markdown block.
 */
std::string FormatScopeFallbackHover(const LocalDefinition& def, std::string_view sourceCode);

/**
 * @brief Context parameters for formatting a local definition hover tooltip.
 */
struct LocalDefinitionHoverQuery
{
    const LocalDefinition& def;       ///< Target local definition record.
    const Scope* scope = nullptr;     ///< Starting lexical scope.
    const Scope* rootScope = nullptr; ///< Document root scope.
    IndexedByteRange hoverNode{};     ///< Byte range of hovered syntax token.
    bool isDirectInit = false;        ///< True if declared with direct-initialization argument list.
};

/**
 * @brief Formats local definition hover tooltip with type deduction, constructors, and doc comments.
 * @param[in] query Local definition hover query parameters.
 * @param[in] exprCtx Semantic expression context for constructor and type deduction.
 * @param[in] docCtx Document lookup context for doc comments.
 * @return Formatted markdown block.
 */
std::string FormatLocalDefinitionHover(const LocalDefinitionHoverQuery& query, const ExpressionTypeContext& exprCtx,
                                       const HoverDocLookupContext& docCtx);

/**
 * @brief Formats symbol candidates markdown including signature, property header, overloads, and doc comments.
 * @param[in] symbols Candidate symbol collection.
 * @param[in] best Best matching overload symbol, if matched.
 * @param[in] accessorPropertyType Synthesized accessor property type, if any.
 * @param[in] nodeText Syntax node text.
 * @param[in] docCtx Document lookup context.
 * @param[in] peekCtx Context for VS Code peek-overloads links.
 * @return Formatted hover markdown string.
 */
std::string FormatSymbolsMarkdown(const std::vector<Symbol>& symbols, const std::optional<Symbol>& best,
                                  std::string_view accessorPropertyType, std::string_view nodeText,
                                  const HoverDocLookupContext& docCtx, const HoverPeekContext& peekCtx);

/**
 * @brief Resolves an unquoted string literal to an absolute asset file path if one exists.
 * @param[in] unquoted Unquoted string literal text.
 * @param[in] uri Document URI for relative path resolution.
 * @param[in] sources Document sources containing include resolution callback.
 * @param[in] config Optional server configuration supplying asset search directories.
 * @return Normalized absolute path if file exists, or empty string.
 */
std::string ResolveStringLiteralPath(std::string_view unquoted, std::string_view uri,
                                     const HoverDocumentSources& sources, const config::ServerConfig* config);

/**
 * @brief Tells whether string text looks like a file path: it has a slash, or a 2-4 character extension.
 * @param[in] str Unquoted string text.
 * @return True for path-like text such as `models/a.mdl` or `a.wav`.
 */
[[nodiscard]] bool IsPathLikeString(std::string_view str);

/**
 * @brief Builds the asset hover for path-like string text that resolves to an existing file.
 * @param[in] path Unquoted string text or the folded value of a constant concatenation.
 * @param[in] showLength Whether the character count line is appended.
 * @param[in] docCtx Document URI, include sources, server configuration and i18n.
 * @return Asset markdown; empty when the text is not path-like, does not resolve, or `docCtx.sources` is null.
 */
[[nodiscard]] std::string FormatStringPathHover(std::string_view path, bool showLength,
                                                const HoverDocLookupContext& docCtx);

/**
 * @brief Folds a constant string `+` chain and builds its asset hover when the result is a resolvable path.
 * @param[in] plusRange Byte range of the topmost `+` binary expression.
 * @param[in] exprCtx Expression context (with root) whose scope resolves identifier operands.
 * @param[in] showLength Whether the character count line is appended.
 * @param[in] docCtx Document URI, include sources, server configuration and i18n.
 * @return Asset markdown; empty when the chain does not fold to a constant or the path does not resolve.
 */
[[nodiscard]] std::string FormatConcatenatedPathHover(IndexedByteRange plusRange, const ExpressionTypeContext& exprCtx,
                                                      bool showLength, const HoverDocLookupContext& docCtx);
} // namespace angel_lsp::analysis
