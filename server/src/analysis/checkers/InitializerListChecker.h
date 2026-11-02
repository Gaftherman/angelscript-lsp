#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"

#include <ankerl/unordered_dense.h>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <unordered_set>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Checks that every initializer list is written against a type that accepts one.
 *
 * `{ ... }` is not a general-purpose initializer in AngelScript. It is only valid where the
 * target type registered a list factory, and the primitives never do - so `int x = {5};` and,
 * less obviously, the stray brace in `array<int> a = {1, 2, {3}, 4};` are both errors. The
 * compiler answers each with `Initialization lists cannot be used with 'int'`, because it
 * matches the list against the element pattern and the element there is a plain `int`.
 *
 * This checker validates initializer list nesting and element compatibility
 * against target types per the AngelScript compiler specifications.
 */
class NodeIndex;

struct InitializerListCheckRequest
{
    /** @brief Root node of the document's syntax tree. */
    TSNode root;

    /** @brief Document source text the tree was parsed from. */
    std::string_view sourceCode;

    /**
     * @brief Root of the document's lexical scope tree, or nullptr when none was collected.
     *
     * Needed to type an element that names something - `array<int> a = {count}` - rather than
     * spelling a literal. Without it the element check still runs on literals and stays silent
     * on everything else, which is the correct degradation.
     */
    const Scope* scopeRoot = nullptr;

    /** @brief Optional pre-indexed node index for fast single-pass querying. */
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Reports initializer lists applied to a type that cannot be built from one.
 * @param request Tree and source text of the document under analysis.
 * @param ctx Diagnostic sink; also carries the TypeConfig naming the array template.
 */
void CheckInitializerLists(const InitializerListCheckRequest& request, DiagnosticContext& ctx);

/**
 * @brief Context for inspecting elements in an initializer list.
 */
struct ElementContext
{
    /** @brief Source text of the document. */
    std::string_view sourceCode;
    /** @brief Scope for looking up named identifiers in element expressions. */
    const Scope* scopeRoot = nullptr;
};

/**
 * @brief Checks one list against a target type the caller already knows.
 *
 * A list written as a call argument is compiled against the parameter it lands on, and which
 * parameter that is depends on the overload the compiler picks - a question this pass cannot
 * answer and CallChecker already has. So the argument case is driven from there: it names the
 * type, this names the verdict, and the rules stay in one place.
 *
 * @param[in] listNode   The `initializer_list` node.
 * @param[in] targetType The type it initializes, as written; decorations are stripped here.
 * @param[in] elements   Element source and scope context.
 * @param[in,out] ctx    Diagnostic sink.
 */
void CheckInitializerListAgainstType(TSNode listNode, const std::string& targetType, const ElementContext& elements,
                                     DiagnosticContext& ctx);

/**
 * @brief Validates an initializer list recursively against an expected target type.
 * @param listNode The `initializer_list` AST node.
 * @param expectedType The expected target type name.
 * @param ctx Diagnostic context.
 */
void ValidateInitializerList(TSNode listNode, const std::string& expectedType, DiagnosticContext& ctx);

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
 * @brief Encapsulates initializer list AST node, expected target type, and active diagnostic sinks.
 */
struct ListValidationContext
{
    DiagnosticContext& ctx;
    const ElementContext& elements;
    const std::unordered_set<std::string>& arrayLikeTemplates;
    StructLayoutCache& cache;
    int depth = 0;
};

/**
 * @brief Context for inspecting and validating nested initializer list expressions.
 */
struct InitializerListContext
{
    const InitializerListCheckRequest& request;
    DiagnosticContext& ctx;
    const std::unordered_set<std::string>& arrayLikeTemplates;
    StructLayoutCache& structCache;
};

/**
 * @brief Analysis engine for recursive validation of initializer list contents against expected types.
 */
struct InitializerListValidator
{
    /**
     * @brief Validates an initializer list node against target composite, container, or struct type.
     * @param[in] listNode Initializer list AST node.
     * @param[in] targetType Target type name.
     * @param[in] valCtx Active list validation context.
     */
    static void ValidateList(TSNode listNode, const std::string& targetType, const ListValidationContext& valCtx);
};
} // namespace angel_lsp::analysis
