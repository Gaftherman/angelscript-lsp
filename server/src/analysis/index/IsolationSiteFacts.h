#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Precalculated facts for a 'shared' modifier keyword token and its target entity.
 */
struct SharedModifierSiteFact
{
    IndexedByteRange bytes;       ///< Byte span of the 'shared' keyword token.
    SourceRange range;            ///< Line and column span of the 'shared' keyword token.
    IndexedByteRange entityBytes; ///< Byte span of the modified declaration node.
    SourceRange entityRange;      ///< Line and column span of the modified declaration node.
    uint16_t entitySymbol = 0;    ///< Tree-sitter grammar symbol of the modified declaration.
    bool isValid = false;         ///< Indicates whether the modifier is attached to an eligible shared entity.
};

/**
 * @brief Precalculated facts for a data type reference evaluated within an isolation context.
 */
struct IsolationTypeSiteFact
{
    IndexedByteRange bytes;          ///< Byte span of the datatype node.
    SourceRange range;               ///< Line and column span of the datatype node.
    std::string_view rawText;        ///< Unprocessed source text spelling of the type.
    std::string_view cleanName;      ///< Type name with surrounding whitespace and modifier decorations removed.
    std::string_view enclosingClass; ///< Name of the class declaring or enclosing this type usage.
    bool inSharedContext = false;    ///< Indicates whether this type appears inside a shared context.
};

/**
 * @brief Precalculated facts for a call or construct invocation evaluated for isolation safety.
 */
struct IsolationCallSiteFact
{
    IndexedByteRange bytes;          ///< Byte span of the call expression node.
    SourceRange range;               ///< Line and column span of the call expression node.
    IndexedByteRange calleeBytes;    ///< Byte span of the invoked callee expression.
    SourceRange calleeRange;         ///< Line and column span of the invoked callee expression.
    std::string_view calleeText;     ///< Trimmed source text spelling of the callee identifier.
    std::string_view enclosingClass; ///< Name of the class containing this call expression.
    bool isDirectOrScoped = false;   ///< True when callee is a plain identifier or scoped identifier.
    bool inSharedContext = false;    ///< Indicates whether this call executes inside a shared context.
};

/**
 * @brief Precalculated facts for an identifier token inspected during variable isolation analysis.
 */
struct IsolationIdentifierSiteFact
{
    IndexedByteRange bytes;          ///< Byte span of the identifier token.
    SourceRange range;               ///< Line and column span of the identifier token.
    std::string_view name;           ///< Trimmed identifier spelling.
    std::string_view enclosingClass; ///< Name of the enclosing class declaration, if any.
    bool isSpecialContext = false;   ///< True when identifier appears in a declaration, type, member, or callee role.
    bool isCallee = false;           ///< True when identifier serves as the callee of a call expression.
    bool isMember = false;           ///< True when identifier serves as the member field of a member expression.
    bool isDeclarationName = false;  ///< True when identifier declares a new variable, function, class, or parameter.
    bool inSharedContext = false;    ///< Indicates whether this identifier occurs within a shared context.
};

/**
 * @brief Precalculated facts and child site slices for a function declaration.
 */
struct FunctionIsolationSiteFact
{
    IndexedByteRange bytes;       ///< Byte span of the function declaration.
    SourceRange range;            ///< Line and column span of the function declaration.
    SourceRange nameRange;        ///< Line and column span of the function name identifier.
    std::string_view name;        ///< Function identifier spelling.
    std::string_view className;   ///< Name of the enclosing class, or empty for free functions.
    bool isShared = false;        ///< Function declaration includes the 'shared' modifier.
    bool isClassShared = false;   ///< Enclosing class declaration includes the 'shared' modifier.
    bool inSharedContext = false; ///< Function operates under shared isolation constraints.
    uint32_t identifierBegin = 0; ///< First slice index in IsolationSiteFacts::identifiers.
    uint32_t identifierCount = 0; ///< Number of identifiers within this function body.
    uint32_t callBegin = 0;       ///< First slice index in IsolationSiteFacts::calls.
    uint32_t callCount = 0;       ///< Number of calls within this function body.
    uint32_t typeBegin = 0;       ///< First slice index in IsolationSiteFacts::types.
    uint32_t typeCount = 0;       ///< Number of types within this function body.
};

