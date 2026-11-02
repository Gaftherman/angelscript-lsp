#include "analysis/mediator/SemanticMediator.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/Constants.h"

#include <algorithm>

namespace angel_lsp::analysis
{

SemanticMediator::SemanticMediator(const SymbolTable& st, const Scope* sc, const NodeIndex* ni, std::string_view src,
                                   std::string_view uri, ExpressionTypeCache* cache,
                                   const config::EngineRuleConfig* rules)
    : m_symbolTable(st), m_scopeRoot(sc), m_nodeIndex(ni), m_sourceCode(src), m_fileUri(uri), m_cache(cache),
      m_engineRules(rules)
{
}

const Scope* SemanticMediator::ScopeAtPoint(uint32_t line, uint32_t col) const noexcept
{
    return m_scopeRoot ? FindInnermostScope(m_scopeRoot, line, col) : nullptr;
}

const Scope* SemanticMediator::ScopeForNode(TSNode node) const noexcept
{
    if (ts_node_is_null(node) || !m_scopeRoot)
    {
        return m_scopeRoot;
    }
    const TSPoint pt = ts_node_start_point(node);
    return FindInnermostScope(m_scopeRoot, pt.row, pt.column);
}

const Scope* SemanticMediator::ScopeAt(uint32_t byte) const noexcept
{
    if (!m_scopeRoot || m_sourceCode.empty() || byte > m_sourceCode.size())
    {
        return m_scopeRoot;
    }
    uint32_t line = 0;
    uint32_t col = 0;
    for (uint32_t i = 0; i < byte; ++i)
    {
        if (m_sourceCode[i] == '\n')
        {
            line++;
            col = 0;
        }
        else
        {
            col++;
        }
    }
    return ScopeAtPoint(line, col);
}

std::string_view SemanticMediator::EnclosingClass(uint32_t byte) const noexcept
{
    if (!m_nodeIndex)
    {
        return {};
    }
    const auto* span = m_nodeIndex->FindInnermostClassSpan(byte);
    if (span && span->nameStartByte < span->nameEndByte && span->nameEndByte <= m_sourceCode.size())
    {
        return m_sourceCode.substr(span->nameStartByte, span->nameEndByte - span->nameStartByte);
    }
    return {};
}

std::string_view SemanticMediator::EnclosingNamespace(uint32_t byte) const noexcept
{
    if (!m_nodeIndex)
    {
        return {};
    }
    const auto containers = m_nodeIndex->CollectEnclosingContainers(byte, m_sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Namespace)
        {
            return c.name;
        }
    }
    return {};
}

std::vector<ContainerInfo> SemanticMediator::EnclosingContainers(uint32_t byte) const
{
    if (m_nodeIndex)
    {
        return m_nodeIndex->CollectEnclosingContainers(byte, m_sourceCode);
    }
    return {};
}

std::span<const std::string> SemanticMediator::UsingNamespaces() const
{
    if (!m_usingNamespacesLoaded)
    {
        if (m_nodeIndex)
        {
            m_usingNamespaces = CollectUsingNamespaces(*m_nodeIndex, m_sourceCode);
        }
        m_usingNamespacesLoaded = true;
    }
    return m_usingNamespaces;
}

std::string SemanticMediator::ResolveExpressionType(TSNode exprNode, const Scope* scope) const
{
    if (ts_node_is_null(exprNode))
    {
        return {};
    }
    const Scope* activeScope = scope ? scope : ScopeForNode(exprNode);
    ExpressionTypeContext ctx(activeScope, m_symbolTable, m_sourceCode, m_fileUri);
    ctx.cache = m_cache;
    ctx.nodeIndex = m_nodeIndex;
    return analysis::ResolveExpressionType(exprNode, ctx);
}

std::string SemanticMediator::ResolveReceiverType(TSNode memberOrMethodNode, const Scope* scope) const
{
    if (ts_node_is_null(memberOrMethodNode))
    {
        return {};
    }
    const Scope* activeScope = scope ? scope : ScopeForNode(memberOrMethodNode);
    ReceiverTypeContext ctx;
    ctx.scope = activeScope;
    ctx.fileUri = m_fileUri;
    ctx.cache = m_cache;
    ctx.engineRules = m_engineRules;
    return analysis::ResolveReceiverType(memberOrMethodNode, m_sourceCode, m_symbolTable, ctx);
}

std::string SemanticMediator::SubstituteTemplateType(std::string_view rawType, std::string_view receiverType) const
{
    if (rawType.empty() || receiverType.empty())
    {
        return std::string(rawType);
    }
    const auto binding = BindTemplateArguments(std::string(receiverType), m_symbolTable);
    if (!binding.usable)
    {
        return std::string(rawType);
    }
    return SubstituteTemplateParameters(rawType, binding);
}

