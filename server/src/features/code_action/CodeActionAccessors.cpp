#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Cleans property name by stripping m_ or _ prefix and capitalizing the first character.
 * @param[in] fieldName Member variable name.
 * @return Cleaned property name suitable for accessor generation.
 */
std::string CleanPropertyName(std::string_view fieldName)
{
    std::string prop(fieldName);
    if (prop.starts_with("m_") && prop.size() > 2)
    {
        prop = prop.substr(2);
    }
    else if (prop.starts_with("_") && prop.size() > 1)
    {
        prop = prop.substr(1);
    }
    if (!prop.empty())
    {
        prop[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(prop[0])));
    }
    return prop;
}

/**
 * @brief Finds enclosing class declaration node and its class body node.
 * @param[in] leaf AST leaf at cursor position.
 * @param[in] sourceCode Source text.
 * @param[out] className Extracted class name.
 * @param[out] classBody Extracted class body node.
 * @return True if class declaration and body were located.
 */
bool FindEnclosingClassAndBody(TSNode leaf, std::string_view sourceCode, std::string& className, TSNode& classBody)
{
    TSNode classNode = leaf;
    while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
    {
        classNode = ts_node_parent(classNode);
    }
    if (ts_node_is_null(classNode))
    {
        return false;
    }
    classBody = parser::GetChildByField(classNode, parser::fields::Body);
    if (ts_node_is_null(classBody))
    {
        uint32_t cnt = ts_node_child_count(classNode);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(classNode, i);
            if (std::string_view(ts_node_type(ch)) == "class_body")
            {
                classBody = ch;
                break;
            }
        }
    }
    if (ts_node_is_null(classBody))
    {
        return false;
    }
    TSNode nameNode = parser::GetChildByField(classNode, parser::fields::Name);
    className = GetNodeText(nameNode, sourceCode);
    return true;
}

/**
 * @brief Collects candidate member fields for accessor generation.
 * @param[in] leaf AST node under cursor.
 * @param[in] classBody Class body AST node.
 * @param[in] request Code action request.
 * @param[in] className Name of enclosing class.
 * @return Vector of {fieldName, fieldType} pairs.
 */
std::vector<std::pair<std::string, std::string>> CollectFieldsForGetterSetter(TSNode leaf, TSNode classBody,
                                                                              const CodeActionRequest& request,
                                                                              const std::string& className)
{
    TSNode varDecl = leaf;
    while (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) != "variable_declaration" &&
           varDecl.id != classBody.id)
    {
        varDecl = ts_node_parent(varDecl);
    }

    std::vector<std::pair<std::string, std::string>> fields;
    if (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) == "variable_declaration")
    {
        TSNode typeNode = parser::GetChildByField(varDecl, parser::fields::VarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(varDecl, parser::fields::Type);
        }
        std::string fieldType = GetNodeText(typeNode, request.sourceCode);
        if (fieldType.empty())
        {
            fieldType = "int";
        }

        uint32_t dCnt = ts_node_child_count(varDecl);
        for (uint32_t i = 0; i < dCnt; ++i)
        {
            TSNode ch = ts_node_child(varDecl, i);
            if (std::string_view(ts_node_type(ch)) == "variable_declarator")
            {
                TSNode vNameNode = parser::GetChildByField(ch, parser::fields::Name);
                std::string fName = GetNodeText(vNameNode, request.sourceCode);
                if (!fName.empty())
                {
                    fields.push_back({fName, fieldType});
                }
            }
        }
    }
    else
    {
        request.symbolTable.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
            {
                for (const auto& sym : symList)
                {
                    if (sym.containerName == className &&
                        (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property))
                    {
                        fields.push_back({sym.name, sym.GetVariable().typeName});
                    }
                }
            });
    }
    return fields;
}

/**
 * @brief Finds the insertion point before the closing brace of a class body.
 * @param[in] classBody Class body node.
 * @return AST point preceding class closing brace.
 */
TSPoint FindClassClosingBracePoint(TSNode classBody)
{
    uint32_t cnt = ts_node_child_count(classBody);
    for (int i = static_cast<int>(cnt) - 1; i >= 0; --i)
    {
        TSNode ch = ts_node_child(classBody, static_cast<uint32_t>(i));
        if (std::string_view(ts_node_type(ch)) == "}")
        {
            return ts_node_start_point(ch);
        }
    }
    return TSPoint{0, 0};
}

struct GetterSetterContext
{
    const std::string& className;
    const std::pair<std::string, std::string>& field;
    TSPoint insertPt;
};

/**
 * @brief Creates a CodeAction inserting an accessor text edit into the class body.
 * @param[in] uri Document URI.
 * @param[in] title Action title.
 * @param[in] text Method code to insert.
 * @param[in] insertPt Class body insertion point.
 * @return Constructed CodeAction.
 */
