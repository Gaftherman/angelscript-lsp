#include "features/hover/HoverInfo.h"

namespace angel_lsp {
namespace features {

static const char* GetKindLabel(analysis::SymbolKind kind, const i18n::LspStrings& s) {
    switch (kind) {
        case analysis::SymbolKind::Variable: return s.kindVariable;
        case analysis::SymbolKind::Function: return s.kindFunction;
        case analysis::SymbolKind::Class: return s.kindClass;
        case analysis::SymbolKind::Namespace: return s.kindNamespace;
        case analysis::SymbolKind::Parameter: return s.kindParameter;
        case analysis::SymbolKind::Property: return s.kindProperty;
        case analysis::SymbolKind::Method: return s.kindMethod;
        case analysis::SymbolKind::Enum: return s.kindEnum;
        case analysis::SymbolKind::EnumMember: return s.kindEnumMember;
        case analysis::SymbolKind::Interface: return s.kindInterface;
        case analysis::SymbolKind::Mixin: return s.kindMixin;
        case analysis::SymbolKind::Constructor: return s.kindConstructor;
        case analysis::SymbolKind::Destructor: return s.kindDestructor;
        case analysis::SymbolKind::Typedef: return s.kindTypedef;
        default: return s.kindUnknown;
    }
}

std::string HoverInfo::ToMarkdown(i18n::Locale locale) const {
    const auto& s = i18n::GetStrings(locale);
    std::string md;

    // 1. CODE BLOCK con scope
    md += "```angelscript\n";
    if (!localScope.empty()) {
        md += "// In ";
        md += localScope;
        md += "\n";
    }
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

    if ((!briefText.empty() || !detailsText.empty()) && hasDocSections) {
        md += "\n\n---\n";
    } else if (hasDocSections) {
        md += "\n\n---\n";
    }

    // 4. DEPRECATED
    if (!deprecated.empty()) {
        md += "\n**";
        md += s.hoverDeprecated;
        md += "** ";
        md += deprecated;
    }

    // 5. TEMPLATE PARAMETERS
    if (templateParameters && !templateParameters->empty()) {
        md += "\n\n**";
        md += s.hoverSectionTemplateParams;
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
        md += s.hoverSectionParams;
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
        md += s.hoverSectionReturns;
        md += "**\n\n`";
        md += *returnType;
        md += "`";
        if (!returnDoc.empty()) {
            md += " — ";
            md += returnDoc;
        }
    }

    // 8. NOTES y WARNINGS
    for (const auto& note : notes) {
        md += "\n\n**";
        std::string sNote = s.hoverNote;
        if (sNote.ends_with(":")) sNote.pop_back();
        md += sNote;
        md += ":** ";
        md += note;
    }
    for (const auto& warn : warnings) {
        md += "\n\n**";
        std::string sWarn = s.hoverWarning;
        if (sWarn.ends_with(":")) sWarn.pop_back();
        md += sWarn;
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

    // Limpiar trailing whitespace (si los hay al final)
    while (!md.empty() && (md.back() == '\n' || md.back() == ' '))
        md.pop_back();

    return md;
}

} // namespace features
} // namespace angel_lsp
