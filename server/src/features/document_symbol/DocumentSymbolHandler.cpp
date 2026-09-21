#include "features/document_symbol/DocumentSymbolHandler.h"
#include "parser/AngelScriptParser.h"

#include "parser/GrammarNames.h"
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
    if (ts_node_is_null(node))
    {
        return "";
    }
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= sourceCode.size() || end > sourceCode.size() || start >= end)
    {
        return "";
    }
    return std::string(sourceCode.substr(start, end - start));
}

/**
 * @brief Helper to query a child node by its grammar field name.
 * @param node Parent TSNode.
 * @param fieldName Field name in grammar.
 * @return Child TSNode.
 */
inline TSNode GetChildByFieldName(TSNode node, const char* fieldName)
{
    return parser::GetChildByField(node, fieldName);
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
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = lsp::SymbolKind::Namespace;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
    sym.detail = "namespace";

    TSNode bodyNode = GetChildByFieldName(node, "body");
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
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    lsp::DocumentSymbol sym;
    sym.name = name;
    if (nodeType == "interface_declaration")
    {
        sym.kind = lsp::SymbolKind::Interface;
        sym.detail = "interface";
    }
    else
    {
        sym.kind = lsp::SymbolKind::Class;
        sym.detail = (nodeType == "mixin_declaration") ? "mixin" : "class";
    }
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);

    TSNode bodyNode = GetChildByFieldName(node, "body");
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
    std::vector<lsp::DocumentSymbol> members;
    const uint32_t childCount = ts_node_named_child_count(enumNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode child = ts_node_named_child(enumNode, i);
        if (std::string_view(ts_node_type(child)) == "enum_member")
        {
            TSNode mNameNode = GetChildByFieldName(child, "name");
            std::string mName = GetNodeText(mNameNode, sourceCode);
            if (!mName.empty())
            {
                lsp::DocumentSymbol mSym;
                mSym.name = std::move(mName);
                mSym.kind = lsp::SymbolKind::EnumMember;
                mSym.range = ToLspRange(child);
                mSym.selectionRange = ts_node_is_null(mNameNode) ? mSym.range : ToLspRange(mNameNode);

                TSNode valNode = GetChildByFieldName(child, "value");
                if (!ts_node_is_null(valNode))
                {
                    mSym.detail = "= " + GetNodeText(valNode, sourceCode);
                }
                members.push_back(std::move(mSym));
            }
        }
    }
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
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = lsp::SymbolKind::Enum;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
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
    const uint32_t cCount = ts_node_child_count(node);
    for (uint32_t i = 0; i < cCount; ++i)
    {
        TSNode child = ts_node_child(node, i);
        const char* cType = ts_node_type(child);
        if (cType && std::strcmp(cType, "~") == 0)
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
 * @brief Processes a func_declaration AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessFunction(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    const bool isDestructor = IsDestructorNode(node);
    const bool isConstructor = ctx.isInsideClass && name == ctx.enclosingClassName;

    lsp::DocumentSymbol sym;
    if (isDestructor)
    {
        sym.name = "~" + name;
        sym.kind = lsp::SymbolKind::Constructor;
    }
    else if (isConstructor)
    {
        sym.name = name;
        sym.kind = lsp::SymbolKind::Constructor;
    }
    else
    {
        sym.name = name;
        sym.kind = ctx.isInsideClass ? lsp::SymbolKind::Method : lsp::SymbolKind::Function;
    }

    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);

    TSNode retNode = GetChildByFieldName(node, "return_type");
    TSNode paramsNode = GetChildByFieldName(node, "parameters");
    const std::string retStr = GetNodeText(retNode, ctx.sourceCode);
    const std::string paramsStr = GetNodeText(paramsNode, ctx.sourceCode);

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
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    TSNode retNode = GetChildByFieldName(node, "return_type");
    TSNode paramsNode = GetChildByFieldName(node, "parameters");

    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = lsp::SymbolKind::Method;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);

    const std::string retStr = GetNodeText(retNode, ctx.sourceCode);
    const std::string paramsStr = GetNodeText(paramsNode, ctx.sourceCode);
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
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = lsp::SymbolKind::Function;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
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
    TSNode typeNode = GetChildByFieldName(node, "var_type");
    const std::string typeStr = GetNodeText(typeNode, ctx.sourceCode);

    const uint32_t childCount = ts_node_named_child_count(node);
    uint32_t declaratorCount = 0;
    for (uint32_t i = 0; i < childCount; ++i)
    {
        if (std::string_view(ts_node_type(ts_node_named_child(node, i))) == "variable_declarator")
        {
            declaratorCount++;
        }
    }

    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode declChild = ts_node_named_child(node, i);
        if (std::string_view(ts_node_type(declChild)) == "variable_declarator")
        {
            TSNode nameNode = GetChildByFieldName(declChild, "name");
            std::string name = GetNodeText(nameNode, ctx.sourceCode);
            if (!name.empty())
            {
                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = ctx.isInsideClass ? lsp::SymbolKind::Field : lsp::SymbolKind::Variable;
                sym.range = (declaratorCount == 1) ? ToLspRange(node) : ToLspRange(declChild);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                if (!typeStr.empty())
                {
                    sym.detail = typeStr;
                }
                outSymbols.push_back(std::move(sym));
            }
        }
    }
}

