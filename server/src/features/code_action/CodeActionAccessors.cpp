#include "analysis/index/AccessorFacts.h"
#include "analysis/types/TypeSanitization.h"
#include "features/code_action/CodeActionInternal.h"
#include "i18n/i18n.h"
#include "utils/Utils.h"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Cleans property name by stripping m_ or _ prefix and capitalizing the first character.
 * @param[in] fieldName Member variable name.
 * @return Cleaned property name suitable for accessor generation.
 */
static std::string CleanPropertyName(std::string_view fieldName)
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
 * @brief Creates a CodeAction inserting an accessor text edit into the class body.
 * @param[in] uri Document URI.
 * @param[in] title Action title.
 * @param[in] text Method code to insert.
 * @param[in] insertPt Class body insertion point coordinates.
 * @return Constructed CodeAction.
 */
static lsp::CodeAction CreateAccessorAction(const std::string& uri, const std::string& title, const std::string& text,
                                            analysis::IndexedSourcePoint insertPt)
{
    lsp::CodeAction action;
    action.title = title;
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorRewrite);
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.line, insertPt.character}, {insertPt.line, insertPt.character}};
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
static std::pair<bool, bool> CheckFieldAccessors(const analysis::SymbolTable& table, const std::string& className,
                                                 const std::string& propName)
{
    const std::string getterName = "get_" + propName;
    const std::string setterName = "set_" + propName;
    if (const auto ruleIdx = table.GetRuleIndex())
    {
        const auto& members = ruleIdx->Members(className);
        return {members.methodNames.contains(getterName), members.methodNames.contains(setterName)};
    }
    return {table.FindMemberSymbolPtr(className, getterName) != nullptr,
            table.FindMemberSymbolPtr(className, setterName) != nullptr};
}

/**
 * @brief Emits Getter, Setter, or combined Getter/Setter actions for a class field.
 * @param[in] request Code action request.
 * @param[in] ctx Getter and setter context.
 * @param[out] actions Destination code actions vector.
 */
static void EmitGetterSetterActionsForField(const CodeActionRequest& request, const GetterSetterContext& ctx,
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
    bool isPassByValue = analysis::IsPrimitiveTypeName(cleanType) || analysis::IsHandleTypeView(cleanType);
    std::string setterParamType = isPassByValue ? cleanType : ("const " + cleanType + " &in");

    std::string getterCode =
        "    " + cleanType + " get_" + propName + "() const\n    {\n        return " + fName + ";\n    }\n";
    std::string setterCode =
        "    void set_" + propName + "(" + setterParamType + " value)\n    {\n        " + fName + " = value;\n    }\n";

    if (!hasGetter)
    {
        actions.push_back(CreateAccessorAction(
            request.uri, i18n::FormatMessage(request.i18n, "action-generate-getter", "Generate Getter"), getterCode,
            ctx.insertPt));
    }
    if (!hasSetter)
    {
        actions.push_back(CreateAccessorAction(
            request.uri, i18n::FormatMessage(request.i18n, "action-generate-setter", "Generate Setter"), setterCode,
            ctx.insertPt));
    }
    if (!hasGetter && !hasSetter)
    {
        actions.push_back(CreateAccessorAction(
            request.uri,
            i18n::FormatMessage(request.i18n, "action-generate-getter-setter", "Generate Getter and Setter"),
            getterCode + "\n" + setterCode, ctx.insertPt));
    }
}

void TryAddGetterSetterActions(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return;
    }

    const auto site = analysis::FindAccessorSite(*request.nodeIndex, request.sourceCode, request.range.start.line,
                                                 request.range.start.character);
    if (!site)
    {
        return;
    }

    const std::string className(
        utils::SliceSource(request.sourceCode, site->className.startByte, site->className.endByte));
    std::string fieldType(utils::SliceSource(request.sourceCode, site->fieldType.startByte, site->fieldType.endByte));
    if (fieldType.empty())
    {
        fieldType = "int";
    }

    for (const auto& r : site->fieldNames)
    {
        std::pair<std::string, std::string> field{
            std::string(utils::SliceSource(request.sourceCode, r.startByte, r.endByte)), fieldType};
        EmitGetterSetterActionsForField(request, GetterSetterContext{className, field, site->insertPoint}, actions);
    }
}

void TryAddAccessorPropertyKeywordFix(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-hint-accessor-portability"))
        {
            continue;
        }

        const auto insertAt = analysis::FindPropertyKeywordInsertionPoint(*request.nodeIndex, diag.range.start.line,
                                                                          diag.range.start.character);
        if (!insertAt)
        {
            continue;
        }

        lsp::TextEdit edit;
        edit.range.start.line = insertAt->line;
        edit.range.start.character = insertAt->character;
        edit.range.end.line = insertAt->line;
        edit.range.end.character = insertAt->character;
        edit.newText = " property";

        lsp::CodeAction action;
        action.title = i18n::FormatMessage(request.i18n, "action-add-property-keyword", "Add the 'property' keyword");
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;
        action.diagnostics = std::vector<lsp::Diagnostic>{diag};

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));

        actions.push_back(MakeDisableDiagnosticAction(
            {diag, "diagnostics.reportAccessorPortability", "as-hint-accessor-portability", request.i18n}));
    }
}

} // namespace angel_lsp::features
