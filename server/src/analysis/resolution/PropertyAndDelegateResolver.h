#pragma once

#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "config/EngineRuleConfig.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
struct DelegateTargetClassRequest
{
    TSNode node;
    std::string_view sourceCode;
    const SymbolTable& symbolTable;
    std::string_view virtualHostClass = {};
    const config::EngineRuleConfig* engineRules = nullptr;
    const NodeIndex* nodeIndex = nullptr;
};

struct DelegatePropertyReceiverContext
{
    TSNode objNode;
    std::string_view objText;
    std::string_view effectiveHostClass;
    std::string_view sourceCode;
    std::string_view arrayTypeName;
    const NodeIndex* nodeIndex = nullptr;
};

std::string PropertyNameFromAccessor(const Symbol& sym, bool keywordRequired);
std::vector<Symbol> FindPropertyAccessors(std::string_view typeName, std::string_view propertyName,
                                          const SymbolTable& symbolTable, bool keywordRequired);
std::vector<Symbol> FindGlobalPropertyAccessors(std::string_view propertyName, const SymbolTable& symbolTable,
                                                bool keywordRequired = false);
std::string PropertyTypeFromAccessors(const std::vector<Symbol>& accessors);
bool IsBaseClassDelegateProperty(std::string_view name, const SymbolTable& symbolTable,
                                 const config::EngineRuleConfig* engineRules = nullptr);
std::string ResolveDelegateTargetBaseClass(const DelegateTargetClassRequest& req);
std::string ResolveDelegatePropertyReceiver(const DelegatePropertyReceiverContext& dctx, const SymbolTable& symbolTable,
                                            const ReceiverTypeContext& ctx);
} // namespace angel_lsp::analysis
