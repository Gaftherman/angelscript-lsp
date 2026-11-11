#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/AccessorNames.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Resolves member type in class type hierarchy.
 * @param[in] table Symbol table.
 * @param[in] parentType Name of parent class/container type.
 * @param[in] memberName Target member name.
 * @return Resolved raw type name of variable or return type of function, or empty string.
 */
static std::string ExtractMemberTypeFromSymbols(const std::vector<analysis::Symbol>& syms)
{
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().typeName.empty())
        {
            return sym.GetVariable().typeName;
        }
        if (sym.type == analysis::SymbolType::Function && !sym.GetFunction().returnType.empty())
        {
            return sym.GetFunction().returnType;
        }
    }
    return "";
}

static std::string FindFunctionReturnType(const analysis::SymbolTable& table, std::string_view name)
{
    if (auto syms = table.FindSymbolsPtr(name))
    {
        for (const auto& sym : *syms)
        {
            if (sym.type == analysis::SymbolType::Function && !sym.GetFunction().returnType.empty())
            {
                return sym.GetFunction().returnType;
            }
        }
    }
    return "";
}

static std::string FindMemberInTypeHierarchy(const analysis::SymbolTable& table, const std::string& parentType,
                                             std::string_view memberName)
{
    if (parentType.empty() || memberName.empty())
    {
        return "";
    }
    char getterBuf[128];
    const std::string_view getterView = utils::ComposeGetterName(memberName, getterBuf);

    for (const auto& cls : GetInheritedTypeHierarchy(table, parentType))
    {
        if (auto syms = table.FindMemberSymbolPtr(cls, memberName))
        {
            if (std::string type = ExtractMemberTypeFromSymbols(*syms); !type.empty())
            {
                return type;
            }
        }
        if (!getterView.empty())
        {
            if (auto syms = table.FindMemberSymbolPtr(cls, getterView))
            {
                if (std::string type = ExtractMemberTypeFromSymbols(*syms); !type.empty())
                {
                    return type;
                }
            }
        }
    }
    return "";
}

static std::string ResolveScopeOrGlobalVariableType(std::string_view nodeText, const CompletionQuery& request,
                                                    const analysis::Scope* innermostScope)
{
    if (innermostScope)
    {
        const analysis::LocalDefinition* def = analysis::ResolveInScope(innermostScope, nodeText);
        if (def)
        {
            if (!def->typeName.empty() && analysis::CleanBaseTypeView(def->typeName) != "auto")
            {
                return def->typeName;
            }
            std::string deduced = analysis::ResolveLocalDefinitionType(
                *def, {innermostScope, request.symbolTable, request.sourceCode, request.uri, request.nodeIndex});
            return !deduced.empty() ? deduced : def->typeName;
        }
    }
    if (auto syms = request.symbolTable.FindSymbolsPtr(nodeText))
    {
        for (const auto& sym : *syms)
        {
            if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().typeName.empty())
            {
                return sym.GetVariable().typeName;
            }
        }
    }
    return "";
}

std::string ResolveNamedReceiverType(std::string_view nodeText, const CompletionQuery& request,
                                     const analysis::Scope* innermostScope)
{
    if (nodeText.empty())
    {
        return "";
    }
    if (nodeText == parser::keywords::This)
    {
        return FindEnclosingClassName(request);
    }
    if (nodeText.starts_with("this."))
    {
        return ResolveNamedReceiverType(nodeText.substr(5), request, innermostScope);
    }
    if (std::string varType = ResolveScopeOrGlobalVariableType(nodeText, request, innermostScope); !varType.empty())
    {
        return varType;
    }
    std::string memberHit = FindMemberInTypeHierarchy(request.symbolTable, FindEnclosingClassName(request), nodeText);
    if (!memberHit.empty())
    {
        return memberHit;
    }
    char getterBuf[128];
    if (const std::string_view getterView = utils::ComposeGetterName(nodeText, getterBuf); !getterView.empty())
    {
        if (std::string ret = FindFunctionReturnType(request.symbolTable, getterView); !ret.empty())
        {
            return ret;
        }
    }
    return FindFunctionReturnType(request.symbolTable, nodeText);
}

/**
 * @brief Resolves the type of the receiver written in a byte range, by name first and then from its syntax.
 * @param[in] receiverRange Byte range of the receiver expression.
 * @param[in] request Completion request.
 * @param[in] innermostScope Innermost lexical scope at cursor.
 * @return Resolved raw type name string, or empty string.
 */
static std::string ResolveReceiverRangeType(IndexedByteRange receiverRange, const CompletionQuery& request,
                                            const analysis::Scope* innermostScope)
{
    const std::string_view nodeText =
        request.sourceCode.substr(receiverRange.startByte, receiverRange.endByte - receiverRange.startByte);
    if (std::string named = ResolveNamedReceiverType(nodeText, request, innermostScope); !named.empty())
    {
        return named;
    }
    return analysis::ResolveCompletionReceiverType(
        receiverRange, {innermostScope, request.symbolTable, request.sourceCode, request.uri, request.nodeIndex});
}

