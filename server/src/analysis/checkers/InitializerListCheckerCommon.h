#pragma once

#include "analysis/checkers/InitializerListChecker.h"
#include "analysis/index/SymbolTable.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Strips type decorations (const, &, @, whitespace) that do not affect initializer list acceptance.
 * @param[in] raw Unsanitized type name string.
 * @return Compact type string without qualifiers or surrounding whitespace.
 */
std::string StripDecorations(std::string_view raw);

/**
 * @brief Decomposes a templated type name into its base identifier and argument list.
 * @param[in] type Type name string possibly containing template arguments.
 * @return TemplateSpelling structure with base name and argument list.
 */
TemplateSpelling ReadTemplateSpelling(const std::string& type);

/**
 * @brief Builds an ordered list of candidate symbol lookup keys for container inspection.
 * @param[in] sym Target symbol to inspect.
 * @param[in] fallbackName Fallback type name if symbol name is empty or distinct.
 * @return Vector of candidate lookup keys.
 */
std::vector<std::string> CandidateContainerKeys(const Symbol& sym, std::string_view fallbackName);

/**
 * @brief Counts the number of values written in an initializer list, including omitted elements.
 * @param[in] listNode Initializer list AST node.
 * @return Total value count represented by the list.
 */
uint32_t ListValueCount(TSNode listNode);

/**
 * @brief Validates an element expression against an expected type and emits conversion diagnostics.
 * @param[in] element AST node of the element expression.
 * @param[in] wanted Target expected type name.
 * @param[in,out] ctx Diagnostic context.
 * @param[in] elements Scope and source text context.
 */
void CheckElementValue(TSNode element, const std::string& wanted, DiagnosticContext& ctx,
                       const ElementContext& elements);

/**
 * @brief Inspects whether a type denotes a sequence or multi-dimensional container.
 * @param[in] type Cleaned type string.
 * @param[in] spelling Parsed template spelling.
 * @param[in] ctx Diagnostic context.
 * @return Resolved container dimensionality and element type.
 */
ListContainerInfo InspectContainer(const std::string& type, const TemplateSpelling& spelling,
                                   const DiagnosticContext& ctx);

/**
 * @brief Recursively validates elements of a multi-dimensional or sequence container.
 * @param[in] node Initializer list AST node.
 * @param[in] dimension Remaining nesting dimension.
 * @param[in] elemType Expected scalar or sub-container element type.
 * @param[in] valCtx Active list validation context.
 */
void ValidateMultiDimensionalContainer(TSNode node, uint32_t dimension, const std::string& elemType,
                                       const ListValidationContext& valCtx);

/**
 * @brief Inspects whether a type denotes an associative container and extracts key and value types.
 * @param[in] spelling Parsed template spelling of the candidate container.
 * @param[in] ctx Diagnostic context.
 * @return Resolved dictionary key and value type descriptors.
 */
DictInfo InspectDictionary(const TemplateSpelling& spelling, const DiagnosticContext& ctx);

/**
 * @brief Validates an initializer list against expected dictionary key-value semantics.
 * @param[in] listNode Initializer list AST node.
 * @param[in] dictInfo Dictionary key and value type descriptors.
 * @param[in] valCtx Active list validation context.
 */
void ValidateDictionaryList(TSNode listNode, const DictInfo& dictInfo, const ListValidationContext& valCtx);

/**
 * @brief Extracts field layout and initializer list support for an aggregate struct or class.
 * @param[in] spelling Parsed template spelling of the struct.
 * @param[in] ctx Diagnostic context.
 * @param[in,out] cache Struct layout cache across inspections.
 * @return Aggregate struct field layout and list initialization support.
 */
AggregateStructInfo InspectAggregateStruct(const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                                           StructLayoutCache& cache);

/**
 * @brief Validates an initializer list against an aggregate struct or class field layout.
 * @param[in] listNode Initializer list AST node.
 * @param[in] type Decorated or unadorned struct type name.
 * @param[in] structInfo Struct layout information.
 * @param[in] valCtx Active list validation context.
 */
void ValidateStructAggregateList(TSNode listNode, const std::string& type, const AggregateStructInfo& structInfo,
                                 const ListValidationContext& valCtx);

} // namespace angel_lsp::analysis
