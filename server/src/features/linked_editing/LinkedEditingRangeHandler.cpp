#include "features/linked_editing/LinkedEditingRangeHandler.h"
#include "features/common/FeatureSharedUtils.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Contains.
 * @param[in] scope Enclosing lexical scope.
 * @param[in] position Target document position.
 * @return True if the condition is satisfied; false otherwise.
 */
static bool Contains(const analysis::Scope& scope, const lsp::Position& position)
{
    if (position.line < scope.startLine || position.line > scope.endLine)
    {
        return false;
    }
    if (position.line == scope.startLine && position.character < scope.startCharacter)
    {
        return false;
    }
    if (position.line == scope.endLine && position.character > scope.endCharacter)
    {
        return false;
    }
    return true;
}

/** @brief Innermost scope containing a position, or nullptr when the tree does not cover it. */
static const analysis::Scope* InnermostScope(const analysis::Scope* root, const lsp::Position& position)
{
    if (!root || !Contains(*root, position))
    {
        return nullptr;
    }

    const analysis::Scope* current = root;
    for (bool descended = true; descended;)
    {
        descended = false;
        for (const auto& child : current->children)
        {
            if (child && Contains(*child, position))
            {
                current = child.get();
                descended = true;
                break;
            }
        }
    }
    return current;
}

/**
 * @brief Covers.
 * @param[in] item AST node or document range item.
 * @param[in] position Target document position.
 * @return True if the condition is satisfied; false otherwise.
 */
template <typename T> static bool Covers(const T& item, const lsp::Position& position)
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
 * @brief The local or parameter the cursor sits on, whether at its declaration or a use.
 *
 * A member access is skipped: the `y` in `x.y` is spelled like a local and is not one, and
 * retyping it together with an unrelated local of the same name would corrupt both.
 */
static const analysis::LocalDefinition* DefinitionAt(const analysis::Scope* scope, const lsp::Position& position,
                                                     const analysis::Scope** owningScope)
{
    for (const analysis::Scope* current = scope; current != nullptr; current = current->parent)
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

    // Not on the declaration, so look for a use and resolve it back.
    for (const analysis::Scope* current = scope; current != nullptr; current = current->parent)
    {
        for (const auto& reference : current->references)
        {
            if (reference.isMemberAccess || !Covers(reference, position))
            {
                continue;
            }

            for (const analysis::Scope* owner = scope; owner != nullptr; owner = owner->parent)
            {
                for (const auto& definition : owner->definitions)
                {
                    if (definition.name == reference.name)
                    {
                        *owningScope = owner;
                        return &definition;
                    }
                }
            }
            return nullptr;
        }
    }
    return nullptr;
}

/**
 * @brief True when a scope is a function body, or sits inside one.
 *
 * The scope tree captures a module-scope global and a function-body local under the very
 * same analysis::LocalDefinitionKind::Variable - the query cannot tell them apart, and says so. What
 * does tell them apart is where the scope owning the definition sits, and this is the only
 * thing standing between offering linked editing on a local and offering it on a global
 * that half the workspace refers to.
 */
static bool IsInsideFunction(const analysis::Scope* scope)
{
    for (const analysis::Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (current->isFunctionScope)
        {
            return true;
        }
    }
    return false;
}

/** @brief Collects every occurrence of a name inside one scope and its children. */
static void CollectIn(const analysis::Scope& scope, const std::string& name, std::vector<lsp::Range>& out)
{
    for (const auto& definition : scope.definitions)
    {
        if (definition.name == name)
        {
            out.push_back(common::ToLspRange(definition));
        }
    }
    for (const auto& reference : scope.references)
    {
        if (!reference.isMemberAccess && reference.name == name)
        {
            out.push_back(common::ToLspRange(reference));
        }
    }

    for (const auto& child : scope.children)
    {
        if (!child)
        {
            continue;
        }

        // A child scope that redeclares the name owns its own, and its occurrences belong
        // to that one rather than to this. Descending into it anyway is how a rename gets
        // a shadowed variable wrong.
        const bool shadowed =
            std::any_of(child->definitions.begin(), child->definitions.end(),
                        [&name](const analysis::LocalDefinition& definition) { return definition.name == name; });
        if (!shadowed)
        {
            CollectIn(*child, name, out);
        }
    }
}

std::optional<lsp::LinkedEditingRanges> GetLinkedEditingRanges(const LinkedEditingRangeRequest& request)
{
    if (request.sourceCode.empty() || !request.scopeRoot)
    {
        return std::nullopt;
    }

    const analysis::Scope* innermost = InnermostScope(request.scopeRoot, request.position);
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

    // Only a local or a parameter, which a lexical scope keeps inside this document. See the
    // header for why anything at file scope has to go through rename instead.
    if (definition->kind != analysis::LocalDefinitionKind::Variable &&
        definition->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return std::nullopt;
    }

    if (!IsInsideFunction(owningScope))
    {
        return std::nullopt;
    }

    std::vector<lsp::Range> ranges;
    CollectIn(*owningScope, definition->name, ranges);

    // A declaration's own identifier is captured twice - once as the definition, once by the
    // bare-identifier reference pattern - and the protocol says the ranges may not overlap, so
    // two copies of one range would be a malformed answer as well as a misleading count.
    for (size_t i = ranges.size(); i-- > 0;)
    {
        const bool duplicate = std::any_of(ranges.begin(), ranges.begin() + i, [&ranges, i](const lsp::Range& earlier)
                                           { return common::SameRange(earlier, ranges[i]); });
        if (duplicate)
        {
            ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    if (ranges.size() < 2)
    {
        // One occurrence is a declaration nobody uses yet. There is nothing to link it to, and
        // an editor given a single range gains nothing it did not already have.
        return std::nullopt;
    }

    lsp::LinkedEditingRanges result;
    result.ranges = std::move(ranges);
    return result;
}
} // namespace angel_lsp::features
