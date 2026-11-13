#pragma once

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

struct MemberLookupContext
{
    std::string_view typeName;
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

/**
 * @brief Names the namespace a symbol lives in, looking through its enclosing class or interface.
 * @param[in] sym Symbol whose enclosing namespace is wanted.
 * @param[in] table Symbol table used to find the declaring class of a member.
 * @return Namespace of sym, or of its declaring class when sym is a member; empty at global scope.
 */
std::string EnclosingNamespaceForSymbol(const Symbol& sym, const SymbolTable& table);

/**
 * @brief Zero-copy variant of EnclosingNamespaceForSymbol.
 * @param[in] sym Symbol whose enclosing namespace is wanted.
 * @param[in] table Symbol table used to find the declaring class of a member.
 * @return Namespace of sym or of its declaring class; empty at global scope.
 * @note The view borrows string storage owned by sym or table, so it is valid while both stay unchanged.
 */
std::string_view EnclosingNamespaceView(const Symbol& sym, const SymbolTable& table);
bool IsTemplateParameterOfContainer(std::string_view typeName, const Symbol& sym, const SymbolTable& table);
bool IsKnownType(std::string_view baseName, const DiagnosticContext& ctx, std::string_view containerScope = {});
[[nodiscard]] bool ResolvesToEnum(std::string_view typeName, const SymbolTable& table);
bool IsVariableType(std::string_view typeName);
std::vector<ContainerInfo> GetEnclosingContainers(IndexedByteRange range, const NodeIndex& nodeIndex,
                                                  std::string_view sourceCode);
std::vector<std::string> CollectUsingNamespaces(const NodeIndex& nodeIndex, std::string_view sourceCode);
std::vector<std::string> CollectVisibleUsingNamespaces(uint32_t byteOffset, std::string_view sourceCode,
                                                       const NodeIndex* nodeIndex);
[[nodiscard]] const std::vector<std::string>& GetUsingNamespaces(const SemanticAnalysisRequest& request);
std::vector<Symbol> FindSymbolsInScope(const SymbolTable& symbolTable, const std::vector<ContainerInfo>& containers,
                                       std::string_view name, std::span<const std::string> usingNamespaces = {});
std::vector<Symbol> FindSymbolsInScope(std::string_view name, uint32_t byteOffset, std::string_view sourceCode,
                                       const SymbolTable& symbolTable, const NodeIndex* nodeIndex = nullptr);
std::vector<Symbol> FilterSymbolsByContainerKind(std::vector<Symbol> found, ContainerKind expected);
std::vector<std::pair<uint32_t, uint32_t>> CollectNamespaceAndContainerLineRanges(const SymbolTable& symbolTable,
                                                                                  const std::string& fileUri,
                                                                                  std::string_view declaringNamespace);

bool NamesAFunctionNotAType(std::string_view name, const SymbolTable& table);
std::vector<Symbol> LookupTypeCandidateSymbols(const std::string& name, const SymbolTable& symbolTable);
bool AreAllSymbolsPureTypes(const std::vector<Symbol>& syms);
/// Identifies a bare type used in a value expression, accounting for local shadowing.
[[nodiscard]] std::optional<std::string> IsBareDataType(std::string_view name, uint32_t byteOffset,
                                                        const ExpressionTypeContext& ctx);
} // namespace angel_lsp::analysis
