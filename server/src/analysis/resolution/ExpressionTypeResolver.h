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

    ExpressionTypeContext(const Scope* sc, const SymbolTable& st, std::string_view src, std::string_view u = "")
        : scope(sc), symbolTable(st), sourceCode(src), uri(u), stringTypeName(st.GetStringType()),
          arrayTypeName(st.GetDefaultArrayType())
    {
    }

    /**
     * @brief Constructs an expression context resolving symbols directly with an optional node index.
     * @param[in] sc Innermost lexical scope at query position.
     * @param[in] st Symbol table for type and method resolution.
     * @param[in] src Document source text buffer.
     * @param[in] u Document file URI.
     * @param[in] ni Optional document node index.
     */
    ExpressionTypeContext(const Scope* sc, const SymbolTable& st, std::string_view src, std::string_view u,
                          const NodeIndex* ni)
        : scope(sc), symbolTable(st), sourceCode(src), uri(u), stringTypeName(st.GetStringType()),
          arrayTypeName(st.GetDefaultArrayType()), nodeIndex(ni)
    {
    }

    ExpressionTypeContext(const Scope* sc, const DiagnosticContext& diagCtx)
        : scope(sc), symbolTable(diagCtx.request.symbolTable), sourceCode(diagCtx.request.sourceCode),
          uri(diagCtx.request.fileUri), stringTypeName(diagCtx.request.GetEffectiveStringTypeName()),
          arrayTypeName(diagCtx.request.GetEffectiveArrayTypeName()),
          disableIntegerDivision(diagCtx.request.DisablesIntegerDivision()), cache(diagCtx.request.exprCache),
          nodeIndex(diagCtx.request.nodeIndex)
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
    std::string_view t1;
    std::string_view t2;
    std::string_view c1;
    std::string_view c2;
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
/**
 * @brief Resolves the type of the expression that spans the given byte range.
 * @param[in] expressionRange Byte span of the expression.
 * @param[in] ctx Semantic expression context.
 * @return Resolved type name or empty string.
 */
std::string ResolveExpressionTypeAt(IndexedByteRange expressionRange, const ExpressionTypeContext& ctx);

inline std::string ResolveExpressionTypeAt(uint32_t startByte, uint32_t endByte, const ExpressionTypeContext& ctx)
{
    return ResolveExpressionTypeAt(IndexedByteRange{startByte, endByte}, ctx);
}

/**
 * @brief Resolves the type of a local definition.
 * @param[in] def Local definition to resolve.
 * @param[in] ctx Semantic expression context.
 * @return Resolved type name or empty string.
 */
std::string ResolveLocalDefinitionType(const LocalDefinition& def, const ExpressionTypeContext& ctx);

/**
 * @brief Resolves receiver expression type across class hierarchy and member chains.
 * @param[in] objRange Byte span of the receiver expression.
 * @param[in] sourceCode Document source text.
 * @param[in] symbolTable Symbol table.
 * @param[in] ctx Receiver type resolution context.
 * @return Resolved receiver type name or empty string.
 */
std::string ResolveReceiverType(IndexedByteRange objRange, std::string_view sourceCode, const SymbolTable& symbolTable,
                                const ReceiverTypeContext& ctx = {});
} // namespace angel_lsp::analysis