const Symbol* SemanticMediator::LookupSymbolInScope(std::string_view name, uint32_t byte) const
{
    if (name.empty())
    {
        return nullptr;
    }
    if (const Scope* scope = ScopeAt(byte))
    {
        if (const auto* def = ResolveInScopeAt(scope, name, byte))
        {
            if (auto syms = m_symbolTable.FindSymbolsPtr(name))
            {
                for (const auto& sym : *syms)
                {
                    if (sym.fileUri == m_fileUri && sym.startLine == def->startLine &&
                        sym.startCharacter == def->startCharacter)
                    {
                        return &sym;
                    }
                }
            }
        }
    }
    if (auto syms = m_symbolTable.FindSymbolsPtr(name))
    {
        return syms->empty() ? nullptr : &syms->front();
    }
    return nullptr;
}

AccessModifier SemanticMediator::ExtractSymbolAccess(const Symbol& sym) noexcept
{
    if (std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return sym.GetFunction().modifiers.access;
    }
    if (std::holds_alternative<VariableSignature>(sym.signature))
    {
        return sym.GetVariable().modifiers.access;
    }
    if (std::holds_alternative<ClassSignature>(sym.signature))
    {
        return sym.GetClass().modifiers.access;
    }
    if (std::holds_alternative<InterfaceSignature>(sym.signature))
    {
        return sym.GetInterface().modifiers.access;
    }
    if (std::holds_alternative<EnumSignature>(sym.signature))
    {
        return sym.GetEnum().modifiers.access;
    }
    return AccessModifier::Public;
}

MemberLookupResult SemanticMediator::LookupClassMember(std::string_view className, std::string_view memberName,
                                                       bool accessorRequired) const
{
    MemberLookupResult result;
    if (className.empty() || memberName.empty())
    {
        return result;
    }

    const auto& hierarchy = GetClassHierarchy(className);
    for (const auto& owner : hierarchy.hierarchy)
    {
        if (const auto syms = m_symbolTable.FindMemberSymbolPtr(owner, memberName))
        {
            for (const auto& sym : *syms)
            {
                result.found = true;
                result.declaringClass = owner;
                result.access = ExtractSymbolAccess(sym);
                if (sym.type == SymbolType::Function && sym.GetFunction().modifiers.isConst)
                {
                    result.hasConstOverload = true;
                }
                return result;
            }
        }
        if (!accessorRequired)
        {
            const auto propPair = FindPropertyAccessors(owner, memberName, false);
            if (!propPair.IsEmpty())
            {
                result.found = true;
                result.viaAccessor = true;
                result.declaringClass = owner;
                result.access = ExtractSymbolAccess(propPair.accessors.front());
                return result;
            }
        }
    }
    return result;
}

const ClassHierarchyInfo& SemanticMediator::GetClassHierarchy(std::string_view className) const
{
    const std::string key(className);
    auto it = m_hierarchyCache.find(key);
    if (it != m_hierarchyCache.end())
    {
        return it->second;
    }

    ClassHierarchyInfo info;
    info.className = key;
    info.hierarchy = GetInheritedTypeHierarchy(key, m_symbolTable, m_cache);
    info.baseClass = ResolveBaseClass(key, m_symbolTable);

    auto [insertedIt, _] = m_hierarchyCache.emplace(key, std::move(info));
    return insertedIt->second;
}

PropertyAccessorPair SemanticMediator::FindPropertyAccessors(std::string_view className, std::string_view propName,
                                                             bool keywordRequired) const
{
    PropertyAccessorPair pair;
    if (className.empty() || propName.empty())
    {
        return pair;
    }
    pair.accessors =
        analysis::FindPropertyAccessors(std::string(className), std::string(propName), m_symbolTable, keywordRequired);
    return pair;
}

CallResolutionContext SemanticMediator::ExtractCallContext(TSNode callNode, const Scope* scope) const
{
    CallResolutionContext result;
    if (ts_node_is_null(callNode))
    {
        return result;
    }

    TSNode argList = ResolveArgumentListNode(callNode);
    if (ts_node_is_null(argList))
    {
        return result;
    }

    const auto argInfos = ExtractCallArguments(argList, m_sourceCode);
    const size_t count = argInfos.size();
    result.argNodes.reserve(count);
    result.argNames.reserve(count);
    result.argTypes.reserve(count);
    result.argIsLValue.reserve(count);

    for (const auto& info : argInfos)
    {
        result.argNodes.push_back(info.exprNode);
        result.argNames.push_back(info.name);
        result.argTypes.push_back(ResolveExpressionType(info.exprNode, scope));
        result.argIsLValue.push_back(false);
    }

    return result;
}

} // namespace angel_lsp::analysis
