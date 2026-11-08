#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Syntactic classification of candidate mutation sites within a method body.
 */
enum class MutationSiteKind : uint8_t
{
    /// Left side identifier or scoped identifier of an assignment or operand of ++/--.
    NamedTarget,
    /// Member name of a `this.member` assignment target.
    ThisMember,
    /// Callee method name of an implicit or explicit call on `this`.
    ThisCall,
};

/**
 * @brief Candidate mutation site detected syntactically within a method body.
 */
struct IndexedMutationSite
{
    /// Kind of candidate mutation site.
    MutationSiteKind kind = MutationSiteKind::NamedTarget;
    /// Byte range of the target name node within document text.
    IndexedByteRange name;
    /// 0-based start row of the name node.
    uint32_t line = 0;
};

/**
 * @brief Pre-indexed mutation facts and candidate sites for a class method.
 */
struct IndexedMethodMutationFacts
{
    /// Name byte range of each variable_declarator in the class outside nested function declarations.
    std::vector<IndexedByteRange> classFieldNames;
    /// Candidate mutations in body traversal order.
    std::vector<IndexedMutationSite> sites;
    /// 0-based start row of the method body.
    uint32_t bodyStartLine = 0;
    /// 0-based end row of the method body.
    uint32_t bodyEndLine = 0;
};

} // namespace angel_lsp::analysis
