#include "analysis/index/ScopeTree.h"

#include <algorithm>

namespace angel_lsp::analysis
{
/**
 * @brief Checks if a scope is contained within a function or closure body.
 */
static bool IsFunctionLocal(const Scope* defScope)
{
    ScopeTraversalGuard guard;
    for (const Scope* s = defScope; guard.CheckAndInsert(s); s = s->parent)
    {
        if (s->kind == ScopeKind::Function || s->kind == ScopeKind::Closure)
        {
            return true;
        }
        if (s->kind == ScopeKind::Class || s->kind == ScopeKind::Namespace || s->kind == ScopeKind::Global)
        {
            return false;
        }
    }
    return false;
}

/**
 * @brief Checks if capturing def across a closure boundary is forbidden.
 */
static bool IsForbiddenClosureCapture(const LocalDefinition& def, const Scope* defScope)
{
    if (def.kind == LocalDefinitionKind::Parameter)
    {
        return true;
    }
    if (def.kind == LocalDefinitionKind::Variable)
    {
        return IsFunctionLocal(defScope);
    }
    return false;
}

/**
 * @brief Evaluates whether a local definition is visible at the current resolution point.
 * @param[in] def Local definition candidate.
 * @param[in] current Scope containing the definition.
 * @param[in] ctx Context containing reference byte and closure traversal status.
 * @return True if definition is visible and accessible, false otherwise.
 */
static bool IsVisibleDefinition(const LocalDefinition& def, const Scope* current, const ResolutionContext& ctx)
{
    if (def.kind == LocalDefinitionKind::Variable && ctx.refByte < def.visibleFromByte)
    {
        return false;
    }

    if (def.kind == LocalDefinitionKind::Parameter &&
        (current->kind == ScopeKind::Global || current->kind == ScopeKind::Namespace ||
         current->kind == ScopeKind::Class))
    {
        return false;
    }

    if (ctx.crossedClosure && ctx.respectClosureBarrier && IsForbiddenClosureCapture(def, current))
    {
        return false;
    }

    return true;
}

const LocalDefinition* ResolveInScopeAt(const Scope* scope, std::string_view name, uint32_t refByte,
                                        const Scope** owner, bool respectClosureBarrier)
{
    ResolutionContext ctx{refByte, false, respectClosureBarrier};
    ScopeTraversalGuard cycleGuard;
    for (const Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (!cycleGuard.CheckAndInsert(current))
        {
            break;
        }
        for (const LocalDefinition& def : current->definitions)
        {
            if (def.name == name)
            {
                if (!IsVisibleDefinition(def, current, ctx))
                {
                    continue;
                }

                if (owner)
                {
                    *owner = current;
                }
                return &def;
            }
        }

        if (current->kind == ScopeKind::Closure)
        {
            ctx.crossedClosure = true;
        }
    }

    return nullptr;
}

const LocalDefinition* ResolveInScopeAt(const Scope* scope, std::string_view name, uint32_t refByte,
                                        bool respectClosureBarrier)
{
    return ResolveInScopeAt(scope, name, refByte, nullptr, respectClosureBarrier);
}

const LocalDefinition* ResolveInScope(const Scope* scope, std::string_view name, const Scope** owner,
                                      bool respectClosureBarrier)
{
    return ResolveInScopeAt(scope, name, UINT32_MAX, owner, respectClosureBarrier);
}

const LocalDefinition* ResolveInScope(const Scope* scope, std::string_view name, bool respectClosureBarrier)
{
    return ResolveInScopeAt(scope, name, UINT32_MAX, nullptr, respectClosureBarrier);
}

bool IsDeclaredInFunctionScope(const Scope* scope) noexcept
{
    ScopeTraversalGuard guard;
    for (const Scope* current = scope; guard.CheckAndInsert(current); current = current->parent)
    {
        if (current->isFunctionScope)
        {
            return true;
        }
    }
    return false;
}

bool IsShadowedByFunctionLocal(const Scope* scope, std::string_view name)
{
    const Scope* owner = nullptr;
    const auto* def = ResolveInScope(scope, name, &owner);
    return def && (def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter) &&
           IsDeclaredInFunctionScope(owner);
}

static bool DefinitionsContain(const Scope& scope, const LocalDefinition& def) noexcept
{
    for (const auto& d : scope.definitions)
    {
        if (d.startLine == def.startLine && d.startCharacter == def.startCharacter && d.name == def.name)
        {
            return true;
        }
    }
    return false;
}

const Scope* FindScopeDeclaringDefinition(const Scope* root, const LocalDefinition& def)
{
    if (!root)
    {
        return nullptr;
    }

    if (const Scope* inner = FindInnermostScope(root, def.startLine, def.startCharacter))
    {
        ScopeTraversalGuard guard;
        for (const Scope* current = inner; guard.CheckAndInsert(current); current = current->parent)
        {
            if (DefinitionsContain(*current, def))
            {
                return current;
            }
        }
    }

    std::vector<const Scope*> queue;
    queue.push_back(root);
    size_t head = 0;
    while (head < queue.size())
    {
        const Scope* current = queue[head++];
        if (DefinitionsContain(*current, def))
        {
            return current;
        }

        for (const auto& child : current->children)
        {
            if (child)
            {
                queue.push_back(child.get());
            }
        }
    }
    return nullptr;
}

const Scope* FindEnclosingClosure(const Scope* scope)
{
    ScopeTraversalGuard guard;
    for (const Scope* current = scope; guard.CheckAndInsert(current); current = current->parent)
    {
        if (current->kind == ScopeKind::Closure)
        {
            return current;
        }
    }
    return nullptr;
}

static inline bool PositionEndsBefore(const Scope& scope, uint32_t line, uint32_t character) noexcept
{
    if (scope.endLine != line)
    {
        return scope.endLine < line;
    }
    return scope.endCharacter < character;
}

const Scope* FindContainingChild(const Scope& parent, uint32_t line, uint32_t character) noexcept
{
    const auto& children = parent.children;
    const size_t count = children.size();
    if (count == 0)
    {
        return nullptr;
    }

    if (count > 8)
    {
        const auto it =
            std::lower_bound(children.begin(), children.end(), std::pair{line, character},
                             [](const std::unique_ptr<Scope>& child, const std::pair<uint32_t, uint32_t>& p) noexcept
                             { return child && PositionEndsBefore(*child, p.first, p.second); });

        if (it != children.end() && *it && ScopeContains(**it, line, character))
        {
            return it->get();
        }
        return nullptr;
    }

    for (const auto& child : children)
    {
        if (child && ScopeContains(*child, line, character))
        {
            return child.get();
        }
    }
    return nullptr;
}

const Scope* FindEnclosingScope(const Scope* root, uint32_t line, uint32_t character)
{
    if (!root)
    {
        return nullptr;
    }
    const Scope* current = root;
    while (const Scope* next = FindContainingChild(*current, line, character))
    {
        current = next;
    }
    return current;
}

const Scope* FindInnermostScope(const Scope* root, uint32_t line, uint32_t character)
{
    if (!root || !ScopeContains(*root, line, character))
    {
        return nullptr;
    }
    return FindEnclosingScope(root, line, character);
}

bool ScopeIndex::MatchesModule(const Scope& scope, const std::optional<std::string>& module)
{
    const auto* bindings = scope.GetCallBindings();
    return !module || !bindings || bindings->Module() == *module;
}

void ScopeIndex::AddCallUses(const std::string& uri, const Scope& scope)
{
    if (const auto* bindings = scope.GetCallBindings())
    {
        auto& calls = m_callUses[bindings->Module()];
        bindings->ForEachDeclaration([&](const CallDeclaration& declaration, std::span<const SourceRange> ranges)
                                     { calls[declaration].try_emplace(&uri, ranges); });
    }
}

void ScopeIndex::RemoveCallUses(const std::string& uri, const ScopeEntry& entry)
{
    if (!entry.occurrencesCurrent || !entry.root->GetCallBindings())
    {
        return;
    }
    const auto& bindings = *entry.root->GetCallBindings();
    const auto module = m_callUses.find(bindings.Module());
    if (module == m_callUses.end())
    {
        return;
    }
    auto& calls = module->second;
    bindings.ForEachDeclaration(
        [&](const CallDeclaration& declaration, [[maybe_unused]] std::span<const SourceRange> ranges)
        {
            auto group = calls.find(declaration);
            if (group != calls.end())
            {
                group->second.erase(&uri);
                if (group->second.empty())
                {
                    calls.erase(group);
                }
            }
        });
    if (calls.empty())
    {
        m_callUses.erase(module);
    }
}

/// Copies one scope's extracted data, leaving its child links to the worklist.
static void CopyLexicalScope(const Scope& source, Scope& target)
{
    target.definitions = source.definitions;
    target.references = source.references;
    target.stringLiteralCallRefs =
        source.stringLiteralCallRefs
            ? std::make_unique<std::vector<StringLiteralCallRef>>(*source.stringLiteralCallRefs)
            : nullptr;
    target.startLine = source.startLine;
    target.startCharacter = source.startCharacter;
    target.endLine = source.endLine;
    target.endCharacter = source.endCharacter;
    target.kind = source.kind;
    target.isFunctionScope = source.isFunctionScope;
    target.isConstMethod = source.isConstMethod;
    target.containerSpanIndex = source.containerSpanIndex;
    target.enclosingContext = source.enclosingContext;
}

std::shared_ptr<Scope> CopyLexicalScopeTree(const Scope& root)
{
    auto copy = std::make_shared<Scope>();
    std::vector<std::pair<const Scope*, Scope*>> pending{{&root, copy.get()}};
    while (!pending.empty())
    {
        const auto [source, target] = pending.back();
        pending.pop_back();
        CopyLexicalScope(*source, *target);
        target->children.reserve(source->children.size());
        for (const auto& child : source->children)
        {
            auto next = std::make_unique<Scope>();
            next->parent = target;
            pending.emplace_back(child.get(), next.get());
            target->children.push_back(std::move(next));
        }
    }
    return copy;
}

void ScopeIndex::SetScopeTree(const std::string& fileUri, std::unique_ptr<Scope> root)
{
    SetScopeTree(fileUri, std::shared_ptr<const Scope>(std::move(root)));
}

void ScopeIndex::SetScopeTree(const std::string& fileUri, std::shared_ptr<const Scope> root)
{
    if (!root)
    {
        ClearDocument(fileUri);
        return;
    }
    std::unique_lock lock(m_mutex);
    const auto entry = m_roots.try_emplace(fileUri).first;
    RemoveCallUses(entry->first, entry->second);
    entry->second = {std::move(root), true};
    AddCallUses(entry->first, *entry->second.root);
}

void ScopeIndex::ClearDocument(const std::string& fileUri)
{
    std::unique_lock lock(m_mutex);
    const auto entry = m_roots.find(fileUri);
    if (entry == m_roots.end())
    {
        return;
    }
    RemoveCallUses(entry->first, entry->second);
    m_roots.erase(entry);
}

void ScopeIndex::InvalidateDocumentOccurrences(const std::string& fileUri)
{
    std::unique_lock lock(m_mutex);
    if (const auto entry = m_roots.find(fileUri); entry != m_roots.end())
    {
        RemoveCallUses(entry->first, entry->second);
        entry->second.occurrencesCurrent = false;
    }
}

std::shared_ptr<const Scope> ScopeIndex::GetRoot(const std::string& fileUri) const
{
    std::shared_lock lock(m_mutex);
    const auto entry = m_roots.find(fileUri);
    return entry == m_roots.end() ? nullptr : entry->second.root;
}

std::shared_ptr<const Scope> ScopeIndex::GetOccurrenceRoot(const std::string& fileUri,
                                                           const std::optional<std::string>& module) const
{
    std::shared_lock lock(m_mutex);
    const auto entry = m_roots.find(fileUri);
    if (entry == m_roots.end() || !entry->second.occurrencesCurrent || !MatchesModule(*entry->second.root, module))
    {
        return nullptr;
    }
    return entry->second.root;
}
} // namespace angel_lsp::analysis