lsp::CodeAction CreateAccessorAction(const std::string& uri, const std::string& title, const std::string& text,
                                     TSPoint insertPt)
{
    lsp::CodeAction action;
    action.title = title;
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
    edit.newText = "\n" + text;

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    return action;
}

/**
 * @brief Checks if a class field already has getter and setter declarations.
 * @param[in] table Symbol table.
 * @param[in] className Name of enclosing class.
 * @param[in] propName Clean property name.
 * @return Pair of booleans {hasGetter, hasSetter}.
 */
std::pair<bool, bool> CheckFieldAccessors(const analysis::SymbolTable& table, const std::string& className,
                                          const std::string& propName)
{
    std::string getterName = "get_" + propName;
    std::string setterName = "set_" + propName;
    bool hasGetter = false;
    bool hasSetter = false;
    table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (sym.containerName == className && sym.type == analysis::SymbolType::Function)
                {
                    if (sym.name == getterName)
                    {
                        hasGetter = true;
                    }
                    if (sym.name == setterName)
                    {
                        hasSetter = true;
                    }
                }
            }
        });
    return {hasGetter, hasSetter};
}

/**
 * @brief Emits Getter, Setter, or combined Getter/Setter actions for a class field.
 * @param[in] request Code action request.
 * @param[in] ctx Getter and setter context.
 * @param[out] actions Destination code actions vector.
 */
void EmitGetterSetterActionsForField(const CodeActionRequest& request, const GetterSetterContext& ctx,
                                     std::vector<lsp::CodeAction>& actions)
{
    const auto& [fName, fType] = ctx.field;
    std::string propName = CleanPropertyName(fName);
    if (propName.empty())
    {
        return;
    }

    auto [hasGetter, hasSetter] = CheckFieldAccessors(request.symbolTable, ctx.className, propName);
    if (hasGetter && hasSetter)
    {
        return;
    }

    std::string cleanType = fType.empty() ? "int" : fType;
    bool isPassByValue = analysis::IsPrimitiveTypeName(cleanType) || cleanType.ends_with("@");
    std::string setterParamType = isPassByValue ? cleanType : ("const " + cleanType + " &in");

    std::string getterCode =
        "    " + cleanType + " get_" + propName + "() const\n    {\n        return " + fName + ";\n    }\n";
    std::string setterCode =
        "    void set_" + propName + "(" + setterParamType + " value)\n    {\n        " + fName + " = value;\n    }\n";

    if (!hasGetter)
    {
        actions.push_back(CreateAccessorAction(request.uri, "Generate Getter", getterCode, ctx.insertPt));
    }
    if (!hasSetter)
    {
        actions.push_back(CreateAccessorAction(request.uri, "Generate Setter", setterCode, ctx.insertPt));
    }
    if (!hasGetter && !hasSetter)
    {
        actions.push_back(CreateAccessorAction(request.uri, "Generate Getter and Setter",
                                               getterCode + "\n" + setterCode, ctx.insertPt));
    }
}

} // namespace

void TryAddGetterSetterActions(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }
    TSPoint pt = {request.range.start.line, request.range.start.character};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return;
    }
    std::string className;
    TSNode classBody;
    if (!FindEnclosingClassAndBody(leaf, request.sourceCode, className, classBody))
    {
        return;
    }
    auto fields = CollectFieldsForGetterSetter(leaf, classBody, request, className);
    if (fields.empty())
    {
        return;
    }
    TSPoint insertPt = FindClassClosingBracePoint(classBody);
    for (const auto& field : fields)
    {
        EmitGetterSetterActionsForField(request, GetterSetterContext{className, field, insertPt}, actions);
    }
}

void TryAddAccessorPropertyKeywordFix(const CodeActionRequest& request, TSNode rootNode,
                                      std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-accessor-portability"))
        {
            continue;
        }

        const TSPoint point = {diag.range.start.line, diag.range.start.character};
        TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);
        while (!ts_node_is_null(node) && std::string_view(ts_node_type(node)) != "func_declaration")
        {
            node = ts_node_parent(node);
        }
        if (ts_node_is_null(node))
        {
            continue;
        }

        const TSNode parameters = parser::GetChildByField(node, parser::fields::Parameters);
        if (ts_node_is_null(parameters))
        {
            continue;
        }

        const TSPoint insertAt = ts_node_end_point(parameters);

        lsp::TextEdit edit;
        edit.range.start.line = insertAt.row;
        edit.range.start.character = insertAt.column;
        edit.range.end.line = insertAt.row;
        edit.range.end.character = insertAt.column;
        edit.newText = " property";

        lsp::CodeAction action;
        action.title = "Add the 'property' keyword";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;
        action.diagnostics = std::vector<lsp::Diagnostic>{diag};

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

} // namespace angel_lsp::features
