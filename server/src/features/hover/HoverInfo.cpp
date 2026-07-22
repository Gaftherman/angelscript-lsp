#include "features/hover/HoverInfo.h"
#include "i18n/LspStrings.h"

namespace angel_lsp
{
    namespace features
    {

        void HoverInfo::PopulateFromDoxygen(const utils::ParsedDoxygenDoc &doc, const std::string &targetParam)
        {
            briefText = doc.brief;
            detailsText = doc.details;
            returnDoc = doc.returns;
            deprecated = doc.deprecated;

            if (!doc.note.empty())
            {
                notes.push_back(doc.note);
            }
            if (!doc.warning.empty())
            {
                warnings.push_back(doc.warning);
            }

            if (!targetParam.empty())
            {
                for (const auto &p : doc.parameters)
                {
                    if (p.name == targetParam)
                    {
                        briefText = p.description;
                        detailsText.clear();
                        returnDoc.clear();
                        break;
                    }
                }
            }
            else if (parameters.has_value())
            {
                for (const auto &p : doc.parameters)
                {
                    for (auto &hp : *parameters)
                    {
                        if (hp.name == p.name)
                        {
                            hp.docDescription = p.description;
                            break;
                        }
                    }
                }
            }
        }

std::string HoverInfo::ToMarkdown(i18n::Locale locale) const {
    const auto& s = i18n::GetStrings(locale);
    std::vector<std::string> blocks;

    // BLOCK 1: Code + Scope
    std::string codeBlock = "```angelscript\n";
    if (!localScope.empty()) {
        if (kind == analysis::SymbolKind::Parameter) {
            // Parameter context displayed as formatted subtitle or internal comment
            codeBlock += "// " + localScope + "\n";
        } else {
            codeBlock += "// " + std::string(s.hoverIn) + " " + localScope + "\n";
        }
    } else if (isBuiltin && !builtinLabel.empty()) {
        codeBlock += "// " + std::string(builtinLabel) + "\n";
    }
    codeBlock += rawSignature;
    if (!enumValue.empty() && kind != analysis::SymbolKind::EnumMember) {
        codeBlock += " = " + enumValue;
    }
    codeBlock += "\n```";
    blocks.push_back(codeBlock);

    // BLOCK 2: Description (Brief + Details)
    if (!briefText.empty()) {
        std::string desc = briefText;
        if (!detailsText.empty() && detailsText != briefText) {
            desc += "\n\n" + detailsText;
        }
        blocks.push_back(desc);
    }

    // BLOCK 3: Template Parameters
    if (templateParameters && !templateParameters->empty()) {
        std::string tpBlock = "### " + std::string(s.hoverTemplateParams) + "\n";
        for (const auto& p : *templateParameters) {
            tpBlock += "- `" + p.typeName + " " + p.name + "`";
            if (!p.docDescription.empty()) tpBlock += " \xE2\x80\x94 " + p.docDescription; // Em-dash
            tpBlock += "\n";
        }
        if (tpBlock.back() == '\n') tpBlock.pop_back();
        blocks.push_back(tpBlock);
    }

    // BLOCK 4: Function Parameters
    if (parameters && !parameters->empty()) {
        bool hasParamDocs = false;
        for (const auto& p : *parameters) {
            if (!p.docDescription.empty()) { hasParamDocs = true; break; }
        }
        
        // Render section if Doxygen docs exist or symbol is a function/method
        if (hasParamDocs || kind == analysis::SymbolKind::Function || kind == analysis::SymbolKind::Method || kind == analysis::SymbolKind::Constructor || kind == analysis::SymbolKind::Destructor || kind == analysis::SymbolKind::Funcdef) {
            std::string pBlock = "### " + std::string(s.hoverParams) + "\n";
            for (const auto& p : *parameters) {
                std::string signature = p.typeName.empty() ? p.name : (p.typeName + " " + p.name);
                pBlock += "- `" + signature + "`";
                if (!p.defaultValue.empty()) pBlock += " = `" + p.defaultValue + "`";
                if (!p.docDescription.empty()) pBlock += " \xE2\x80\x94 " + p.docDescription;
                pBlock += "\n";
            }
            if (pBlock.back() == '\n') pBlock.pop_back();
            blocks.push_back(pBlock);
        }
    }

    // BLOCK 5: Return Value
    if (returnType && *returnType != "void" && !returnType->empty()) {
        std::string retBlock = "### " + std::string(s.hoverReturns) + "\n`" + *returnType + "`";
        if (!returnDoc.empty()) {
            retBlock += " \xE2\x80\x94 " + returnDoc;
        }
        blocks.push_back(retBlock);
    }

    // BLOCK 6: Notes
    for (const auto& note : notes) {
        blocks.push_back("**" + std::string(s.hoverNote) + ":** " + note);
    }

    // BLOCK 7: Warnings
    for (const auto& warn : warnings) {
        blocks.push_back("**" + std::string(s.hoverWarning) + ":** " + warn);
    }

    // BLOCK 8: Deprecated
    if (!deprecated.empty()) {
        blocks.push_back("**" + std::string(s.hoverDeprecated) + ":** " + deprecated);
    }

    // BLOCK 9: Overloads Counter
    if (overloadCount > 0) {
        blocks.push_back("*+" + std::to_string(overloadCount) + " " + std::string(s.hoverOverloads) + "*");
    }

    // FINAL ASSEMBLY: Join non-empty blocks using '---' dividers
    std::string result;
    for (size_t i = 0; i < blocks.size(); ++i) {
        result += blocks[i];
        if (i + 1 < blocks.size()) {
            result += "\n\n---\n\n";
        }
    }
    return result;
}

std::vector<HoverInfo::HoverSection> HoverInfo::ToHoverSections(i18n::Locale locale) const {
    std::vector<HoverSection> resultSections;
    std::string md = ToMarkdown(locale);
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