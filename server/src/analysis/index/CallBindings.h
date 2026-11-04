#pragma once

#include "analysis/index/Symbol.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Physical identity of a declaration in the symbol snapshot used to bind a call.
 */
struct CallDeclaration
{
    std::string uri;
    std::string qualifiedName;
    SourceRange range;
    std::string signature;

    explicit CallDeclaration(const Symbol& symbol);

    [[nodiscard]] bool Matches(const Symbol& symbol) const noexcept;
    bool operator==(const CallDeclaration&) const noexcept = default;
};

struct CallDeclarationHash
{
    size_t operator()(const CallDeclaration& declaration) const noexcept;
};

/** @brief Editor and configuration revision used to interpret byte offsets in the call index. */
struct CallBindingRevision
{
    uint64_t generation = 0;
    int version = -1;
    uint64_t configRevision = 0;

    bool operator==(const CallBindingRevision&) const noexcept = default;
};

/**
 * @brief Call links built with one document's semantic revision and published with its scope tree.
 */
class CallBindings
{
  public:
    CallBindings(std::optional<CallBindingRevision> revision, std::string module)
        : m_revision(revision), m_module(std::move(module))
    {
    }
    CallBindings(const CallBindings&) = delete;
    CallBindings& operator=(const CallBindings&) = delete;

    void Add(uint32_t nameByte, const SourceRange& use, const Symbol& declaration);
    [[nodiscard]] const CallDeclaration* Find(uint32_t nameByte) const;
    [[nodiscard]] bool IsFrom(const std::optional<CallBindingRevision>& revision) const noexcept
    {
        return m_revision == revision;
    }
    [[nodiscard]] const std::string& Module() const noexcept
    {
        return m_module;
    }

    template <typename Fn> void ForEachDeclaration(Fn&& callback) const
    {
        for (const auto& [declaration, ranges] : m_uses)
        {
            callback(declaration, std::span<const SourceRange>(ranges));
        }
    }

  private:
    std::optional<CallBindingRevision> m_revision;
    std::string m_module;

    // The node-based map keeps the declaration address valid through rehashes.
    std::unordered_map<CallDeclaration, std::vector<SourceRange>, CallDeclarationHash> m_uses;
    ankerl::unordered_dense::map<uint32_t, const CallDeclaration*> m_targets;
};
} // namespace angel_lsp::analysis
