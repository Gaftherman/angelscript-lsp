#pragma once

#include "analysis/index/SymbolTable.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "config/EngineRuleConfig.h"
#include <ankerl/unordered_dense.h>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
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
    const std::string& cleanClass;
    const SymbolTable& symbolTable;
    ankerl::unordered_dense::set<std::string>& visited;
    std::vector<std::string>& result;
};

NonInstantiableKind ClassifyNonInstantiable(std::string_view baseTypeName, const SymbolTable& table);
bool IsMixinClass(std::string_view baseTypeName, const SymbolTable& table);
std::vector<std::string> GetInheritedTypeHierarchy(const std::string& className, const SymbolTable& symbolTable,
                                                   ExpressionTypeCache* cache = nullptr);
std::string ResolveBaseClass(std::string_view className, const SymbolTable& symbolTable);
bool IsDerivedFrom(std::string_view derivedClass, std::string_view baseClass, const SymbolTable& symbolTable);
std::vector<std::string> ResolveLinkedBaseClasses(std::string_view className, const SymbolTable& symbolTable,
                                                  const LinkedBaseOptions& options = {});
std::vector<Symbol> FindOverriddenBaseMethodSymbols(std::string_view containerClass, std::string_view methodName,
                                                    const SymbolTable& symbolTable,
                                                    const LinkedBaseOptions& options = {});
void ExpandMixinHostHierarchies(std::string_view containerClass, const SymbolTable& symbolTable,
                                std::vector<std::string>& hierarchy);
bool HierarchyIsFullyVisible(const std::string& typeName, const SymbolTable& symbolTable);
std::vector<std::string> GetAllRelatedClasses(const std::string& className, const SymbolTable& symbolTable);
std::vector<std::string> GetDerivedClasses(const std::string& className, const SymbolTable& symbolTable);
std::vector<std::string> GetCompatibleMemberClasses(const std::string& className, const std::string& memberName,
                                                    AccessModifier access, const SymbolTable& symbolTable);
std::string InferHostClassFromEnclosingMixin(TSNode node, std::string_view sourceCode, const SymbolTable& symbolTable);
TSNode FindMixinInclusionNode(TSNode hostNode, std::string_view mixinName, std::string_view mixinShortName,
                              std::string_view sourceCode);
} // namespace angel_lsp::analysis
