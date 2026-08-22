#include "features/definition/DefinitionHandler.h"
#include "analysis/SemanticHelpers.h"
#include "utils/Utils.h"
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        bool IsInsideScope(const analysis::Scope &scope, uint32_t line, uint32_t character)
        {
            if (line < scope.startLine || line > scope.endLine)
            {
                return false;
            }
            if (line == scope.startLine && character < scope.startCharacter)
            {
                return false;
            }
            if (line == scope.endLine && character > scope.endCharacter)
            {
                return false;
            }
            return true;
        }

        const analysis::Scope *FindInnermostScope(const analysis::Scope *root, uint32_t line, uint32_t character)
        {
            if (!root || !IsInsideScope(*root, line, character))
            {
                return nullptr;
            }

            const analysis::Scope *current = root;
            bool foundChild = true;
            while (foundChild)
            {
                foundChild = false;
                for (const auto &child : current->children)
                {
                    if (child && IsInsideScope(*child, line, character))
                    {
                        current = child.get();
                        foundChild = true;
                        break;
                    }
                }
            }
            return current;
        }

        std::string GetNodeTextAt(const DefinitionRequest &request, TSNode &outNode)
        {
            if (!request.tree || request.sourceCode.empty())
            {
                return "";
            }

            TSNode rootNode = ts_tree_root_node(request.tree);
            TSPoint point = { request.position.line, request.position.character };
            TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

            if (ts_node_is_null(node))
            {
                return "";
            }

            std::string_view nodeType = ts_node_type(node);
            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                if (request.position.character > 0)
                {
                    TSPoint prevPoint = { request.position.line, request.position.character - 1 };
                    TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPoint, prevPoint);
                    if (!ts_node_is_null(prevNode))
                    {
                        std::string_view prevType = ts_node_type(prevNode);
                        if (prevType == "identifier" || prevType == "primitive_type" || prevType == "scoped_identifier")
                        {
                            node = prevNode;
                            nodeType = prevType;
                        }
                    }
                }
            }

            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                return "";
            }

            uint32_t startByte = ts_node_start_byte(node);
            uint32_t endByte = ts_node_end_byte(node);
            if (startByte >= request.sourceCode.size() || endByte > request.sourceCode.size() || startByte >= endByte)
            {
                return "";
            }

            outNode = node;
            return request.sourceCode.substr(startByte, endByte - startByte);
        }
    }

    std::optional<std::vector<lsp::Location>> GetDefinition(const DefinitionRequest &request)
    {
        TSNode node{};
        std::string nodeText = GetNodeTextAt(request, node);
        if (nodeText.empty() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        std::vector<lsp::Location> locations;

        // 1. Member Access vs Local Scope Precedence
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        TSNode parent = ts_node_parent(node);

        // Check if cursor node is the member child of a member_expression (e.g. "prop" in "obj.prop")
        bool isMemberChildOfExpression = false;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
        {
            TSNode memNode = ts_node_child_by_field_name(parent, "member", 6);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
            {
                isMemberChildOfExpression = true;
            }
        }

        auto resolveMemberDefinition = [&]() -> std::optional<std::vector<lsp::Location>>
        {
            if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
            {
                return std::nullopt;
            }

            TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
            if (ts_node_is_null(objectNode))
            {
                return std::nullopt;
            }

            uint32_t objStart = ts_node_start_byte(objectNode);
            uint32_t objEnd = ts_node_end_byte(objectNode);
            if (objStart >= request.sourceCode.size() || objEnd > request.sourceCode.size() || objStart >= objEnd)
            {
                return std::nullopt;
            }

            std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);
            std::string receiverTypeName;

            if (objText == "this")
            {
                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type == analysis::SymbolType::Class && sym.fileUri == request.uri)
                        {
                            if (request.position.line >= sym.startLine && request.position.line <= sym.endLine)
                            {
                                receiverTypeName = sym.name;
                            }
                        }
                    }
                });
            }
            else if (rootScope)
            {
                const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
                if (scope)
                {
                    const analysis::LocalDefinition *objDef = analysis::ResolveInScope(scope, objText);
                    if (objDef && !objDef->typeName.empty())
                    {
                        receiverTypeName = analysis::CleanBaseType(objDef->typeName);
                    }
                }
            }

            if (receiverTypeName.empty())
            {
                auto globSyms = request.symbolTable.FindSymbols(objText);
                for (const auto &sym : globSyms)
                {
                    if (sym.type == analysis::SymbolType::Variable)
                    {
                        const auto &var = sym.GetVariable();
                        if (!var.typeName.empty())
                        {
                            receiverTypeName = analysis::CleanBaseType(var.typeName);
                            break;
                        }
                    }
                }
            }

            if (!receiverTypeName.empty())
            {
                std::vector<analysis::Symbol> memberSymbols;
                auto hierarchy = analysis::GetInheritedTypeHierarchy(receiverTypeName, request.symbolTable);
                for (const auto &typeName : hierarchy)
                {
                    std::string qualifiedMember = typeName + "::" + nodeText;
                    auto found = request.symbolTable.FindSymbols(qualifiedMember);
                    if (!found.empty())
                    {
                        memberSymbols = std::move(found);
                        break;
                    }
                }

                std::vector<lsp::Location> memLocations;
                for (const auto &sym : memberSymbols)
                {
                    if (sym.type != analysis::SymbolType::CallReference)
                    {
                        memLocations.push_back(lsp::Location{
                            lsp::DocumentUri::parse(sym.fileUri),
                            lsp::Range{
                                lsp::Position{ sym.startLine, sym.startCharacter },
                                lsp::Position{ sym.endLine, sym.endCharacter }
                            }
                        });
                    }
                }

                if (!memLocations.empty())
                {
                    return memLocations;
                }
            }

            return std::nullopt;
        };

        // If cursor is on the member child of obj.prop, member resolution takes precedence over local scope!
        if (isMemberChildOfExpression)
        {
            auto memberLocs = resolveMemberDefinition();
            if (memberLocs.has_value())
            {
                return memberLocs;
            }
        }

        // 2. Local Scope Definition (Parameters and Variables in Function Scope)
        if (rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def && (def->kind == analysis::LocalDefinitionKind::Parameter ||
                            def->kind == analysis::LocalDefinitionKind::Variable))
                {
                    locations.push_back(lsp::Location{
                        lsp::DocumentUri::parse(request.uri),
                        lsp::Range{
                            lsp::Position{ def->startLine, def->startCharacter },
                            lsp::Position{ def->endLine, def->endCharacter }
                        }
                    });
                    return locations;
                }
            }
        }

        // 3. Fallback Member Access Resolution if not already resolved
        if (!isMemberChildOfExpression)
        {
            auto memberLocs = resolveMemberDefinition();
            if (memberLocs.has_value())
            {
                return memberLocs;
            }
        }

        // 4. Container / Scoped / Global Symbol Lookup
        auto symbols = analysis::FindSymbolsInScope(nodeText, node, request.sourceCode, request.symbolTable);
        if (symbols.empty())
        {
            if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "scoped_identifier")
            {
                uint32_t pStart = ts_node_start_byte(parent);
                uint32_t pEnd = ts_node_end_byte(parent);
                if (pStart < request.sourceCode.size() && pEnd <= request.sourceCode.size())
                {
                    std::string scopedText = request.sourceCode.substr(pStart, pEnd - pStart);
                    symbols = analysis::FindSymbolsInScope(scopedText, node, request.sourceCode, request.symbolTable);
                }
            }
        }

        // Fallback to local scope definition (e.g. Field or non-function scope definition) if not in SymbolTable
        if (symbols.empty() && rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def)
                {
                    locations.push_back(lsp::Location{
                        lsp::DocumentUri::parse(request.uri),
                        lsp::Range{
                            lsp::Position{ def->startLine, def->startCharacter },
                            lsp::Position{ def->endLine, def->endCharacter }
                        }
                    });
                    return locations;
                }
            }
        }

        for (const auto &sym : symbols)
        {
            if (sym.type != analysis::SymbolType::CallReference)
            {
                locations.push_back(lsp::Location{
                    lsp::DocumentUri::parse(sym.fileUri),
                    lsp::Range{
                        lsp::Position{ sym.startLine, sym.startCharacter },
                        lsp::Position{ sym.endLine, sym.endCharacter }
                    }
                });
            }
        }

        if (!locations.empty())
        {
            return locations;
        }

        return std::nullopt;
    }

    std::optional<std::vector<lsp::Location>> GetTypeDefinition(const DefinitionRequest &request)
    {
        TSNode node{};
        std::string nodeText = GetNodeTextAt(request, node);
        if (nodeText.empty() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        std::string typeNameToFind;

        // 1. Check local scope
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        if (rootScope)
        {
            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (scope)
            {
                const analysis::LocalDefinition *def = analysis::ResolveInScope(scope, nodeText);
                if (def && !def->typeName.empty())
                {
                    typeNameToFind = analysis::CleanBaseType(def->typeName);
                }
            }
        }

        // 2. Check global symbols
        if (typeNameToFind.empty())
        {
            auto symbols = request.symbolTable.FindSymbols(nodeText);
            for (const auto &sym : symbols)
            {
                if (sym.type == analysis::SymbolType::Variable)
                {
                    const auto &var = sym.GetVariable();
                    if (!var.typeName.empty())
                    {
                        typeNameToFind = analysis::CleanBaseType(var.typeName);
                        break;
                    }
                }
                else if (sym.type == analysis::SymbolType::Function)
                {
                    const auto &fn = sym.GetFunction();
                    if (!fn.returnType.empty())
                    {
                        typeNameToFind = analysis::CleanBaseType(fn.returnType);
                        break;
                    }
                }
                else if (sym.type == analysis::SymbolType::Class ||
                         sym.type == analysis::SymbolType::Interface ||
                         sym.type == analysis::SymbolType::Enum ||
                         sym.type == analysis::SymbolType::Typedef ||
                         sym.type == analysis::SymbolType::Funcdef)
                {
                    typeNameToFind = sym.name;
                    break;
                }
            }
        }

        // 3. Fallback: maybe nodeText itself is a type name (e.g. Player in Player@ p)
        if (typeNameToFind.empty())
        {
            typeNameToFind = analysis::CleanBaseType(nodeText);
        }

        if (typeNameToFind.empty() || analysis::IsPrimitiveTypeName(typeNameToFind))
        {
            return std::nullopt;
        }

        std::vector<lsp::Location> locations;
        auto typeSymbols = request.symbolTable.FindSymbols(typeNameToFind);

        for (const auto &sym : typeSymbols)
        {
            if (sym.type == analysis::SymbolType::Class ||
                sym.type == analysis::SymbolType::Interface ||
                sym.type == analysis::SymbolType::Enum ||
                sym.type == analysis::SymbolType::Typedef ||
                sym.type == analysis::SymbolType::Funcdef)
            {
                locations.push_back(lsp::Location{
                    lsp::DocumentUri::parse(sym.fileUri),
                    lsp::Range{
                        lsp::Position{ sym.startLine, sym.startCharacter },
                        lsp::Position{ sym.endLine, sym.endCharacter }
                    }
                });
            }
        }

        if (!locations.empty())
        {
            return locations;
        }

        return std::nullopt;
    }
}
