#pragma once

#include "analysis/index/CodeActionFacts.h"
#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Classification of member references occurring in a mixin class or function body.
 */
enum class MixinRefKind : uint8_t
{
    /// Explicit member access through the 'this' pseudo-variable (e.g. this.member or this->member).
    ThisMember,
    /// Unqualified bare function call expression (e.g. helperMethod()).
    Call,
    /// Unqualified bare identifier reference (e.g. memberVariable).
    Identifier
};

/**
 * @brief Precalculated facts for a member reference within a mixin definition.
 */
struct IndexedMixinMemberRef
{
    /// Span of the referenced member-name node (member field of this.x, callee identifier, or bare identifier); its
    /// bytes give the name.
    IndexedSourceSpan span{};
    /// Syntactic category of this member reference.
    MixinRefKind kind = MixinRefKind::Identifier;

    [[nodiscard]] constexpr bool operator==(const IndexedMixinMemberRef& other) const noexcept = default;
};

/**
 * @brief Syntactic member references and local identifier scopes of a mixin function declaration.
 */
struct IndexedMixinFunction
{
    /// Byte ranges of parameter names and local variable declarators defined inside the function.
    std::vector<IndexedByteRange> localNames;
    /// Member references inside the function body: all ThisMember refs, then all Call refs, then all Identifier refs,
    /// each group in start-byte order (the diagnostic order the mixin checks rely on).
    std::vector<IndexedMixinMemberRef> refs;
};

/**
 * @brief Aggregated member references and function facts extracted from a mixin declaration body.
 */
struct MixinBodyFacts
{
    /// True when the target mixin or class declaration AST node and body exist; false otherwise.
    bool found = false;
    /// Function declarations within the mixin containing their local symbols and references.
    std::vector<IndexedMixinFunction> functions;
};

/**
 * @brief Search parameters for locating a mixin inclusion within a host class declaration.
 */
struct MixinInclusionQuery
{
    /// 0-based source coordinate of the host class declaration symbol.
    IndexedSourcePoint hostPos{};
    /// Fully qualified or canonical name of the included mixin to locate.
    std::string_view mixinName{};
    /// Short unaliased identifier name of the included mixin.
    std::string_view mixinShortName{};
};

/**
 * @brief Extracts source spans for all base class or interface entries in an inheritance list.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] startLine 0-based start line of the enclosing class or interface symbol.
 * @param[in] endLine 0-based end line of the enclosing class or interface symbol.
 * @return One span per named child of the first base_class_list node starting within
 *         [startLine, endLine]; empty when no base_class_list exists in range.
 */
[[nodiscard]] std::vector<IndexedSourceSpan> BuildBaseClassSpans(const NodeIndex& index, uint32_t startLine,
                                                                 uint32_t endLine);

/**
 * @brief Locates the source span of a mixin inclusion statement within a host class declaration.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] source Document source text buffer.
 * @param[in] query Mixin inclusion query specifying host position and mixin names.
 * @return Source span of the mixin inclusion entry in the enclosing host class;
 *         std::nullopt when the index has no tree, no host class, or no inclusion.
 */
[[nodiscard]] std::optional<IndexedSourceSpan> FindMixinInclusionSpan(const NodeIndex& index, std::string_view source,
                                                                      const MixinInclusionQuery& query);

/**
 * @brief Appends unqualified identifier references occurring within a mixin function body.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] body Byte range of the function body.
 * @param[out] refs Destination collection for mixin member references appended in start-byte order.
 * @note Identifiers that are declarations, field names, scoped-identifier segments or otherwise
 *       ignored by the mixin check are skipped here so the rule layer only sees real references.
 */
void AppendMixinIdentifierRefs(const NodeIndex& index, IndexedByteRange body, std::vector<IndexedMixinMemberRef>& refs);

/**
 * @brief Extracts AST-free member references and function declarations from a mixin body.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] source Document source text buffer.
 * @param[in] mixinPos 0-based source coordinate of the mixin declaration symbol.
 * @return Extracted mixin body facts; found=false when no declaration body exists.
 */
[[nodiscard]] MixinBodyFacts BuildMixinBodyFacts(const NodeIndex& index, std::string_view source,
                                                 IndexedSourcePoint mixinPos);

} // namespace angel_lsp::analysis
