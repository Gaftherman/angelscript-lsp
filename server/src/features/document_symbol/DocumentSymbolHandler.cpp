#include "features/document_symbol/DocumentSymbolHandler.h"
#include "document/Document.h"
#include "parser/ASTUtils.h"
#include "parser/AngelScriptParser.h"

#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
namespace
{
/**
 * @brief Context for document symbol AST traversal.
 */
struct TraversalContext
{
    std::string_view sourceCode;
    bool isInsideClass = false;
    std::string_view enclosingClassName;
};

/**
 * @brief Converts a Tree-Sitter TSNode range to an LSP Range.
 * @param node Tree-Sitter AST node.
 * @return Enclosing LSP Range.
 */
inline lsp::Range ToLspRange(TSNode node)
{
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);
    return lsp::Range{lsp::Position{start.row, start.column}, lsp::Position{end.row, end.column}};
}

/**
 * @brief Extracts text content from source code for a given AST node.
 * @param node Target TSNode.
 * @param sourceCode Full document source text.
 * @return Extracted string slice.
 */
inline std::string GetNodeText(TSNode node, std::string_view sourceCode)
{
    return parser::GetNodeText(node, sourceCode);
}

/**
 * @brief Helper to query a child node by its grammar field name.
 * @param node Parent TSNode.
 * @param fieldName Field name in grammar.
 * @return Child TSNode.
 */
inline TSNode GetChildByField(TSNode node, TSFieldId fieldId) noexcept
{
    return parser::GetChildByField(node, fieldId);
}

/**
 * @brief Constructs a DocumentSymbol with range and selectionRange initialized from AST nodes.
 * @param[in] name Symbol identifier name.
 * @param[in] kind LSP SymbolKind category.
 * @param[in] node AST enclosing node for the entire symbol range.
 * @param[in] nameNode AST name identifier node for the selection range.
 * @return Initialized DocumentSymbol.
 */
inline lsp::DocumentSymbol MakeDocumentSymbol(std::string name, lsp::SymbolKind kind, TSNode node, TSNode nameNode)
{
    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = kind;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
    return sym;
}

/**
 * @brief Constructs a DocumentSymbol for a declaration having an identifier field.
 * @param[in] node AST enclosing node for the declaration.
 * @param[in] kind LSP SymbolKind category.
 * @param[in] ctx Traversal context.
 * @param[in] fieldName Optional grammar field name for the identifier (default "name").
 * @param[out] outNameNode Optional output pointer to receive the name TSNode.
 * @return Initialized DocumentSymbol if name is non-empty; nullopt otherwise.
 */
inline std::optional<lsp::DocumentSymbol> CreateDeclarationSymbol(TSNode node, lsp::SymbolKind kind,
                                                                  const TraversalContext& ctx, TSFieldId fieldId = 0)
{
    if (fieldId == 0)
    {
        fieldId = parser::GetGrammarSymbols().fieldName;
    }
    TSNode nameNode = GetChildByField(node, fieldId);
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return std::nullopt;
    }
    return MakeDocumentSymbol(std::move(name), kind, node, nameNode);
}

std::vector<lsp::DocumentSymbol> ProcessChildren(TSNode containerNode, const TraversalContext& ctx);
void ProcessNode(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols);

/**
 * @brief Processes a namespace_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessNamespace(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto symOpt = CreateDeclarationSymbol(node, lsp::SymbolKind::Namespace, ctx);
    if (!symOpt)
    {
        return;
    }

    auto sym = std::move(*symOpt);
    sym.detail = "namespace";

    TSNode bodyNode = GetChildByField(node, parser::GetGrammarSymbols().fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        TraversalContext bodyCtx{ctx.sourceCode, false, ""};
        auto children = ProcessChildren(bodyNode, bodyCtx);
        if (!children.empty())
        {
            sym.children = std::move(children);
        }
    }

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Processes class, mixin, or interface declaration AST nodes.
 * @param[in] node AST node.
 * @param[in] nodeType Grammar node type string.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessClassLike(TSNode node, std::string_view nodeType, const TraversalContext& ctx,
                      std::vector<lsp::DocumentSymbol>& outSymbols)
{
    const bool isInterface = (nodeType == "interface_declaration");
    const auto kind = isInterface ? lsp::SymbolKind::Interface : lsp::SymbolKind::Class;
    auto symOpt = CreateDeclarationSymbol(node, kind, ctx);
    if (!symOpt)
    {
        return;
    }

    auto sym = std::move(*symOpt);
    const std::string name = sym.name;
    sym.detail = isInterface ? "interface" : ((nodeType == "mixin_declaration") ? "mixin" : "class");

    TSNode bodyNode = GetChildByField(node, parser::GetGrammarSymbols().fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        TraversalContext bodyCtx{ctx.sourceCode, true, name};
        auto children = ProcessChildren(bodyNode, bodyCtx);
        if (!children.empty())
        {
            sym.children = std::move(children);
        }
    }

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Collects enum member symbols from an enum_declaration node.
 * @param[in] enumNode Enum AST node.
 * @param[in] sourceCode Source text.
 * @return Vector of enum member document symbols.
 */
