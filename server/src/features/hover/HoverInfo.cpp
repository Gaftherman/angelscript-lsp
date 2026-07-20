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

    // 1. HEADING H3
    md += "### ";
    md += GetKindLabel(kind, s);
    md += " `";
    md += name;
    md += "`\n\n";

    // 2. RULER
    md += "---\n";

    // 3. CODE BLOCK con scope
    md += "```angelscript\n";
    if (!localScope.empty()) {
        md += "// In ";
        md += localScope;
        md += "\n";
    }
    md += rawSignature;
    if (!enumValue.empty()) md += " = " + enumValue;
    md += "\n```\n\n";

    // 4. BRIEF TEXT
    if (!briefText.empty()) {
        md += briefText;
        md += "\n";
    }

    // 5. DETAILS TEXT
    if (!detailsText.empty()) {
        if (!briefText.empty()) md += "\n";
        md += detailsText;
        md += "\n";
    }

    bool hasDocSections = (parameters && !parameters->empty()) ||
                          (templateParameters && !templateParameters->empty()) ||
                          (returnType && returnType != "void" && !returnType->empty()) ||
                          !notes.empty() || !warnings.empty() || !deprecated.empty();

    if ((!briefText.empty() || !detailsText.empty()) && hasDocSections) {
        md += "\n---\n";
    }

    // 6. DEPRECATED
    if (!deprecated.empty()) {
        md += "> **";
        md += s.hoverDeprecated;
        md += "** ";
        md += deprecated;
        md += "\n\n";
    }

    // 7. TEMPLATE PARAMETERS
    if (templateParameters && !templateParameters->empty()) {
        md += "### ";
        md += s.hoverSectionTemplateParams;
        md += "\n";
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
        md += "\n---\n";
    }

    // 8. PARAMETERS
    if (parameters && !parameters->empty()) {
        md += "### ";
        md += s.hoverSectionParams;
        md += "\n";
        for (const auto& p : *parameters) {
            md += "- `";
            md += p.name;
            md += "`";
            if (!p.typeName.empty()) {
                md += " `";
                md += p.typeName;
                md += "`";
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
        md += "\n---\n";
    }

    // 9. RETURNS
    if (returnType && *returnType != "void" && !returnType->empty()) {
        md += "### ";
        md += s.hoverSectionReturns;
        md += "\n`";
        md += *returnType;
        md += "`";
        if (!returnDoc.empty()) {
            md += " — ";
            md += returnDoc;
        }
        md += "\n\n---\n";
    }

    // 10. NOTES y WARNINGS
    for (const auto& note : notes) {
        md += "> **";
        md += s.hoverNote;
        md += "** ";
        md += note;
        md += "\n";
    }
    for (const auto& warn : warnings) {
        md += "> **";
        md += s.hoverWarning;
        md += "** ";
        md += warn;
        md += "\n";
    }

    // 11. OVERLOADS
    if (overloadCount > 0) {
        if (!notes.empty() || !warnings.empty()) md += "\n";
        md += "*+";
        md += std::to_string(overloadCount);
        md += " ";
        md += s.hoverOverloads;
        md += "*\n";
    }

    // Limpiar trailing whitespace
    while (!md.empty() && (md.back() == '\n' || md.back() == ' '))
        md.pop_back();

    return md;
}

} // namespace features
} // namespace angel_lsp