/**
 * @brief Processes a virtual_property AST node.
 * @param[in] node AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Symbol list to append to.
 */
void ProcessVirtualProperty(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    TSNode typeNode = GetChildByFieldName(node, "prop_type");
    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = lsp::SymbolKind::Property;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
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
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    TSNode baseTypeNode = GetChildByFieldName(node, "base_type");
    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = lsp::SymbolKind::Class;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
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
    TSNode nameNode = GetChildByFieldName(node, "name");
    std::string name = GetNodeText(nameNode, ctx.sourceCode);
    if (name.empty())
    {
        return;
    }

    TSNode retNode = GetChildByFieldName(node, "return_type");
    TSNode paramsNode = GetChildByFieldName(node, "parameters");

    lsp::DocumentSymbol sym;
    sym.name = std::move(name);
    sym.kind = lsp::SymbolKind::Function;
    sym.range = ToLspRange(node);
    sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
    sym.detail = "funcdef " + GetNodeText(retNode, ctx.sourceCode) + " " + GetNodeText(paramsNode, ctx.sourceCode);

    outSymbols.push_back(std::move(sym));
}

/**
 * @brief Dispatches symbol processing for a single Tree-Sitter AST node based on its type.
 * @param[in] node Current AST node.
 * @param[in] ctx Traversal context.
 * @param[in,out] outSymbols Output accumulator for created symbols.
 */
void ProcessNode(TSNode node, const TraversalContext& ctx, std::vector<lsp::DocumentSymbol>& outSymbols)
{
    if (ts_node_is_null(node))
    {
        return;
    }

    const char* type = ts_node_type(node);
    if (!type)
    {
        return;
    }

    const std::string_view nodeType(type);
    if (nodeType == "namespace_declaration")
    {
        ProcessNamespace(node, ctx, outSymbols);
    }
    else if (nodeType == "class_declaration" || nodeType == "mixin_declaration" || nodeType == "interface_declaration")
    {
        ProcessClassLike(node, nodeType, ctx, outSymbols);
    }
    else if (nodeType == "enum_declaration")
    {
        ProcessEnum(node, ctx, outSymbols);
    }
    else if (nodeType == "func_declaration")
    {
        ProcessFunction(node, ctx, outSymbols);
    }
    else if (nodeType == "interface_method")
    {
        ProcessInterfaceMethod(node, ctx, outSymbols);
    }
    else if (nodeType == "import_declaration")
    {
        ProcessImport(node, ctx, outSymbols);
    }
    else if (nodeType == "variable_declaration")
    {
        ProcessVariable(node, ctx, outSymbols);
    }
    else if (nodeType == "virtual_property")
    {
        ProcessVirtualProperty(node, ctx, outSymbols);
    }
    else if (nodeType == "typedef_declaration")
    {
        ProcessTypedef(node, ctx, outSymbols);
    }
    else if (nodeType == "funcdef_declaration")
    {
        ProcessFuncdef(node, ctx, outSymbols);
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
} // namespace

std::optional<DocumentSymbolResult> GetDocumentSymbols(const DocumentSymbolRequest& request)
{
    if (request.sourceCode.empty())
    {
        return DocumentSymbolResult{};
    }

    TraversalContext ctx{request.sourceCode, false, ""};

    if (request.tree != nullptr)
    {
        TSNode root = ts_tree_root_node(request.tree);
        return ProcessChildren(root, ctx);
    }

    parser::AngelScriptParser parser;
    TSTree* tempTree = parser.Parse(request.sourceCode);
    if (!tempTree)
    {
        return DocumentSymbolResult{};
    }

    TSNode root = ts_tree_root_node(tempTree);
    auto result = ProcessChildren(root, ctx);
    ts_tree_delete(tempTree);
    return result;
}
} // namespace angel_lsp::features
