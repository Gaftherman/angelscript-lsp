#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"

#include <tree_sitter/api.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "utils/LspLogger.h"

namespace angel_lsp::analysis
{
/** @brief What kind of thing the cursor is on, which decides where its occurrences can be. */
enum class TargetKind
{
    Local,           ///< A variable or parameter, confined to one function scope.
    ClassMember,     ///< A field or method, reachable through a class hierarchy.
    NamespaceSymbol, ///< Declared inside a namespace and qualified by it.
    GlobalSymbol,    ///< Everything else: a free function, a type, a global variable.
    EnumMember       ///< An enumerator member of an enum.
};

/** @brief The symbol under the cursor, resolved far enough to look its occurrences up. */
struct TargetDescriptor
{
    TargetKind kind = TargetKind::GlobalSymbol;
    std::string name;
    std::string qualifiedName;

    const Scope* definingScope = nullptr;
    LocalDefinition localDef;
    std::string localUri;

    std::string declaringClass;
    std::vector<std::string> relatedClasses;
    AccessModifier access = AccessModifier::Public;

    std::string declaringNamespace;
    std::string declaringEnum;

    bool isFunction = false;
    size_t minArgs = 0;
    size_t maxArgs = 0;
};

/**
 * @brief Cursor position for target resolution in analysis layer.
 */
struct TargetPosition
{
    uint32_t line = 0;
    uint32_t character = 0;
};

/**
 * @brief Location of a symbol occurrence in source code.
 */
struct OccurrenceLocation
{
    std::string fileUri;
    SourceRange range;
};

/**
 * @brief True for a name that is a legal AngelScript identifier and not reserved.
 *
 * Rename needs it for the *new* name as well, which is why it is exported rather than private
 * to this module.
 * @param[in] name Identifier name candidate.
 * @return True if valid non-reserved AngelScript identifier.
 */
bool IsValidIdentifier(std::string_view name);

/**
 * @brief Context and input parameters for resolving the target symbol under the cursor.
 */
struct ResolveTargetRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    TargetPosition position;
    const SymbolTable& symbolTable;
    const ScopeIndex& scopeIndex;
    TSNode& outNode;
    angel_lsp::utils::LspLogger* logger = nullptr;
};

/**
 * @brief Context and input parameters for collecting occurrences of a resolved target.
 */
struct CollectOccurrencesRequest
{
    const TargetDescriptor& target;
    std::string currentUri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    const SymbolTable& symbolTable;
    const ScopeIndex& scopeIndex;
    bool includeDeclaration = true;
    angel_lsp::utils::LspLogger* logger = nullptr;
};

/**
 * @brief Works out what the cursor is on.
 *
 * @param[in,out] request Immutable request context.
 * @return The target, or nullopt when the position is not on a renameable symbol.
 */
std::optional<TargetDescriptor> ResolveTargetSymbol(ResolveTargetRequest& request);

/**
 * @brief Every place the resolved target appears, across every indexed document.
 *
 * @param[in] request Immutable occurrences collection request.
 * @return Locations of all occurrences found.
 */
std::vector<OccurrenceLocation> CollectOccurrences(const CollectOccurrencesRequest& request);

/**
 * @brief Validation constraints and typing expectations for function call arguments.
 */
struct FunctionArgConstraints
{
    bool cursorOnCall = false;
    size_t callArgCount = 0;
};

/**
 * @brief Accumulates AST occurrences and byte ranges matching target symbol references.
 */
struct OccurrenceCollector
{
    std::vector<OccurrenceLocation> results;
    std::set<std::tuple<std::string, uint32_t, uint32_t>> seen;
    std::set<std::tuple<std::string, uint32_t, uint32_t>> declRanges;

    /**
     * @brief Adds a location if not already collected.
     * @param[in] uri File URI.
     * @param[in] range Target range.
     */
    void Add(const std::string& uri, const SourceRange& range)
    {
        if (seen.insert({uri, range.startLine, range.startCharacter}).second)
        {
            results.push_back(OccurrenceLocation{uri, range});
        }
    }

    /**
     * @brief Adds a local reference occurrence.
     * @param[in] uri File URI.
     * @param[in] ref Reference to add.
     */
    void Add(const std::string& uri, const analysis::LocalReference& ref)
    {
        Add(uri, SourceRange{ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter});
    }
};

/**
 * @brief Byte range and token metadata identifying a symbol reference in source text.
 */
struct SymbolSpan
{
    uint32_t sL = 0;
    uint32_t sC = 0;
    uint32_t eL = 0;
    uint32_t eC = 0;
};

/**
 * @brief Context for scanning AST nodes for occurrences of a specific symbol target.
 */
struct OccurrenceScanContext
{
    std::string fileUri;
    const CollectOccurrencesRequest& request;
    const std::set<std::tuple<std::string, uint32_t, uint32_t>>& declRanges;
    OccurrenceCollector& collector;
};
} // namespace angel_lsp::analysis