std::vector<lsp::DocumentSymbol> CollectEnumMembers(TSNode enumNode, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<lsp::DocumentSymbol> members;
    parser::ForEachNamedChildNode(enumNode,
                                  [&](TSNode child)
                                  {
                                      if (ts_node_symbol(child) == gs.symEnumMember)
                                      {
                                          TSNode mNameNode = GetChildByField(child, gs.fieldName);
                                          std::string mName = GetNodeText(mNameNode, sourceCode);
                                          if (!mName.empty())
                                          {
                                              auto mSym = MakeDocumentSymbol(
                                                  std::move(mName), lsp::SymbolKind::EnumMember, child, mNameNode);

                                              TSNode valNode = GetChildByField(child, gs.fieldValue);
                                              if (!ts_node_is_null(valNode))
                                              {
                                                  mSym.detail = "= " + GetNodeText(valNode, sourceCode);
                                              }
                                              members.push_back(std::move(mSym));
                                          }
                                      }
                                  });
    return members;
}

/**
 * @brief Processes an enum_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessEnum(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto symOpt = CreateDeclarationSymbol(node, lsp::SymbolKind::Enum, ctx);
    if (!symOpt)
    {
        return;
    }

    auto sym = std::move(*symOpt);
    sym.detail = "enum";

    auto members = CollectEnumMembers(node, ctx.sourceCode);
    if (!members.empty())
    {
        sym.children = std::move(members);
    }

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Checks if a function declaration is a destructor node.
 * @param[in] node AST node.
 * @return True if node contains a tilde token child.
 */
