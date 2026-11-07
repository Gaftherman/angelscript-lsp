#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "config/EngineRuleConfig.h"
#include "utils/AccessorNames.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Matched getter and setter accessor symbols for a class property.
 */
struct PropertyAccessorPair
{
    std::vector<Symbol> accessors;

    [[nodiscard]] bool HasGetter() const noexcept
    {
        for (const auto& sym : accessors)
        {
            if (utils::IsGetterName(sym.name))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool HasSetter() const noexcept
    {
        for (const auto& sym : accessors)
        {
            if (utils::IsSetterName(sym.name))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return accessors.empty();
    }
};

/**
 * @brief Memoized inheritance hierarchy and base class metadata.
 */
struct ClassHierarchyInfo
{
    std::string className;
    std::vector<std::string> hierarchy;
    std::string baseClass;
};

/**
 * @brief Central mediator coordinating AST index, scopes, symbols, and resolution passes.
 */
class SemanticMediator
{
  public:
    /**
     * @brief Constructs a mediator binding symbol table, scopes, node index, and caches.
     * @param[in] st Active symbol table.
     * @param[in] sc Document scope tree root.
     * @param[in] ni Pre-computed AST node index.
     * @param[in] src Document source buffer.
     * @param[in] uri Document file URI.
     * @param[in] cache Optional expression type memoization cache.
     * @param[in] rules Optional engine rule configuration.
     */
    SemanticMediator(const SymbolTable& st, const Scope* sc, const NodeIndex* ni, std::string_view src,
                     std::string_view uri = "", ExpressionTypeCache* cache = nullptr,
                     const config::EngineRuleConfig* rules = nullptr);

    ~SemanticMediator() = default;
    SemanticMediator(const SemanticMediator&) = delete;
    SemanticMediator& operator=(const SemanticMediator&) = delete;
    SemanticMediator(SemanticMediator&&) noexcept = default;
    SemanticMediator& operator=(SemanticMediator&&) noexcept = default;

    // --- Context & Container Queries (O(1)) ---
    [[nodiscard]] const Scope* ScopeAt(uint32_t byte) const noexcept;
    [[nodiscard]] const Scope* ScopeAtPoint(uint32_t line, uint32_t col) const noexcept;
    [[nodiscard]] const Scope* ScopeForNode(TSNode node) const noexcept;
    [[nodiscard]] std::string_view EnclosingClass(uint32_t byte) const noexcept;
    [[nodiscard]] std::string_view EnclosingNamespace(uint32_t byte) const noexcept;
    [[nodiscard]] std::vector<ContainerInfo> EnclosingContainers(uint32_t byte) const;
    [[nodiscard]] std::span<const std::string> UsingNamespaces() const;

    // --- Type & Expression Queries ---
    [[nodiscard]] std::string ResolveExpressionType(TSNode exprNode, const Scope* scope = nullptr) const;
    [[nodiscard]] std::string ResolveReceiverType(TSNode memberOrMethodNode, const Scope* scope = nullptr) const;
    [[nodiscard]] std::string SubstituteTemplateType(std::string_view rawType, std::string_view receiverType) const;

    // --- Symbol, Member & Hierarchy Queries ---
    [[nodiscard]] const Symbol* LookupSymbolInScope(std::string_view name, uint32_t byte) const;
    [[nodiscard]] MemberLookupResult LookupClassMember(std::string_view className, std::string_view memberName,
                                                       bool accessorRequired = false) const;
    [[nodiscard]] const ClassHierarchyInfo& GetClassHierarchy(std::string_view className) const;
    [[nodiscard]] PropertyAccessorPair FindPropertyAccessors(std::string_view className, std::string_view propName,
                                                             bool keywordRequired = false) const;

    // --- Call & Overload Queries ---
    [[nodiscard]] CallResolutionContext ExtractCallContext(TSNode callNode, const Scope* scope = nullptr) const;

    // --- Direct Accessors ---
    [[nodiscard]] const SymbolTable& GetSymbolTable() const noexcept
    {
        return m_symbolTable;
    }
    [[nodiscard]] const Scope* GetScopeRoot() const noexcept
    {
        return m_scopeRoot;
    }
    [[nodiscard]] const NodeIndex* GetNodeIndex() const noexcept
    {
        return m_nodeIndex;
    }
    [[nodiscard]] std::string_view GetSourceCode() const noexcept
    {
        return m_sourceCode;
    }
    [[nodiscard]] std::string_view GetFileUri() const noexcept
    {
        return m_fileUri;
    }
    [[nodiscard]] ExpressionTypeCache* GetCache() const noexcept
    {
        return m_cache;
    }

  private:
    const SymbolTable& m_symbolTable;
    const Scope* m_scopeRoot = nullptr;
    const NodeIndex* m_nodeIndex = nullptr;
    std::string_view m_sourceCode;
    std::string_view m_fileUri;
    ExpressionTypeCache* m_cache = nullptr;
    const config::EngineRuleConfig* m_engineRules = nullptr;

    mutable std::vector<std::string> m_usingNamespaces;
    mutable bool m_usingNamespacesLoaded = false;
    mutable ankerl::unordered_dense::map<std::string, ClassHierarchyInfo> m_hierarchyCache;

    /**
     * @brief Resolves declaration accessibility across polymorphic symbol signature variants.
     * @param[in] sym Candidate symbol descriptor.
     * @return Declared access modifier, defaulting to Public.
     */
    [[nodiscard]] static AccessModifier ExtractSymbolAccess(const Symbol& sym) noexcept;
};

} // namespace angel_lsp::analysis
