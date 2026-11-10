#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/Symbol.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Scoped identifier reference such as `A::B` or `::A::B`.
 */
struct ScopedIdentifierFact
{
    IndexedByteRange bytes;         ///< Byte span of the entire scoped identifier node.
    SourceRange range;              ///< Line and column span of the scoped identifier.
    std::string_view qualifiedText; ///< Full qualified spelling including colons.
    IndexedByteRange prefixBytes;   ///< Byte span of the leading scope qualifier identifier.
    SourceRange prefixRange;        ///< Line and column span of the leading scope qualifier.
    std::string_view prefix;        ///< Scope qualifier prefix name.
    IndexedByteRange memberBytes;   ///< Byte span of the trailing member identifier.
    SourceRange memberRange;        ///< Line and column span of the trailing member identifier.
    std::string_view memberName;    ///< Trailing member identifier name.
    bool hasLeadingGlobal = false;  ///< True if prefixed with the global scope operator `::`.
};

/**
 * @brief Scoped type reference such as `A::B` appearing in a type position.
 */
struct ScopedTypeFact
{
    IndexedByteRange bytes;         ///< Byte span of the scoped type node.
    SourceRange range;              ///< Line and column span of the scoped type.
    std::string_view qualifiedText; ///< Full qualified spelling including colons.
    IndexedByteRange prefixBytes;   ///< Byte span of the leading scope qualifier identifier.
    SourceRange prefixRange;        ///< Line and column span of the leading scope qualifier.
    std::string_view prefix;        ///< Scope qualifier prefix name.
    IndexedByteRange memberBytes;   ///< Byte span of the trailing type identifier.
    SourceRange memberRange;        ///< Line and column span of the trailing type identifier.
    std::string_view memberName;    ///< Trailing type name identifier.
    bool hasLeadingGlobal = false;  ///< True if prefixed with the global scope operator `::`.
};

/**
 * @brief Namespace declaration statement capturing its name and body spans.
 */
struct NamespaceDeclarationFact
{
    IndexedByteRange bytes;     ///< Byte span of the namespace declaration.
    SourceRange range;          ///< Line and column span of the namespace declaration.
    IndexedByteRange nameBytes; ///< Byte span of the declared namespace name.
    SourceRange nameRange;      ///< Line and column span of the declared namespace name.
    std::string_view name;      ///< Declared namespace identifier or scoped path.
    IndexedByteRange bodyBytes; ///< Byte span of the namespace body block.
    SourceRange bodyRange;      ///< Line and column span of the namespace body block.
};

/**
 * @brief Using declaration importing a namespace into lexical scope.
 */
struct UsingDeclarationFact
{
    IndexedByteRange bytes;         ///< Byte span of the using declaration.
    SourceRange range;              ///< Line and column span of the using declaration.
    IndexedByteRange nameBytes;     ///< Byte span of the imported namespace identifier.
    SourceRange nameRange;          ///< Line and column span of the imported namespace identifier.
    std::string_view namespaceName; ///< Imported namespace identifier or scoped path.
};

/**
 * @brief Import declaration or import syntax error node.
 */
struct ImportDeclarationFact
{
    IndexedByteRange bytes;     ///< Byte span of the import declaration or error node.
    SourceRange range;          ///< Line and column span of the import statement.
    std::string_view text;      ///< Source text of the import declaration.
    bool hasBodyBlock = false;  ///< True if mistakenly followed by a `{ ... }` body block.
    bool isSyntaxError = false; ///< True if captured from a tree-sitter error node.
};

/**
 * @brief Enclosing namespace, class, or interface container metadata.
 */
struct EnclosingContainerFact
{
    IndexedByteRange bytes;                        ///< Byte span of the container declaration.
    SourceRange range;                             ///< Line and column span of the container declaration.
    std::string_view name;                         ///< Container identifier or qualified name in source.
    ContainerKind kind = ContainerKind::Namespace; ///< Container classification (Namespace, Class, Interface).
    bool isMixin = false;                          ///< True if this container is a mixin class.
};

