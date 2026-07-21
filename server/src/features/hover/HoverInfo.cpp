#include "features/hover/HoverInfo.h"
#include "i18n/LspStrings.h"

namespace angel_lsp {
namespace features {

std::vector<HoverInfo::HoverSection> HoverInfo::ToHoverSections(i18n::Locale locale) const {
    const auto& s = i18n::GetStrings(locale);
    std::vector<std::string> sections;

    // 1. CODE BLOCK with signature
    std::vector<HoverSection> resultSections;
    HoverSection codeBlock;
    codeBlock.isCodeBlock = true;
    codeBlock.language = "angelscript";
    if (!localScope.empty()) {
        codeBlock.content += "// In " + localScope + "\n";
    }
    codeBlock.content += rawSignature;
    if (!enumValue.empty()) codeBlock.content += " = " + enumValue;
    resultSections.push_back(codeBlock);

    // 2. BRIEF & DETAILS TEXT
    std::string docText;
    if (!briefText.empty()) {
        docText += briefText;
    }
    if (!detailsText.empty()) {
        if (!docText.empty()) docText += "\n\n";
        docText += detailsText;
    }
    if (!docText.empty()) {
        sections.push_back(docText);
    }

    // 4. DEPRECATED
    if (!deprecated.empty()) {
        sections.push_back("**" + std::string(s.hoverDeprecated) + ":** " + deprecated);
    }

    // 5. TEMPLATE PARAMETERS
    if (templateParameters && !templateParameters->empty()) {
        std::string tpSection = "### " + std::string(s.hoverTemplateParams) + "\n\n";
        for (const auto& p : *templateParameters) {
            tpSection += "- `" + p.name + "`";
            if (!p.docDescription.empty()) {
                tpSection += " \xE2\x80\x94 " + p.docDescription;
            }
            tpSection += "\n";
        }
        if (tpSection.back() == '\n') tpSection.pop_back();
        sections.push_back(tpSection);
    }

    // 6. PARAMETERS
    if (parameters && !parameters->empty()) {
        std::string paramSection = "### " + std::string(s.hoverParams) + "\n\n";
        for (const auto& p : *parameters) {
            std::string signature = p.typeName.empty() ? p.name : (p.typeName + " " + p.name);
            if (!p.defaultValue.empty()) signature += " = " + p.defaultValue;
            paramSection += "- `" + signature + "`";
            if (!p.docDescription.empty()) {
                paramSection += " \xE2\x80\x94 " + p.docDescription;
            }
            paramSection += "\n";
        }
        if (paramSection.back() == '\n') paramSection.pop_back();
        sections.push_back(paramSection);
    }

    // 7. RETURNS
    if (returnType && *returnType != "void" && !returnType->empty()) {
        std::string retSection = "### " + std::string(s.hoverReturns) + "\n\n`" + *returnType + "`";
        if (!returnDoc.empty()) {
            retSection += " \xE2\x80\x94 " + returnDoc;
        }
        sections.push_back(retSection);
    }

    // 8. NOTES and WARNINGS
    if (!notes.empty() || !warnings.empty()) {
        std::string alerts;
        for (const auto& note : notes) {
            if (!alerts.empty()) alerts += "\n\n";
            alerts += "**" + std::string(s.hoverNote) + ":** " + note;
        }
        for (const auto& warn : warnings) {
            if (!alerts.empty()) alerts += "\n\n";
            alerts += "**" + std::string(s.hoverWarning) + ":** " + warn;
        }
        sections.push_back(alerts);
    }

    // 9. OVERLOADS
    if (overloadCount > 0) {
        sections.push_back("*+" + std::to_string(overloadCount) + " " + s.hoverOverloads + "*");
    }

    // JOIN TEXT SECTIONS WITH ---
    std::string md;
    for (size_t i = 0; i < sections.size(); ++i) {
        md += sections[i];
        if (i < sections.size() - 1) {
            md += "\n\n---\n\n";
        } 
    }

    // Clear trailing whitespace
    while (!md.empty() && (md.back() == '\n' || md.back() == ' '))
        md.pop_back();

    if (!md.empty()) {
        HoverSection textSection;
        textSection.isCodeBlock = false;
        textSection.content = md;
        resultSections.push_back(textSection);
    }

    return resultSections;
}

} // namespace features
} // namespace angel_lsp
