#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string_view>

namespace angel_lsp::features
{
namespace
{
using analysis::Symbol;
using analysis::SymbolTable;
using analysis::SymbolType;

bool IsTypeSymbol(const Symbol& sym)
{
    return sym.type == SymbolType::Class || sym.type == SymbolType::Interface;
}

/** @brief Every base a declaration lists, whichever kind of declaration it is. */
std::vector<std::string> DeclaredBases(const Symbol& sym)
{
    if (sym.type == SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(sym.signature))
    {
        return sym.GetClass().bases;
    }
    if (sym.type == SymbolType::Interface && std::holds_alternative<analysis::InterfaceSignature>(sym.signature))
    {
        return sym.GetInterface().inheritedInterfaces;
    }
    return {};
}

lsp::Range ToRange(const analysis::SourceRange& range)
{
    return lsp::Range{lsp::Position{range.startLine, range.startCharacter},
                      lsp::Position{range.endLine, range.endCharacter}};
}

lsp::TypeHierarchyItem ToItem(const Symbol& sym)
{
    lsp::TypeHierarchyItem item;
    item.name = analysis::LastScopeSegment(sym.name);
    item.kind = sym.type == SymbolType::Interface ? lsp::SymbolKind::Interface : lsp::SymbolKind::Class;
    item.uri = lsp::DocumentUri::parse(sym.fileUri);
    std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
    item.data = lsp::json::Value(std::move(qName));
    if (!sym.containerName.empty())
    {
        item.detail = sym.containerName;
    }

    // fullRange covers the declaration and its body; selectionRange is the name alone. The
    // protocol requires the second be contained by the first, and a collector that recorded
    // neither would give both as an empty range at the origin - so the symbol's own
    // start/end is the fallback, which is always at least the name.
    const bool hasFullRange = sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0;
    item.range = hasFullRange ? ToRange(sym.fullRange)
                              : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                           lsp::Position{sym.endLine, sym.endCharacter}};

    const bool hasSelection = sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0;
    item.selectionRange = hasSelection ? ToRange(sym.selectionRange)
                                       : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                                    lsp::Position{sym.endLine, sym.endCharacter}};
    return item;
}

/** @brief The type declarations a bare name resolves to, if any. */
std::vector<Symbol> FindTypeDeclarations(const std::string& name, const SymbolTable& table)
{
    std::vector<Symbol> found;
    const auto symbols = table.FindSymbolsPtr(name);
    if (!symbols)
    {
        return found;
    }
    for (const auto& sym : *symbols)
    {
        if (IsTypeSymbol(sym))
        {
            found.push_back(sym);
        }
    }
    return found;
}

/** @brief The identifier the cursor sits on, or empty when it is not on one. */
std::string IdentifierAt(const TypeHierarchyPrepareRequest& request, TSNode& outNode)
{
    outNode = TSNode{};
    if (!request.tree)
    {
        return "";
    }

    const TSNode root = ts_tree_root_node(request.tree);
    const TSPoint point{request.position.line, request.position.character};
    TSNode node = ts_node_descendant_for_point_range(root, point, point);
    if (ts_node_is_null(node))
    {
        return "";
    }

    outNode = node;
    if (std::string_view(ts_node_type(node)) != "identifier")
    {
        return "";
    }

    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > request.sourceCode.size())
    {
        return "";
    }
    return request.sourceCode.substr(start, end - start);
}
} // namespace