/**
 * @brief Resolves fallback receiver type when AST node resolution is unavailable or incomplete.
 * @param[in] prefix Prefix text before cursor.
 * @param[in] dotCol Column offset of access dot.
 * @param[in] collector Completion collector context.
 * @param[in] innermostScope Lexical scope at cursor.
 * @return Resolved raw type name, or empty string.
 */
static std::string ResolveFallbackReceiverType(const std::string& prefix, size_t dotCol,
                                               const CompletionCollector& collector,
                                               const analysis::Scope* innermostScope)
{
    size_t s = SkipWhitespaceAndCommentsBackward(prefix, dotCol);
    auto peel = PeelIndexBracketsWithExpr(prefix, s);
    PeelCallParentheses(prefix, s);
    s = SkipWhitespaceAndCommentsBackward(prefix, s);
    size_t e = s;
    s = ExtractIdentifierBackward(prefix, s);
    if (e <= s)
    {
        return "";
    }
    std::string ident(prefix.substr(s, e - s));
    std::string rawTypeName;
    std::string parentType;
    size_t prevDot = SkipWhitespaceAndCommentsBackward(prefix, s);
    if (prevDot > 0 && prefix[prevDot - 1] == '.')
    {
        parentType = ResolveFallbackReceiverType(prefix, prevDot - 1, collector, innermostScope);
        rawTypeName = FindMemberInTypeHierarchy(collector.request.symbolTable, parentType, ident);
    }
    else
    {
        rawTypeName = ResolveNamedReceiverType(ident, collector.request, innermostScope);
    }
    if (rawTypeName.empty())
    {
        return "";
    }

    std::string enclosingClass = FindEnclosingClassName(collector.request);
    size_t indexCount = peel.count;
    if (indexCount > 0 && IsIndexedPropertyAccessor(collector.request.symbolTable, parentType, ident, enclosingClass))
    {
        --indexCount;
    }
    if (indexCount > 0)
    {
        std::string indexArgType = DeduceLiteralOrNamedType(peel.lastIndexExpr, collector.request, innermostScope);
        const auto& arrayContainer = collector.request.symbolTable.GetDefaultArrayType();
        if (indexCount == 1)
        {
            rawTypeName = analysis::ResolveIndexedTypeWithArg(rawTypeName, collector.request.symbolTable,
                                                              arrayContainer, indexArgType);
        }
        else
        {
            rawTypeName =
                analysis::ResolveIndexedType(rawTypeName, indexCount, collector.request.symbolTable, arrayContainer);
        }
    }
    return rawTypeName;
}

bool TryCompleteMemberAccess(const std::string& prefix, const analysis::Scope* innermostScope,
                             CompletionCollector& collector)
{
    auto dotCol = FindAccessDotCol(prefix);
    if (!dotCol)
    {
        return false;
    }

    std::string rawTypeName;
    if (collector.request.nodeIndex)
    {
        size_t lineStart = utils::LineStartOffset(collector.request.sourceCode, collector.request.position.line);
        const auto dotByteOffset = static_cast<uint32_t>(lineStart + *dotCol);
        if (const auto receiverRange = analysis::FindMemberReceiverRange(*collector.request.nodeIndex,
                                                                         collector.request.sourceCode, dotByteOffset))
        {
            rawTypeName = ResolveReceiverRangeType(*receiverRange, collector.request, innermostScope);
        }
    }

    if (rawTypeName.empty() || rawTypeName.find("<poison>") != std::string::npos)
    {
        rawTypeName = ResolveFallbackReceiverType(prefix, *dotCol, collector, innermostScope);
        if (rawTypeName.empty() && collector.request.nodeIndex)
        {
            size_t lineStart = utils::LineStartOffset(collector.request.sourceCode, collector.request.position.line);
            const auto dotByteOffset = static_cast<uint32_t>(lineStart + *dotCol);
            std::string sourcePrefix(collector.request.sourceCode.substr(0, dotByteOffset + 1));
            rawTypeName = ResolveFallbackReceiverType(sourcePrefix, dotByteOffset, collector, innermostScope);
        }
    }
    else if (analysis::IsCorePrimitive(rawTypeName))
    {
        std::string fallback = ResolveFallbackReceiverType(prefix, *dotCol, collector, innermostScope);
        if (!fallback.empty() && !analysis::IsCorePrimitive(fallback) && fallback.find("<poison>") == std::string::npos)
        {
            rawTypeName = std::move(fallback);
        }
    }

    if (rawTypeName.empty() || analysis::IsCorePrimitive(rawTypeName) || rawTypeName == "void")
    {
        return true;
    }

    std::string_view memberPrefix =
        (*dotCol + 1 < prefix.size()) ? std::string_view(prefix).substr(*dotCol + 1) : "";
    PopulateHierarchicalMembers(rawTypeName, memberPrefix, collector);
    return true;
}

} // namespace angel_lsp::analysis::completion