bool IsDestructorNode(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t cCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < cCount; ++i)
    {
        TSNode child = ts_node_child(node, i);
        if (ts_node_symbol(child) == gs.tokTilde)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Builds the detail string for a function or method symbol.
 * @param[in] retStr Return type string.
 * @param[in] paramsStr Parameters string.
 * @param[in] isCtorOrDtor True if constructor or destructor.
 * @return Formatted detail string.
 */
std::string BuildFunctionDetail(std::string_view retStr, std::string_view paramsStr, bool isCtorOrDtor)
{
    if (!retStr.empty() && !paramsStr.empty())
    {
        return std::string(retStr) + " " + std::string(paramsStr);
    }
    if (!retStr.empty())
    {
        return std::string(retStr);
    }
    if (isCtorOrDtor)
    {
        return std::string(paramsStr);
    }
    return "";
}

/**
 * @brief Extracted components of a function or method AST node.
 */
struct FunctionComponents
{
    TSNode nameNode;
    std::string name;
    TSNode retNode;
    TSNode paramsNode;
};

/**
 * @brief Extracts standard function components (name, return type, parameters) from an AST node.
 * @param[in] node AST node.
 * @param[in] sourceCode Document text.
 * @return Extracted components if a non-empty name is present.
 */
std::optional<FunctionComponents> ExtractFunctionComponents(TSNode node, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode nameNode = GetChildByField(node, gs.fieldName);
    std::string name = GetNodeText(nameNode, sourceCode);
    if (name.empty())
    {
        return std::nullopt;
    }
    TSNode retNode = GetChildByField(node, gs.fieldReturnType);
    TSNode paramsNode = GetChildByField(node, gs.fieldParameters);
    return FunctionComponents{nameNode, std::move(name), retNode, paramsNode};
}

/**
 * @brief Processes a func_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessFunction(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto comps = ExtractFunctionComponents(node, ctx.sourceCode);
    if (!comps)
    {
        return;
    }

    const bool isDestructor = IsDestructorNode(node);
    const bool isConstructor = ctx.isInsideClass && comps->name == ctx.enclosingClassName;

    const auto kind = (isDestructor || isConstructor)
                          ? lsp::SymbolKind::Constructor
                          : (ctx.isInsideClass ? lsp::SymbolKind::Method : lsp::SymbolKind::Function);
    std::string symName = isDestructor ? ("~" + comps->name) : std::move(comps->name);
    auto sym = MakeDocumentSymbol(std::move(symName), kind, node, comps->nameNode);

    const std::string retStr = GetNodeText(comps->retNode, ctx.sourceCode);
    const std::string paramsStr = GetNodeText(comps->paramsNode, ctx.sourceCode);

    sym.detail = BuildFunctionDetail(retStr, paramsStr, isDestructor || isConstructor);
    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Processes an interface_method AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessInterfaceMethod(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto comps = ExtractFunctionComponents(node, ctx.sourceCode);
    if (!comps)
    {
        return;
    }

    auto sym = MakeDocumentSymbol(std::move(comps->name), lsp::SymbolKind::Method, node, comps->nameNode);

    const std::string retStr = GetNodeText(comps->retNode, ctx.sourceCode);
    const std::string paramsStr = GetNodeText(comps->paramsNode, ctx.sourceCode);
    sym.detail = BuildFunctionDetail(retStr, paramsStr, false);

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Processes an import_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessImport(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto symOpt = CreateDeclarationSymbol(node, lsp::SymbolKind::Function, ctx);
    if (!symOpt)
    {
        return;
    }

    auto sym = std::move(*symOpt);
    sym.detail = "import";

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Processes a variable_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessVariable(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode typeNode = GetChildByField(node, gs.fieldVarType);
    const std::string typeStr = GetNodeText(typeNode, ctx.sourceCode);

    uint32_t declaratorCount = 0;
    parser::ForEachNamedChildNode(node,
                                  [&](TSNode child)
                                  {
                                      if (ts_node_symbol(child) == gs.symVariableDeclarator)
                                      {
                                          declaratorCount++;
                                      }
                                  });

    const auto kind = ctx.isInsideClass ? lsp::SymbolKind::Field : lsp::SymbolKind::Variable;
    parser::ForEachNamedChildNode(node,
                                  [&](TSNode declChild)
                                  {
                                      if (ts_node_symbol(declChild) == gs.symVariableDeclarator)
                                      {
                                          auto symOpt = CreateDeclarationSymbol(declChild, kind, ctx, gs.fieldName);
                                          if (symOpt)
                                          {
                                              auto sym = std::move(*symOpt);
                                              if (declaratorCount == 1)
                                              {
                                                  sym.range = ToLspRange(node);
                                              }
                                              if (!typeStr.empty())
                                              {
                                                  sym.detail = typeStr;
                                              }
                                              outSymbols.push_back(std::move(sym));
                                          }
                                      }
                                  });
}

/**
 * @brief Processes a virtual_property AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessVirtualProperty(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto symOpt = CreateDeclarationSymbol(node, lsp::SymbolKind::Property, ctx);
    if (!symOpt)
    {
        return;
    }

    auto sym = std::move(*symOpt);
    TSNode typeNode = GetChildByField(node, parser::GetGrammarSymbols().fieldPropType);
    sym.detail = GetNodeText(typeNode, ctx.sourceCode);

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Processes a typedef_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessTypedef(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto symOpt = CreateDeclarationSymbol(node, lsp::SymbolKind::Class, ctx);
    if (!symOpt)
    {
        return;
    }

    auto sym = std::move(*symOpt);
    TSNode baseTypeNode = GetChildByField(node, parser::GetGrammarSymbols().fieldBaseType);
    sym.detail = "typedef " + GetNodeText(baseTypeNode, ctx.sourceCode);

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Processes a funcdef_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessFuncdef(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    auto comps = ExtractFunctionComponents(node, ctx.sourceCode);
    if (!comps)
    {
        return;
    }

    auto sym = MakeDocumentSymbol(std::move(comps->name), lsp::SymbolKind::Function, node, comps->nameNode);
    sym.detail =
        "funcdef " + GetNodeText(comps->retNode, ctx.sourceCode) + " " + GetNodeText(comps->paramsNode, ctx.sourceCode);

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Dispatches symbol processing for a single Tree-Sitter AST node based on its type.
 * @param[in] node Current AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Output accumulator for created symbols.
 */
bool ProcessStructuralNode(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symNamespaceDeclaration)
    {
        ProcessNamespace(node, ctx, outSymbols);
        return true;
    }
    if (sym == gs.symClassDeclaration || sym == gs.symMixinDeclaration || sym == gs.symInterfaceDeclaration)
    {
        const std::string_view nodeType = (sym == gs.symClassDeclaration)   ? "class_declaration"
                                          : (sym == gs.symMixinDeclaration) ? "mixin_declaration"
                                                                            : "interface_declaration";
        ProcessClassLike(node, nodeType, ctx, outSymbols);
        return true;
    }
    if (sym == gs.symEnumDeclaration)
    {
        ProcessEnum(node, ctx, outSymbols);
        return true;
    }
    return false;
}

