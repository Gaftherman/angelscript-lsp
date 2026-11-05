#include "features/linked_editing/LinkedEditingRangeHandler.h"

#include "features/common/FeatureSharedUtils.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Checks if a range-bearing item encompasses an LSP position.
 * @tparam T Type exposing startLine, endLine, startCharacter, and endCharacter coordinates.
 * @param[in] item AST node or document range item.
 * @param[in] position Target document position to check for containment.
 * @return True if the target position falls within the item's line and character bounds.
 */
template <typename T> static bool Covers(const T& item, const lsp::Position& position) noexcept
{
    if (position.line < item.startLine || position.line > item.endLine)
    {
        return false;
    }
    if (position.line == item.startLine && position.character < item.startCharacter)
    {
        return false;
    }
    if (position.line == item.endLine && position.character > item.endCharacter)
    {
        return false;
    }
    return true;
}

/**
 * @brief Finds the local definition corresponding to the cursor position, whether directly declared or referenced.
 * @param[in] scope Innermost lexical scope containing the cursor position.
 * @param[in] position Cursor position.
 * @param[out] owningScope Scope containing the resolved definition.
 * @return Pointer to the resolved LocalDefinition, or nullptr if none found.
 */
static const analysis::LocalDefinition* DefinitionAt(const analysis::Scope* scope, const lsp::Position& position,
                                                     const analysis::Scope** owningScope)
{
    analysis::ScopeTraversalGuard defGuard;
    for (const analysis::Scope* current = scope; defGuard.CheckAndInsert(current); current = current->parent)
    {
        for (const auto& definition : current->definitions)
        {
            if (Covers(definition, position))
            {
                *owningScope = current;
                return &definition;
            }
        }
    }

    analysis::ScopeTraversalGuard refGuard;
    for (const analysis::Scope* current = scope; refGuard.CheckAndInsert(current); current = current->parent)
    {
        for (const auto& reference : current->references)
        {
            if (reference.isMemberAccess || !Covers(reference, position))
            {
                continue;
            }

            return analysis::ResolveInScope(current, reference.name, owningScope);
        }
    }
    return nullptr;
}

/**
 * @brief Collects all occurrences of an identifier in a scope and its non-shadowed child scopes iteratively.
 * @param[in] rootScope Declaring lexical scope.
 * @param[in] name Identifier name to collect occurrences for.
 * @param[out] out Destination vector receiving LSP ranges.
 */
static void CollectOccurrences(const analysis::Scope& rootScope, std::string_view name, std::vector<lsp::Range>& out)
{
    std::vector<const analysis::Scope*> worklist{&rootScope};
    while (!worklist.empty())
    {
        const analysis::Scope* current = worklist.back();
        worklist.pop_back();
        if (!current)
        {
            continue;
        }

        for (const auto& definition : current->definitions)
        {
            if (definition.name == name)
            {
                out.push_back(common::ToLspRange(definition));
            }
        }
        for (const auto& reference : current->references)
        {
            if (!reference.isMemberAccess && reference.name == name)
            {
                out.push_back(common::ToLspRange(reference));
            }
        }

        for (auto it = current->children.rbegin(); it != current->children.rend(); ++it)
        {
            const auto& child = *it;
            if (!child)
            {
                continue;
            }

            const bool shadowed =
                std::any_of(child->definitions.begin(), child->definitions.end(),
                            [name](const analysis::LocalDefinition& definition) { return definition.name == name; });
            if (!shadowed)
            {
                worklist.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Sorts and deduplicates LSP ranges in document order.
 * @param[in,out] ranges Vector of ranges to sort and deduplicate.
 */
static void DeduplicateRanges(std::vector<lsp::Range>& ranges)
{
    std::sort(ranges.begin(), ranges.end(),
              [](const lsp::Range& a, const lsp::Range& b) noexcept
              {
                  if (a.start.line != b.start.line)
                  {
                      return a.start.line < b.start.line;
                  }
                  if (a.start.character != b.start.character)
                  {
                      return a.start.character < b.start.character;
                  }
                  if (a.end.line != b.end.line)
                  {
                      return a.end.line < b.end.line;
                  }
                  return a.end.character < b.end.character;
              });

    const auto last = std::unique(ranges.begin(), ranges.end(), [](const lsp::Range& a, const lsp::Range& b) noexcept
                                  { return common::SameRange(a, b); });
    ranges.erase(last, ranges.end());
}

std::optional<lsp::LinkedEditingRanges> GetLinkedEditingRanges(const LinkedEditingRangeRequest& request)
{
    if (request.sourceCode.empty() || !request.scopeRoot)
    {
        return std::nullopt;
    }

    const analysis::Scope* innermost =
        analysis::FindInnermostScope(request.scopeRoot, request.position.line, request.position.character);
    if (!innermost)
    {
        return std::nullopt;
    }

    const analysis::Scope* owningScope = nullptr;
    const analysis::LocalDefinition* definition = DefinitionAt(innermost, request.position, &owningScope);
    if (!definition || !owningScope)
    {
        return std::nullopt;
    }

    if (definition->kind != analysis::LocalDefinitionKind::Variable &&
        definition->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return std::nullopt;
    }

    if (!analysis::IsDeclaredInFunctionScope(owningScope))
    {
        return std::nullopt;
    }

    std::vector<lsp::Range> ranges;
    CollectOccurrences(*owningScope, definition->name, ranges);
    DeduplicateRanges(ranges);

    if (ranges.size() < 2)
    {
        return std::nullopt;
    }

    lsp::LinkedEditingRanges result;
    result.ranges = std::move(ranges);
    return result;
}
} // namespace angel_lsp::features
