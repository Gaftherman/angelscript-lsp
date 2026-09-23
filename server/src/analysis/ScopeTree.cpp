#include "analysis/ScopeTree.h"

namespace angel_lsp::analysis
{
namespace
{
/**
 * @brief Checks if a scope is contained within a function or closure body.
 */
bool IsFunctionLocal(const Scope* defScope)
{
    size_t depth = 0;
    for (const Scope* s = defScope; s != nullptr; s = s->parent)
    {
        if (++depth > kMaxScopeDepth)
        {
            break;
        }
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
bool IsForbiddenClosureCapture(const LocalDefinition& def, const Scope* defScope)
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
} // namespace

const LocalDefinition* ResolveInScope(const Scope* scope, std::string_view name, const Scope** owner,
                                      bool respectClosureBarrier)
{
    bool crossedClosure = false;
    size_t depth = 0;
    for (const Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (++depth > kMaxScopeDepth)
        {
            break;
        }
        for (const LocalDefinition& def : current->definitions)
        {
            if (def.name == name)
            {
                if (crossedClosure && respectClosureBarrier && IsForbiddenClosureCapture(def, current))
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
            crossedClosure = true;
        }
    }

    return nullptr;
}

const LocalDefinition* ResolveInScope(const Scope* scope, std::string_view name, bool respectClosureBarrier)
{
    return ResolveInScope(scope, name, nullptr, respectClosureBarrier);
}

const Scope* FindScopeDeclaringDefinition(const Scope* root, const LocalDefinition& def)
{
    if (!root)
    {
        return nullptr;
    }

    std::vector<const Scope*> queue;
    queue.push_back(root);
    ankerl::unordered_dense::set<const Scope*> visited;
    visited.insert(root);

    size_t head = 0;
    while (head < queue.size())
    {
        const Scope* current = queue[head++];
        for (const auto& d : current->definitions)
        {
            if (d.name == def.name && d.startLine == def.startLine && d.startCharacter == def.startCharacter)
            {
                return current;
            }
        }

        for (const auto& child : current->children)
        {
            if (child && visited.insert(child.get()).second)
            {
                queue.push_back(child.get());
            }
        }
    }
    return nullptr;
}

const Scope* FindEnclosingClosure(const Scope* scope)
{
    size_t depth = 0;
    for (const Scope* current = scope; current != nullptr; current = current->parent)
    {
        if (++depth > kMaxScopeDepth)
        {
            break;
        }
        if (current->kind == ScopeKind::Closure)
        {
            return current;
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
    size_t depth = 0;

    while (current && ++depth <= kMaxScopeDepth)
    {
        const Scope* next = nullptr;
        for (const auto& child : current->children)
        {
            if (!child)
            {
                continue;
            }
            if (line >= child->startLine && line <= child->endLine)
            {
                if (line == child->startLine && character < child->startCharacter)
                {
                    continue;
                }
                if (line == child->endLine && character > child->endCharacter)
                {
                    continue;
                }
                next = child.get();
                break;
            }
        }
        if (!next)
        {
            break;
        }
        current = next;
    }
    return current;
}

const Scope* FindInnermostScope(const Scope* root, uint32_t line, uint32_t character)
{
    if (!root)
    {
        return nullptr;
    }

    const auto contains = [line, character](const Scope& scope)
    {
        if (line < scope.startLine || line > scope.endLine)
        {
            return false;
        }
        if (line == scope.startLine && character < scope.startCharacter)
        {
            return false;
        }
        if (line == scope.endLine && character > scope.endCharacter)
        {
            return false;
        }
        return true;
    };

    if (!contains(*root))
    {
        return nullptr;
    }

    const Scope* current = root;
    size_t depth = 0;
    for (bool descended = true; descended;)
    {
        if (++depth > kMaxScopeDepth)
        {
            break;
        }
        descended = false;
        for (const auto& child : current->children)
        {
            if (child && contains(*child))
            {
                current = child.get();
                descended = true;
                break;
            }
        }
    }
    return current;
}

void ScopeIndex::SetScopeTree(const std::string& fileUri, std::unique_ptr<Scope> root)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_roots[fileUri] = std::shared_ptr<Scope>(std::move(root));
}

void ScopeIndex::SetScopeTree(const std::string& fileUri, std::shared_ptr<const Scope> root)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_roots[fileUri] = std::const_pointer_cast<Scope>(root);
}

void ScopeIndex::ClearDocument(const std::string& fileUri)
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_roots.erase(fileUri);
}

std::shared_ptr<const Scope> ScopeIndex::GetRoot(const std::string& fileUri) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto it = m_roots.find(fileUri);
    if (it == m_roots.end())
        return nullptr;

    return it->second;
}
} // namespace angel_lsp::analysis
