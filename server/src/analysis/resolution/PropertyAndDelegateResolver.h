#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "config/EngineRuleConfig.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
struct DelegateTargetClassRequest
{
    IndexedByteRange targetRange{};
    const NodeIndex* nodeIndex = nullptr;
    std::string_view sourceCode{};
    const SymbolTable& symbolTable;
    const config::EngineRuleConfig* engineRules = nullptr;
};

struct DelegatePropertyReceiverContext
{
    IndexedByteRange objRange{};
    std::string_view objText{};
    std::string_view effectiveHostClass{};
    std::string_view sourceCode{};
    std::string_view arrayTypeName{};
    const NodeIndex* nodeIndex = nullptr;
};

std::string PropertyNameFromAccessor(const Symbol& sym, bool keywordRequired);
std::vector<Symbol> FindPropertyAccessors(std::string_view typeName, std::string_view propertyName,
                                          const SymbolTable& symbolTable, bool keywordRequired);
std::vector<Symbol> FindGlobalPropertyAccessors(std::string_view propertyName, const SymbolTable& symbolTable,
                                                bool keywordRequired = false);
std::string PropertyTypeFromAccessors(std::span<const Symbol> accessors);
bool IsBaseClassDelegateProperty(std::string_view name, const SymbolTable& symbolTable,
                                 const config::EngineRuleConfig* engineRules = nullptr);
std::string ResolveDelegateTargetBaseClass(const DelegateTargetClassRequest& req);
std::string ResolveDelegatePropertyReceiver(const DelegatePropertyReceiverContext& dctx, const SymbolTable& symbolTable,
                                            const ReceiverTypeContext& ctx);
} // namespace angel_lsp::analysis
