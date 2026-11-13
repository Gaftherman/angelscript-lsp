#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "config/EngineRuleConfig.h"
#include "utils/TransparentHash.h"

#include <ankerl/unordered_dense.h>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
enum class NonInstantiableKind
{
    None,
    Abstract,
    Interface,
    Mixin
};

struct HierarchyClassifiedBase
{
    std::string name;
    bool isMixin = false;
};

struct LinkedBaseOptions
{
    std::span<const std::string> delegateProps = {};
    const config::StringVectorMap* proxyMap = nullptr;

    [[nodiscard]] bool HasActiveOptions() const noexcept
    {
        return (proxyMap && !proxyMap->empty()) || !delegateProps.empty();
    }
};

struct MixinExpansionContext
{
    std::string_view cleanClass;
    const SymbolTable& symbolTable;
    ankerl::unordered_dense::set<std::string>& visited;
    std::vector<std::string>& result;
};

bool IsDestructorSymbol(const Symbol& sym);
bool IsDestructorDeclaration(const Symbol& sym, std::string_view source);
bool IsDestructorDeclaration(const Symbol& sym, const DiagnosticContext& ctx);

NonInstantiableKind ClassifyNonInstantiable(std::string_view baseTypeName, const SymbolTable& table);
bool IsMixinClass(std::string_view baseTypeName, const SymbolTable& table);
std::vector<std::string> GetInheritedTypeHierarchy(std::string_view className, const SymbolTable& symbolTable,
                                                   ExpressionTypeCache* cache = nullptr);

/**
 * @brief Per-request memo of inherited type hierarchies that exposes owner names as string views.
 * @note Not thread-safe; owns the hierarchy strings so pass-2 callers never hold owned names.
 */
class InheritedHierarchyCache
{
  public:
    /**
     * @brief Visits className and every type it inherits from until the visitor accepts one.
     * @param[in] className Qualified class name whose hierarchy is visited.
     * @param[in] symbolTable Symbol table used to build the hierarchy on first request.
     * @param[in] visit Callable taking std::string_view and returning true to stop the walk.
     * @return True when visit returned true for some owner; false after the whole hierarchy.
     */
    template <typename Visitor>
    bool AnyInheritedType(std::string_view className, const SymbolTable& symbolTable, Visitor&& visit)
    {
        auto hit = m_hierarchies.find(className);
        if (hit == m_hierarchies.end())
        {
            hit = m_hierarchies.emplace(className, GetInheritedTypeHierarchy(className, symbolTable)).first;
        }
        for (const std::string& owner : hit->second)
        {
            if (visit(std::string_view(owner)))
            {
                return true;
            }
        }
        return false;
    }

  private:
    ankerl::unordered_dense::map<std::string, std::vector<std::string>, TransparentStringHash, std::equal_to<>>
        m_hierarchies;
};
std::string ResolveBaseClass(std::string_view className, const SymbolTable& symbolTable);
bool IsDerivedFrom(std::string_view derivedClass, std::string_view baseClass, const SymbolTable& symbolTable);
std::vector<std::string> ResolveLinkedBaseClasses(std::string_view className, const SymbolTable& symbolTable,
                                                  const LinkedBaseOptions& options = {});
std::vector<Symbol> FindOverriddenBaseMethodSymbols(std::string_view containerClass, std::string_view methodName,
                                                    const SymbolTable& symbolTable,
                                                    const LinkedBaseOptions& options = {});
void ExpandMixinHostHierarchies(std::string_view containerClass, const SymbolTable& symbolTable,
                                std::vector<std::string>& hierarchy, ExpressionTypeCache* cache = nullptr);
bool HierarchyIsFullyVisible(std::string_view typeName, const SymbolTable& symbolTable);
std::vector<std::string> GetAllRelatedClasses(std::string_view className, const SymbolTable& symbolTable);
std::vector<std::string> GetDerivedClasses(std::string_view className, const SymbolTable& symbolTable);
std::vector<std::string> GetCompatibleMemberClasses(std::string_view className, std::string_view memberName,
                                                    AccessModifier access, const SymbolTable& symbolTable);
/**
 * @brief Infers the host class of the mixin enclosing a byte range, answering the container lookup from the index.
 * @param[in] targetRange Byte range of the node whose enclosing containers are inspected.
 * @param[in] index Document index; its container spans give the enclosing containers.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] symbolTable Symbol table providing the rule index of mixin hosts.
 * @return Qualified name of the first recorded host class; empty when no enclosing mixin has a host.
 */
std::string InferHostClassFromEnclosingMixin(IndexedByteRange targetRange, const NodeIndex& index,
                                             std::string_view sourceCode, const SymbolTable& symbolTable);
std::string InferHostClassFromEnclosingMixin(IndexedByteRange targetRange, const NodeIndex* index,
                                             std::string_view sourceCode, const SymbolTable& symbolTable);
} // namespace angel_lsp::analysis
