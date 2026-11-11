#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "config/EngineRuleConfig.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

struct ExpressionTypeContext
{
    const Scope* scope = nullptr;
    const SymbolTable& symbolTable;
    std::string_view sourceCode;
    std::string_view uri = "";
    std::string_view stringTypeName = {};
    std::string_view arrayTypeName = {};
    bool disableIntegerDivision = false;
    ExpressionTypeCache* cache = nullptr;
    const NodeIndex* nodeIndex = nullptr;
    TSNode rootNode = {};

    ExpressionTypeContext(const Scope* sc, const SymbolTable& st, std::string_view src, std::string_view u = "",
                          TSNode root = {})
        : scope(sc), symbolTable(st), sourceCode(src), uri(u), stringTypeName(st.GetStringType()),
          arrayTypeName(st.GetDefaultArrayType()), rootNode(root)
    {
    }

    /**
     * @brief Constructs an expression context resolving the AST root directly from an optional node index.
     * @param[in] sc Innermost lexical scope at query position.
     * @param[in] st Symbol table for type and method resolution.
     * @param[in] src Document source text buffer.
     * @param[in] u Document file URI.
     * @param[in] ni Optional document node index supplying the tree root.
     */
    ExpressionTypeContext(const Scope* sc, const SymbolTable& st, std::string_view src, std::string_view u,
                          const NodeIndex* ni)
        : scope(sc), symbolTable(st), sourceCode(src), uri(u), stringTypeName(st.GetStringType()),
          arrayTypeName(st.GetDefaultArrayType()), nodeIndex(ni), rootNode(ni ? ni->Root() : TSNode{})
    {
    }

    ExpressionTypeContext(const Scope* sc, const DiagnosticContext& diagCtx)
        : scope(sc), symbolTable(diagCtx.request.symbolTable), sourceCode(diagCtx.request.sourceCode),
          uri(diagCtx.request.fileUri), stringTypeName(diagCtx.request.GetEffectiveStringTypeName()),
          arrayTypeName(diagCtx.request.GetEffectiveArrayTypeName()),
          disableIntegerDivision(diagCtx.request.DisablesIntegerDivision()), cache(diagCtx.request.exprCache),
          nodeIndex(diagCtx.request.nodeIndex),
          rootNode(diagCtx.request.tree ? ts_tree_root_node(diagCtx.request.tree) : TSNode{})
    {
    }
};

struct ReceiverTypeContext
{
    const Scope* scope = nullptr;
    std::string_view virtualHostClass = {};
    std::string_view fileUri = {};
    std::string_view arrayTypeName = {};
    ExpressionTypeCache* cache = nullptr;
    const config::EngineRuleConfig* engineRules = nullptr;
    std::span<const std::string> delegateProps = {};
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Query parameters for member search across class and mixin hierarchies.
 */
struct MemberHierarchyQuery
{
    std::string_view hostClass;
    std::string_view memberName;
    std::string_view arrayTypeName;
    ExpressionTypeCache* cache = nullptr;
    const NodeIndex* nodeIndex = nullptr;
};

struct TernaryBranchTypes
{
    std::string t1;
    std::string t2;
    std::string c1;
    std::string c2;
};

struct ActiveLocalDefKey
{
    std::string_view name;
    uint32_t line = 0;
    uint32_t col = 0;
};

struct ActiveLocalDefGuard
{
    bool pushed = false;
    ~ActiveLocalDefGuard();
};

inline std::string ResolveReceiverMemberType(std::string_view receiverType, std::string_view memberName,
                                             const SymbolTable& table)
{
    auto memberSyms = table.FindMemberSymbolPtr(receiverType, memberName);
    if (memberSyms && !memberSyms->empty())
    {
        const auto& m = memberSyms->front();
        if (m.type == SymbolType::Variable && !m.GetVariable().typeName.empty())
        {
            return CleanBaseType(m.GetVariable().typeName);
        }
        if (m.type == SymbolType::Function && !m.GetFunction().returnType.empty())
        {
            return CleanBaseType(m.GetFunction().returnType);
        }
    }
    return {};
}
std::string ResolveExpressionType(TSNode exprNode, const ExpressionTypeContext& ctx);
inline std::string ResolveExpressionType(TSNode exprNode, const Scope* scope, const SymbolTable& symbolTable,
                                         std::string_view sourceCode)
{
    return ResolveExpressionType(exprNode, ExpressionTypeContext{scope, symbolTable, sourceCode, ""});
}
std::string ResolveExpressionTypeAt(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx);
inline std::string ResolveExpressionTypeAt(uint32_t startByte, uint32_t endByte, const ExpressionTypeContext& ctx)
{
    return ResolveExpressionTypeAt(IndexedByteRange{startByte, endByte}, ctx);
}
std::string ResolveLocalDefinitionType(const LocalDefinition& def, const ExpressionTypeContext& ctx);
std::string ResolveReceiverType(IndexedByteRange objRange, std::string_view sourceCode, const SymbolTable& symbolTable,
                                const ReceiverTypeContext& ctx = {});
std::string ResolveReceiverType(TSNode objNode, std::string_view sourceCode, const SymbolTable& symbolTable,
                                const ReceiverTypeContext& ctx = {});
} // namespace angel_lsp::analysis
