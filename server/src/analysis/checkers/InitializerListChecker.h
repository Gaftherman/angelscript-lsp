#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/InitializerListFacts.h"
#include "analysis/index/ScopeTree.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Reports initializer lists applied to a type that cannot be built from one.
 * @param[in] request Tree and source text of the document under analysis.
 * @param[in,out] ctx Diagnostic sink; also carries the TypeConfig naming the array template.
 */
void CheckInitializerLists(const AstCheckContext& request, DiagnosticContext& ctx);

/**
 * @brief Evaluates pre-extracted initializer list facts and reports invalid lists.
 * @param[in] facts Precomputed initializer list facts for the document revision.
 * @param[in] sourceCode Source text of the document under analysis.
 * @param[in] scopeRoot Root of the lexical scope tree, or nullptr.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
void CheckInitializerLists(const InitializerListFacts& facts, std::string_view sourceCode, const Scope* scopeRoot,
                           DiagnosticContext& ctx);

/**
 * @brief AST checker for initializer list nesting and element type compatibility.
 */
struct InitializerListChecker
{
    static constexpr std::string_view Name = "InitializerListChecker";
    static bool IsEnabled([[maybe_unused]] const SemanticAnalysisRequest& request)
    {
        return true;
    }
    static void Run(const AstCheckContext& ast, DiagnosticContext& ctx)
    {
        CheckInitializerLists(ast, ctx);
    }
};

/**
 * @brief Context for inspecting elements in an initializer list.
 */
struct ElementContext
{
    /**
     * @brief Source text of the document.
     */
    std::string_view sourceCode;
    /**
     * @brief Scope for looking up named identifiers in element expressions.
     */
    const Scope* scopeRoot = nullptr;
};

/**
 * @brief Request parameters for checking an initializer list against a known target type.
 */
struct InitializerListTargetRequest
{
    const InitializerListFact& list;
    const InitializerListFacts& facts;
    std::string_view targetType;
    ElementContext elements{};
};

/**
 * @brief Checks one initializer list fact against a target type using a structured request.
 * @param[in] request Target type, element scope, and initializer list facts.
 * @param[in,out] ctx Diagnostic sink.
 */
void CheckInitializerListAgainstType(const InitializerListTargetRequest& request, DiagnosticContext& ctx);

/**
 * @brief Checks one initializer list fact against a target type using default element scope.
 * @param[in] list Initializer list fact to check.
 * @param[in] facts Enclosing facts of the document.
 * @param[in] targetType Target type name.
 * @param[in,out] ctx Diagnostic sink.
 */
void CheckInitializerListAgainstType(const InitializerListFact& list, const InitializerListFacts& facts,
                                     std::string_view targetType, DiagnosticContext& ctx);

/**
 * @brief Validates an initializer list fact recursively against an expected target type.
 * @param[in] list Initializer list fact.
 * @param[in] facts Enclosing document facts.
 * @param[in] expectedType The expected target type name.
 * @param[in,out] ctx Diagnostic context.
 */
void ValidateInitializerList(const InitializerListFact& list, const InitializerListFacts& facts,
                             std::string_view expectedType, DiagnosticContext& ctx);

/**
 * @brief Canonical and instantiated type name spellings for template type arguments.
 */
struct TemplateSpelling
{
    std::string name;
    std::vector<std::string> arguments;
};

/**
 * @brief Container inspection state for initializer list target types (arrays, dictionaries, or aggregate structs).
 */
struct ListContainerInfo
{
    bool isContainer = false;
    uint32_t dimensions = 1;
    std::string elementType;
};

/**
 * @brief Inspection results for member functions and operators supported by a list container type.
 */
struct ContainerMemberInspection
{
    uint32_t maxIndexParams = 0;
    std::string indexReturnType;
    bool hasListConstructor = false;
};

/**
 * @brief Extracted key and value type descriptors for dictionary initializer list validation.
 */
struct DictInfo
{
    bool isDict = false;
    std::string keyType;
    std::string valType;
};

/**
 * @brief Evaluation state for dictionary key-value pair types during initializer list type checking.
 */
struct DictTypeState
{
    std::string keyType;
    std::string valType;
    bool isAssociative = false;
};

/**
 * @brief Field metadata describing member name, declared type, and offset for aggregate struct initialization.
 */
struct StructFieldInfo
{
    std::string name;
    std::string type;
    uint32_t line = 0;
    uint32_t col = 0;
};

/**
 * @brief Extracted field list and initialization layout for aggregate struct initialization.
 */
struct AggregateStructInfo
{
    bool hasListSupport = false;
    std::vector<StructFieldInfo> fields;
};

using StructLayoutCache = ankerl::unordered_dense::map<std::string, AggregateStructInfo>;

/**
 * @brief Encapsulates initializer list fact context, expected target type, and active diagnostic sinks.
 */
struct ListValidationContext
{
    DiagnosticContext& ctx;
    const ElementContext& elements;
    StructLayoutCache& cache;
    const InitializerListFacts& facts;
    int depth = 0;
};

/**
 * @brief Analysis engine for recursive validation of initializer list contents against expected types.
 */
struct InitializerListValidator
{
    /**
     * @brief Validates an initializer list fact against target composite, container, or struct type.
     * @param[in] list Initializer list fact.
     * @param[in] targetType Target type name view.
     * @param[in] valCtx Active list validation context.
     */
    static void ValidateList(const InitializerListFact& list, std::string_view targetType,
                             const ListValidationContext& valCtx);
};

} // namespace angel_lsp::analysis
