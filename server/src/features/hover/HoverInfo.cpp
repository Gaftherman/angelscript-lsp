#include "features/hover/HoverInfo.h"

namespace angel_lsp {
namespace features {


std::string HoverInfo::ToMarkdown(i18n::Locale locale) const {
    const auto& s = i18n::GetStrings(locale);
    std::string md;

    // 1. CODE BLOCK with signature
    md += "```angelscript\n";
    md += rawSignature;
    if (!enumValue.empty()) md += " = " + enumValue;
    md += "\n```\n";

    // 2. BRIEF TEXT
    if (!briefText.empty()) {
        md += "\n";
        md += briefText;
    }

    // 3. DETAILS TEXT
    if (!detailsText.empty()) {
        md += "\n\n";
        md += detailsText;
    }

    bool hasDocSections = (parameters && !parameters->empty()) ||
                          (templateParameters && !templateParameters->empty()) ||
                          (returnType && returnType != "void" && !returnType->empty()) ||
                          !notes.empty() || !warnings.empty() || !deprecated.empty();

    if (hasDocSections) {
        md += "\n\n---\n";
    }

    // 4. DEPRECATED
    if (!deprecated.empty()) {
        md += "\n**";
        md += s.hoverDeprecated;
        md += ":** ";
        md += deprecated;
    }

    // 5. TEMPLATE PARAMETERS
    if (templateParameters && !templateParameters->empty()) {
        md += "\n\n**";
        md += s.hoverTemplateParams;
        md += "**\n\n";
        for (const auto& p : *templateParameters) {
            md += "- `";
            md += p.name;
            md += "`";
            if (!p.docDescription.empty()) {
                md += " — ";
                md += p.docDescription;
            }
            md += "\n";
        }
    }

    // 6. PARAMETERS
    if (parameters && !parameters->empty()) {
        md += "\n\n**";
        md += s.hoverParams;
        md += "**\n\n";
        for (const auto& p : *parameters) {
            md += "- `";
            md += p.name;
            md += "`";
            if (!p.typeName.empty()) {
                md += " [`";
                md += p.typeName;
                md += "`]";
            }
            if (!p.defaultValue.empty()) {
                md += " = `";
                md += p.defaultValue;
                md += "`";
            }
            if (!p.docDescription.empty()) {
                md += " — ";
                md += p.docDescription;
            }
            md += "\n";
        }
    }

    // 7. RETURNS
    if (returnType && *returnType != "void" && !returnType->empty()) {
        md += "\n\n**";
        md += s.hoverReturns;
        md += "**\n\n`";
        md += *returnType;
        md += "`";
        if (!returnDoc.empty()) {
            md += " — ";
            md += returnDoc;
        }
    }

    // 8. NOTES and WARNINGS
    for (const auto& note : notes) {
        md += "\n\n**";
        md += s.hoverNote;
        md += ":** ";
        md += note;
    }
    for (const auto& warn : warnings) {
        md += "\n\n**";
        md += s.hoverWarning;
        md += ":** ";
        md += warn;
    }

    // 9. OVERLOADS
    if (overloadCount > 0) {
        md += "\n\n*+";
        md += std::to_string(overloadCount);
        md += " ";
        md += s.hoverOverloads;
        md += "*";
    }

    // Clear trailing whitespace (if any)
    while (!md.empty() && (md.back() == '\n' || md.back() == ' '))
        md.pop_back();

    return md;
}

} // namespace features
} // namespace angel_lsp