std::optional<std::vector<lsp::TypeHierarchyItem>> PrepareTypeHierarchy(const TypeHierarchyPrepareRequest& request)
{
    TSNode node{};
    const std::string name = IdentifierAt(request, node);

    std::vector<Symbol> declarations;

    // Check if cursor sits on or inside a qualified type (e.g. Outer::Widget or Other::Base)
    if (!ts_node_is_null(node))
    {
        TSNode p = node;
        while (!ts_node_is_null(p))
        {
            std::string_view pType = ts_node_type(p);
            if (pType == "type" || pType == "scoped_identifier")
            {
                std::string text = analysis::CleanBaseType(analysis::GetNodeText(p, request.sourceCode));
                if (!text.empty() && text.find("::") != std::string::npos)
                {
                    declarations = FindTypeDeclarations(text, request.symbolTable);
                    if (declarations.empty())
                    {
                        for (const auto& container : analysis::GetEnclosingContainers(p, request.sourceCode))
                        {
                            if (container.kind == analysis::ContainerKind::Namespace)
                            {
                                declarations =
                                    FindTypeDeclarations(container.qualifiedName + "::" + text, request.symbolTable);
                                if (!declarations.empty())
                                {
                                    break;
                                }
                            }
                        }
                    }
                    if (!declarations.empty())
                    {
                        break;
                    }
                }
            }
            if (pType == "class_declaration" || pType == "interface_declaration" || pType == "func_declaration" ||
                pType == "statement_block")
            {
                break;
            }
            p = ts_node_parent(p);
        }
    }

    if (declarations.empty() && !name.empty())
    {
        declarations = FindTypeDeclarations(name, request.symbolTable);
        if (declarations.empty() && !ts_node_is_null(node))
        {
            for (const auto& container : analysis::GetEnclosingContainers(node, request.sourceCode))
            {
                if (container.kind == analysis::ContainerKind::Namespace)
                {
                    declarations = FindTypeDeclarations(container.qualifiedName + "::" + name, request.symbolTable);
                    if (!declarations.empty())
                    {
                        break;
                    }
                }
            }
        }
        if (declarations.empty())
        {
            for (const auto& sym : request.symbolTable.FindTypeSymbolsByShortName(name))
            {
                if (IsTypeSymbol(sym))
                {
                    declarations.push_back(sym);
                }
            }
        }
    }

    // Not on a type's name, but perhaps inside one's body - which is where a reader asking for
    // the hierarchy usually has the cursor.
    if (declarations.empty() && !ts_node_is_null(node))
    {
        for (const auto& container : analysis::GetEnclosingContainers(node, request.sourceCode))
        {
            if (container.kind != analysis::ContainerKind::Class &&
                container.kind != analysis::ContainerKind::Interface)
            {
                continue;
            }
            declarations = FindTypeDeclarations(container.qualifiedName, request.symbolTable);
            if (declarations.empty())
            {
                declarations = FindTypeDeclarations(container.name, request.symbolTable);
            }
            if (declarations.empty())
            {
                for (const auto& sym : request.symbolTable.FindTypeSymbolsByShortName(container.name))
                {
                    if (IsTypeSymbol(sym))
                    {
                        declarations.push_back(sym);
                    }
                }
            }
            break;
        }
    }

    if (declarations.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::TypeHierarchyItem> items;
    items.reserve(declarations.size());
    for (const auto& sym : declarations)
    {
        items.push_back(ToItem(sym));
    }
    return items;
}

std::optional<std::vector<lsp::TypeHierarchyItem>> GetSupertypes(const TypeHierarchyItemRequest& request)
{
    std::vector<Symbol> declarations;
    if (request.item.data.has_value() && request.item.data->isString() && !request.item.data->string().empty())
    {
        declarations = FindTypeDeclarations(request.item.data->string(), request.symbolTable);
    }
    if (declarations.empty())
    {
        declarations = FindTypeDeclarations(request.item.name, request.symbolTable);
    }
    if (declarations.empty())
    {
        for (const auto& sym : request.symbolTable.FindTypeSymbolsByShortName(request.item.name))
        {
            if (IsTypeSymbol(sym))
            {
                declarations.push_back(sym);
            }
        }
    }
    if (declarations.size() > 1 && request.item.uri.isValid())
    {
        const std::string itemUri = request.item.uri.toString();
        auto it = std::find_if(declarations.begin(), declarations.end(),
                               [&](const Symbol& sym)
                               {
                                   if (sym.fileUri != itemUri && angel_lsp::utils::PathToUri(sym.fileUri) != itemUri)
                                   {
                                       return false;
                                   }
                                   auto range =
                                       (sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0)
                                           ? ToRange(sym.selectionRange)
                                       : (sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0)
                                           ? ToRange(sym.fullRange)
                                           : lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                                        lsp::Position{sym.endLine, sym.endCharacter}};
                                   return range.start.line == request.item.selectionRange.start.line;
                               });
        if (it != declarations.end())
        {
            declarations = {*it};
        }
    }
    if (declarations.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::TypeHierarchyItem> items;
    std::vector<std::string> seen;

    for (const auto& declaration : declarations)
    {
        std::string declPrefix;
        auto lastScope = declaration.name.rfind("::");
        if (lastScope != std::string::npos)
        {
            declPrefix = declaration.name.substr(0, lastScope);
        }

        for (const auto& base : DeclaredBases(declaration))
        {
            const std::string cleanBase = analysis::CleanBaseType(base);
            const std::string baseName = analysis::LastScopeSegment(cleanBase);
            if (baseName.empty())
            {
                continue;
            }

            // Look up base types in order:
            // 1. Fully qualified / written base (e.g. "Other::Base")
            // 2. Enclosing namespace + clean base (e.g. "Game::Base")
            // 3. Short name in table
            // 4. Fallback: FindTypeSymbolsByShortName
            std::vector<Symbol> baseSymbols;
            if (cleanBase.find("::") != std::string::npos)
            {
                baseSymbols = FindTypeDeclarations(cleanBase, request.symbolTable);
            }
            if (baseSymbols.empty() && !declPrefix.empty())
            {
                baseSymbols = FindTypeDeclarations(declPrefix + "::" + cleanBase, request.symbolTable);
            }
            if (baseSymbols.empty())
            {
                baseSymbols = FindTypeDeclarations(baseName, request.symbolTable);
            }
            if (baseSymbols.empty())
            {
                for (const auto& sym : request.symbolTable.FindTypeSymbolsByShortName(baseName))
                {
                    if (IsTypeSymbol(sym))
                    {
                        baseSymbols.push_back(sym);
                    }
                }
            }
            for (const auto& baseSymbol : baseSymbols)
            {
                const std::string key = baseSymbol.qualifiedName.empty() ? baseSymbol.name : baseSymbol.qualifiedName;
                if (std::find(seen.begin(), seen.end(), key) == seen.end())
                {
                    seen.push_back(key);
                    items.push_back(ToItem(baseSymbol));
                }
            }
        }
    }
    return items.empty() ? std::nullopt : std::optional{items};
}

std::optional<std::vector<lsp::TypeHierarchyItem>> GetSubtypes(const TypeHierarchyItemRequest& request)
{
    const std::string target = analysis::LastScopeSegment(request.item.name);
    if (target.empty())
    {
        return std::nullopt;
    }

    std::string qualifiedTarget;
    if (request.item.data.has_value() && request.item.data->isString() && !request.item.data->string().empty())
    {
        qualifiedTarget = request.item.data->string();
    }

    std::vector<lsp::TypeHierarchyItem> items;
    const auto ruleIndex = request.symbolTable.GetRuleIndex();
    if (ruleIndex)
    {
        ankerl::unordered_dense::set<std::string> seen;
        auto collectFrom = [&](const std::string& baseKey)
        {
            auto it = ruleIndex->derivedByBase.find(baseKey);
            if (it == ruleIndex->derivedByBase.end())
            {
                return;
            }
            for (const auto& derived : it->second)
            {
                const std::string key = derived.qualifiedName.empty() ? derived.name : derived.qualifiedName;
                if (!seen.insert(key).second)
                {
                    continue;
                }
                const auto symList = request.symbolTable.FindSymbolsPtr(key);
                if (symList)
                {
                    for (const auto& sym : *symList)
                    {
                        if (IsTypeSymbol(sym) && analysis::LastScopeSegment(sym.name) != target)
                        {
                            items.push_back(ToItem(sym));
                        }
                    }
                }
            }
        };

        if (!qualifiedTarget.empty())
        {
            collectFrom(qualifiedTarget);
        }
        collectFrom(target);
        if (request.item.name != target)
        {
            collectFrom(request.item.name);
        }
    }
    else
    {
        request.symbolTable.ForEachSymbol(
            [&](const std::string&, const std::vector<Symbol>& symbols)
            {
                for (const auto& sym : symbols)
                {
                    if (!IsTypeSymbol(sym) || analysis::LastScopeSegment(sym.name) == target)
                    {
                        continue;
                    }

                    for (const auto& base : DeclaredBases(sym))
                    {
                        const std::string cleanBase = analysis::CleanBaseType(base);
                        if ((!qualifiedTarget.empty() && cleanBase == qualifiedTarget) ||
                            analysis::LastScopeSegment(cleanBase) == target)
                        {
                            items.push_back(ToItem(sym));
                            break;
                        }
                    }
                }
            });
    }

    return items.empty() ? std::nullopt : std::optional{items};
}
} // namespace angel_lsp::features
