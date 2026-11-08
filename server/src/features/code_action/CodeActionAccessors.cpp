#include "analysis/index/AccessorFacts.h"
#include "analysis/types/TypeSanitization.h"
#include "features/code_action/CodeActionInternal.h"
#include "i18n/i18n.h"
#include "utils/AccessorNames.h"
#include "utils/Utils.h"

#include <cctype>
#include <cstdint>
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
 * @brief Creates a CodeActionEntry inserting an accessor text edit into the class body.
 * @param[in] title Action title.
 * @param[in] text Method code to insert.
 * @param[in] insertPt Class body insertion point coordinates.
 * @return Constructed CodeActionEntry.
 */
static analysis::code_action::CodeActionEntry CreateAccessorEntry(std::string title, const std::string& text,
                                                                  analysis::IndexedSourcePoint insertPt)
{
    analysis::code_action::CodeActionEntry entry;
    entry.title = std::move(title);
    entry.kind = analysis::code_action::CodeActionEntryKind::RefactorRewrite;
    entry.edits = std::vector<analysis::code_action::CodeActionTextEdit>{
        analysis::code_action::CodeActionTextEdit{
            analysis::SourceRange{insertPt.line, insertPt.character, insertPt.line, insertPt.character},
            "\n" + text,
        },
    };
    return entry;
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
    const std::string getterName = std::string(utils::k_getterPrefix) + propName;
    const std::string setterName = std::string(utils::k_setterPrefix) + propName;
    if (const auto ruleIdx = table.GetRuleIndex())
    {
        const auto& members = ruleIdx->Members(className);
        return {members.methodNames.contains(getterName), members.methodNames.contains(setterName)};
    }
    return {table.FindMemberSymbolPtr(className, getterName) != nullptr,
            table.FindMemberSymbolPtr(className, setterName) != nullptr};
}

/**
 * @brief Emits Getter, Setter, or combined Getter/Setter entries for a class field.
 * @param[in] query Incoming code action query context.
 * @param[in] ctx Getter and setter context.
 * @param[in,out] entries Destination code action entries vector.
 */
static void EmitGetterSetterEntriesForField(const analysis::code_action::CodeActionQuery& query,
                                            const GetterSetterContext& ctx,
                                            std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    const auto& [fName, fType] = ctx.field;
    std::string propName = CleanPropertyName(fName);
    if (propName.empty())
    {
        return;
    }

    auto [hasGetter, hasSetter] = CheckFieldAccessors(query.symbolTable, ctx.className, propName);
    if (hasGetter && hasSetter)
    {
        return;
    }

    std::string cleanType = fType.empty() ? "int" : fType;
    bool isPassByValue = analysis::IsPrimitiveTypeName(cleanType) || analysis::IsHandleTypeView(cleanType);
    std::string setterParamType = isPassByValue ? cleanType : ("const " + cleanType + " &in");

    std::string getterCode = "    " + cleanType + " " + std::string(utils::k_getterPrefix) + propName +
                             "() const\n    {\n        return " + fName + ";\n    }\n";
    std::string setterCode = "    void " + std::string(utils::k_setterPrefix) + propName + "(" + setterParamType +
                             " value)\n    {\n        " + fName + " = value;\n    }\n";

    if (!hasGetter)
    {
        entries.push_back(CreateAccessorEntry(
            i18n::FormatMessage(query.i18n, "action-generate-getter", "Generate Getter"), getterCode, ctx.insertPt));
    }
    if (!hasSetter)
    {
        entries.push_back(CreateAccessorEntry(
            i18n::FormatMessage(query.i18n, "action-generate-setter", "Generate Setter"), setterCode, ctx.insertPt));
    }
    if (!hasGetter && !hasSetter)
    {
        entries.push_back(CreateAccessorEntry(
            i18n::FormatMessage(query.i18n, "action-generate-getter-setter", "Generate Getter and Setter"),
            getterCode + "\n" + setterCode, ctx.insertPt));
    }
}

/**
 * @brief Generates refactoring entries to create getter and setter methods for class fields.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddGetterSetterActions(const analysis::code_action::CodeActionQuery& query,
                               std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }

    const auto site = analysis::FindAccessorSite(*query.nodeIndex, query.sourceCode, query.range.startLine,
                                                 query.range.startCharacter);
    if (!site)
    {
        return;
    }

    const std::string className(
        utils::SliceSource(query.sourceCode, site->className.startByte, site->className.endByte));
    std::string fieldType(utils::SliceSource(query.sourceCode, site->fieldType.startByte, site->fieldType.endByte));
    if (fieldType.empty())
    {
        fieldType = "int";
    }

    for (const auto& r : site->fieldNames)
    {
        std::pair<std::string, std::string> field{
            std::string(utils::SliceSource(query.sourceCode, r.startByte, r.endByte)), fieldType};
        EmitGetterSetterEntriesForField(query, GetterSetterContext{className, field, site->insertPoint}, entries);
    }
}

/**
 * @brief Generates quick-fix entries adding the property keyword to accessor methods.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddAccessorPropertyKeywordFix(const analysis::code_action::CodeActionQuery& query,
                                      std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        if (!MatchDiagnosticCode(diag, "as-hint-accessor-portability"))
        {
            continue;
        }

        const auto insertAt = analysis::FindPropertyKeywordInsertionPoint(*query.nodeIndex, diag.range.startLine,
                                                                          diag.range.startCharacter);
        if (!insertAt)
        {
            continue;
        }

        std::string title =
            i18n::FormatMessage(query.i18n, "action-add-property-keyword", "Add the 'property' keyword");

        entries.push_back(MakeQuickFixEntry(QuickFixEntryOptions{
            std::move(title),
            i,
            {analysis::code_action::CodeActionTextEdit{
                analysis::SourceRange{insertAt->line, insertAt->character, insertAt->line, insertAt->character},
                " property"}},
            true}));

        entries.push_back(MakeDisableDiagnosticEntry(DisableDiagnosticEntryOptions{
            i, "diagnostics.reportAccessorPortability", "as-hint-accessor-portability", query.i18n}));
    }
}

} // namespace angel_lsp::features