/**
 * @brief Precalculated facts and member method slices for a class declaration.
 */
struct ClassIsolationSiteFact
{
    IndexedByteRange bytes;     ///< Byte span of the class declaration.
    SourceRange range;          ///< Line and column span of the class declaration.
    SourceRange nameRange;      ///< Line and column span of the class name identifier.
    std::string_view name;      ///< Class identifier spelling.
    bool isShared = false;      ///< Class declaration includes the 'shared' modifier.
    uint32_t functionBegin = 0; ///< First slice index in IsolationSiteFacts::functions.
    uint32_t functionCount = 0; ///< Number of member functions in this class.
    uint32_t typeBegin = 0;     ///< First slice index in IsolationSiteFacts::types.
    uint32_t typeCount = 0;     ///< Number of field types directly in this class.
};

/**
 * @brief Aggregated collection of pass-1 isolation facts for a syntax revision.
 */
struct IsolationSiteFacts
{
    std::vector<SharedModifierSiteFact> sharedModifiers;  ///< All 'shared' modifier keyword sites.
    std::vector<ClassIsolationSiteFact> classes;          ///< Class declarations ordered by start byte.
    std::vector<FunctionIsolationSiteFact> functions;     ///< Function declarations ordered by start byte.
    std::vector<IsolationIdentifierSiteFact> identifiers; ///< Identifier references in source order.
    std::vector<IsolationCallSiteFact> calls;             ///< Call expressions in source order.
    std::vector<IsolationTypeSiteFact> types;             ///< Data type expressions in source order.

    /**
     * @brief Finds a class declaration site matching an exact byte range.
     * @param[in] bytes Exact byte span to search for.
     * @return Pointer to matching class site fact, or nullptr if not found.
     */
    [[nodiscard]] inline const ClassIsolationSiteFact* FindClass(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(classes, bytes);
    }

    /**
     * @brief Finds a function declaration site matching an exact byte range.
     * @param[in] bytes Exact byte span to search for.
     * @return Pointer to matching function site fact, or nullptr if not found.
     */
    [[nodiscard]] inline const FunctionIsolationSiteFact* FindFunction(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(functions, bytes);
    }

    /**
     * @brief Finds a call expression site matching an exact byte range.
     * @param[in] bytes Exact byte span to search for.
     * @return Pointer to matching call site fact, or nullptr if not found.
     */
    [[nodiscard]] inline const IsolationCallSiteFact* FindCall(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(calls, bytes);
    }

    /**
     * @brief Finds an identifier site matching an exact byte range.
     * @param[in] bytes Exact byte span to search for.
     * @return Pointer to matching identifier site fact, or nullptr if not found.
     */
    [[nodiscard]] inline const IsolationIdentifierSiteFact* FindIdentifier(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(identifiers, bytes);
    }

    /**
     * @brief Finds a type expression site matching an exact byte range.
     * @param[in] bytes Exact byte span to search for.
     * @return Pointer to matching type site fact, or nullptr if not found.
     */
    [[nodiscard]] inline const IsolationTypeSiteFact* FindType(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(types, bytes);
    }

    /**
     * @brief Finds a shared modifier keyword site matching an exact byte range.
     * @param[in] bytes Exact byte span to search for.
     * @return Pointer to matching modifier site fact, or nullptr if not found.
     */
    [[nodiscard]] inline const SharedModifierSiteFact* FindSharedModifier(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(sharedModifiers, bytes);
    }
};

/**
 * @brief Extracts all isolation and thread-sharing facts from document syntax nodes in pass 1.
 * @param[in] index Precomputed syntax node index.
 * @param[in] source Document source text buffer.
 * @return Aggregated collection of pass-1 isolation facts.
 */
[[nodiscard]] IsolationSiteFacts BuildIsolationSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