bool ProcessCallableNode(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symFuncDeclaration)
    {
        ProcessFunction(node, ctx, outSymbols);
        return true;
    }
    if (sym == gs.symInterfaceMethod)
    {
        ProcessInterfaceMethod(node, ctx, outSymbols);
        return true;
    }
    if (sym == gs.symFuncdefDeclaration)
    {
        ProcessFuncdef(node, ctx, outSymbols);
        return true;
    }
    return false;
}

void ProcessNode(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    if (ts_node_is_null(node))
    {
        return;
    }

    if (ProcessStructuralNode(node, ctx, outSymbols) || ProcessCallableNode(node, ctx, outSymbols))
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symVariableDeclaration)
    {
        ProcessVariable(node, ctx, outSymbols);
    }
    else if (sym == gs.symVirtualProperty)
    {
        ProcessVirtualProperty(node, ctx, outSymbols);
    }
    else if (sym == gs.symTypedefDeclaration)
    {
        ProcessTypedef(node, ctx, outSymbols);
    }
    else if (sym == gs.symImportDeclaration)
    {
        ProcessImport(node, ctx, outSymbols);
    }
}

/**
 * @brief Iterates child nodes of a container node (e.g. translation unit, namespace_body, class_body)
 *        and produces a vector of DocumentSymbol objects.
 * @param[in] containerNode Container AST node.
 * @param[in] ctx Traversal context.
 * @return Vector of extracted child DocumentSymbol instances.
 */
std::vector<lsp::DocumentSymbol> ProcessChildren(TSNode containerNode, const TraversalContext& ctx)
{
    std::vector<lsp::DocumentSymbol> symbols;
    uint32_t count = ts_node_named_child_count(containerNode);
    symbols.reserve(count);

    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(containerNode, i);
        ProcessNode(child, ctx, symbols);
    }

    return symbols;
}

inline lsp::SymbolKind MapSymbolTypeToKind(analysis::SymbolType type) noexcept
{
    switch (type)
    {
    case analysis::SymbolType::Class:
        return lsp::SymbolKind::Class;
    case analysis::SymbolType::Interface:
        return lsp::SymbolKind::Interface;
    case analysis::SymbolType::Enum:
        return lsp::SymbolKind::Enum;
    case analysis::SymbolType::Namespace:
        return lsp::SymbolKind::Namespace;
    case analysis::SymbolType::Funcdef:
    case analysis::SymbolType::Typedef:
    case analysis::SymbolType::Function:
        return lsp::SymbolKind::Function;
    case analysis::SymbolType::Property:
        return lsp::SymbolKind::Property;
    default:
        return lsp::SymbolKind::Variable;
    }
}
} // namespace

std::optional<DocumentSymbolResult> GetDocumentSymbols(const DocumentSymbolRequest& request)
{
    if (request.sourceCode.empty())
    {
        return DocumentSymbolResult{};
    }

    TraversalContext ctx{request.sourceCode, false, ""};
    DocumentSymbolResult symbols;

    if (request.tree != nullptr)
    {
        TSNode root = ts_tree_root_node(request.tree);
        symbols = ProcessChildren(root, ctx);
    }
    else
    {
        parser::AngelScriptParser parser;
        document::TreePtr tempTree = document::MakeTreePtr(parser.Parse(request.sourceCode));
        if (tempTree)
        {
            TSNode root = ts_tree_root_node(tempTree.get());
            symbols = ProcessChildren(root, ctx);
        }
    }

    if (request.tree != nullptr && ts_node_has_error(ts_tree_root_node(request.tree)))
    {
        ankerl::unordered_dense::set<std::string> existingNames;
        existingNames.reserve(symbols.size());
        for (const auto& s : symbols)
        {
            existingNames.insert(s.name);
        }

        request.symbolTable.ForEachSymbolInFile(
            request.uri,
            [&]([[maybe_unused]] const std::string& key, const std::vector<analysis::Symbol>& bucket)
            {
                for (const auto& sym : bucket)
                {
                    if (!sym.isSynthesized && !existingNames.contains(sym.name))
                    {
                        lsp::DocumentSymbol ds;
                        ds.name = sym.name;
                        ds.kind = MapSymbolTypeToKind(sym.type);
                        ds.range.start.line = sym.startLine;
                        ds.range.start.character = sym.startCharacter;
                        ds.range.end.line = sym.endLine;
                        ds.range.end.character = sym.endCharacter;
                        ds.selectionRange = ds.range;
                        symbols.push_back(std::move(ds));
                        existingNames.insert(sym.name);
                    }
                }
            });
    }

    return symbols;
}
} // namespace angel_lsp::features
