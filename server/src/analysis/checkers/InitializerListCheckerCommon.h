#pragma once

#include "analysis/checkers/InitializerListChecker.h"
#include "analysis/index/InitializerListFacts.h"
#include "analysis/index/SymbolTable.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Strips type decorations (const, &, @, whitespace) that do not affect initializer list acceptance.
 * @param[in] raw Unsanitized type name string view.
 * @return Compact type string without qualifiers or surrounding whitespace.
 */
std::string StripDecorations(std::string_view raw);

/**
 * @brief Decomposes a templated type name into its base identifier and argument list.
 * @param[in] type Type name string view possibly containing template arguments.
 * @return TemplateSpelling structure with base name and argument list.
 */
TemplateSpelling ReadTemplateSpelling(std::string_view type);

/**
 * @brief Builds an ordered list of candidate symbol lookup keys for container inspection.
 * @param[in] sym Target symbol to inspect.
 * @param[in] fallbackName Fallback type name if symbol name is empty or distinct.
 * @return Vector of candidate lookup keys.
 */
std::vector<std::string> CandidateContainerKeys(const Symbol& sym, std::string_view fallbackName);

/**
 * @brief Returns the total value count represented by an initializer list fact.
 * @param[in] list Initializer list fact.
 * @return Precomputed value count written in the list.
 */
inline uint32_t ListValueCount(const InitializerListFact& list) noexcept
{
    return list.valueCount;
}

/**
 * @brief Validates an element expression against an expected type and emits conversion diagnostics.
 * @param[in] element Element fact containing byte span and source range.
 * @param[in] wanted Target expected type name.
 * @param[in,out] ctx Diagnostic context.
 * @param[in] elements Scope and source text context.
 */
void CheckElementValue(const InitializerListElementFact& element, std::string_view wanted, DiagnosticContext& ctx,
                       const ElementContext& elements);

/**
 * @brief Inspects whether a type denotes a sequence or multi-dimensional container.
 * @param[in] type Cleaned type string view.
 * @param[in] spelling Parsed template spelling.
 * @param[in] ctx Diagnostic context.
 * @return Resolved container dimensionality and element type.
 */
ListContainerInfo InspectContainer(std::string_view type, const TemplateSpelling& spelling,
                                   const DiagnosticContext& ctx);

/**
 * @brief Recursively validates elements of a multi-dimensional or sequence container.
 * @param[in] list Initializer list fact.
 * @param[in] dimension Remaining nesting dimension.
 * @param[in] elemType Expected scalar or sub-container element type.
 * @param[in] valCtx Active list validation context.
 */
void ValidateMultiDimensionalContainer(const InitializerListFact& list, uint32_t dimension, std::string_view elemType,
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
 * @param[in] list Initializer list fact.
 * @param[in] dictInfo Dictionary key and value type descriptors.
 * @param[in] valCtx Active list validation context.
 */
void ValidateDictionaryList(const InitializerListFact& list, const DictInfo& dictInfo,
                            const ListValidationContext& valCtx);

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
 * @param[in] list Initializer list fact.
 * @param[in] type Decorated or unadorned struct type name view.
 * @param[in] structInfo Struct layout information.
 * @param[in] valCtx Active list validation context.
 */
void ValidateStructAggregateList(const InitializerListFact& list, std::string_view type,
                                 const AggregateStructInfo& structInfo, const ListValidationContext& valCtx);

} // namespace angel_lsp::analysis
