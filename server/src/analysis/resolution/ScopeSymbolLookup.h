#pragma once

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ClassHierarchyResolver.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

struct MemberLookupContext
{
    std::string typeName;
    const SymbolTable& table;
    bool accessorKeywordRequired = false;
};

struct MemberLookupResult
{
    bool found = false;
    bool decided = false;
    bool hasConstOverload = false;
    bool returnsSelfReference = false;
    bool viaAccessor = false;
    AccessModifier access = AccessModifier::Public;
    std::string declaringClass;
};

std::string EnclosingNamespaceForSymbol(const Symbol& sym, const SymbolTable& table);
bool IsTemplateParameterOfContainer(std::string_view typeName, const Symbol& sym, const SymbolTable& table);
bool IsKnownType(std::string_view baseName, const DiagnosticContext& ctx, std::string_view containerScope = {});
[[nodiscard]] bool ResolvesToEnum(std::string_view typeName, const SymbolTable& table);
bool IsVariableType(std::string_view typeName);
std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode,
                                                  const NodeIndex* nodeIndex = nullptr);
std::vector<std::string> CollectUsingNamespaces(const NodeIndex& nodeIndex, std::string_view sourceCode);
std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode);
[[nodiscard]] const std::vector<std::string>& GetUsingNamespaces(const SemanticAnalysisRequest& request);
bool IsKnownScope(const std::string& prefix, TSNode node, std::string_view sourceCode, const SymbolTable& table,
                  std::span<const std::string> precomputedUsings = {});
std::vector<Symbol> FindSymbolsInScope(const SymbolTable& symbolTable, const std::vector<ContainerInfo>& containers,
                                       const std::string& name, const std::vector<std::string>& usingNamespaces = {});
std::vector<Symbol> FindSymbolsInScope(const std::string& name, TSNode node, std::string_view sourceCode,
                                       const SymbolTable& symbolTable);
std::vector<Symbol> FilterSymbolsByContainerKind(std::vector<Symbol> found, ContainerKind expected);
std::vector<std::pair<uint32_t, uint32_t>> CollectNamespaceAndContainerLineRanges(const SymbolTable& symbolTable,
                                                                                  const std::string& fileUri,
                                                                                  std::string_view declaringNamespace);

bool NamesAFunctionNotAType(std::string_view name, const SymbolTable& table);
std::vector<Symbol> LookupTypeCandidateSymbols(const std::string& name, const SymbolTable& symbolTable);
bool AreAllSymbolsPureTypes(const std::vector<Symbol>& syms);
std::optional<std::string> IsBareDataType(TSNode node, const Scope* scope, const SymbolTable& symbolTable,
                                          std::string_view sourceCode);
std::string ExtractDatatypeScopePrefix(TSNode datatypeNode, std::string_view sourceCode);
} // namespace angel_lsp::analysis