/**
 * @brief Identifier use site with pre-indexed enclosing container context.
 */
struct IdentifierUseSiteFact
{
    IndexedByteRange bytes;      ///< Byte span of the identifier node.
    SourceRange range;           ///< Line and column span of the identifier node.
    std::string_view name;       ///< Identifier spelling.
    bool isInsideError = false;  ///< True if the identifier resides inside a syntax error node.
    bool isInsideMixin = false;  ///< True if the identifier resides inside a mixin declaration.
    uint32_t containerBegin = 0; ///< First slice index into NamespaceUsageFacts::enclosingContainers.
    uint32_t containerCount = 0; ///< Number of enclosing containers, innermost first.
};

/**
 * @brief AST-free facts for namespace usage, scoped references, and identifier container contexts.
 * @note Views point into the source passed to BuildNamespaceUsageFacts, which must outlive this object.
 */
struct NamespaceUsageFacts
{
    std::vector<ScopedIdentifierFact> scopedIdentifiers;         ///< Ordered by start byte, then end descending.
    std::vector<ScopedTypeFact> scopedTypes;                     ///< Ordered by start byte, then end descending.
    std::vector<NamespaceDeclarationFact> namespaceDeclarations; ///< Ordered by start byte, then end descending.
    std::vector<UsingDeclarationFact> usingDeclarations;         ///< Ordered by start byte, then end descending.
    std::vector<ImportDeclarationFact> importDeclarations;       ///< Ordered by start byte, then end descending.
    std::vector<IdentifierUseSiteFact> identifierUseSites;       ///< Ordered by start byte, then end descending.
    std::vector<EnclosingContainerFact> enclosingContainers;     ///< Sliced by IdentifierUseSiteFact.

    /**
     * @brief Finds the scoped identifier fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the scoped identifier node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ScopedIdentifierFact* FindScopedIdentifier(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(scopedIdentifiers, bytes);
    }

    /**
     * @brief Finds the scoped type fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the scoped type node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ScopedTypeFact* FindScopedType(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(scopedTypes, bytes);
    }

    /**
     * @brief Finds the namespace declaration fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the namespace declaration node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const NamespaceDeclarationFact* FindNamespaceDeclaration(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(namespaceDeclarations, bytes);
    }

    /**
     * @brief Finds the using declaration fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the using declaration node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const UsingDeclarationFact* FindUsingDeclaration(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(usingDeclarations, bytes);
    }

    /**
     * @brief Finds the import declaration fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the import declaration node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ImportDeclarationFact* FindImportDeclaration(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(importDeclarations, bytes);
    }

    /**
     * @brief Finds the identifier use site fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the identifier node.
     * @return Matching fact, or nullptr when not found.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const IdentifierUseSiteFact* FindIdentifierUseSite(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(identifierUseSites, bytes);
    }

    /**
     * @brief Retrieves the enclosing container slice for an identifier use site.
     * @param[in] site Identifier use site fact.
     * @return Span of enclosing container facts, innermost first.
     */
    [[nodiscard]] std::span<const EnclosingContainerFact>
    EnclosingContainersFor(const IdentifierUseSiteFact& site) const noexcept
    {
        if (site.containerBegin >= enclosingContainers.size())
        {
            return {};
        }
        const uint32_t count =
            std::min(site.containerCount, static_cast<uint32_t>(enclosingContainers.size() - site.containerBegin));
        return std::span<const EnclosingContainerFact>(enclosingContainers.data() + site.containerBegin, count);
    }
};

/**
 * @brief Captures namespace usage, scoped references, and identifier container contexts of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text; the facts' views point into it.
 * @return Namespace usage facts, ordered by source position for exact-span binary search.
 */
[[nodiscard]] NamespaceUsageFacts BuildNamespaceUsageFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
