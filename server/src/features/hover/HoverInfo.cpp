#include "features/hover/HoverInfo.h"
#include "i18n/LspStrings.h"

namespace angel_lsp {
namespace features {

std::vector<HoverInfo::HoverSection> HoverInfo::ToHoverSections(i18n::Locale locale) const {
    const auto& s = i18n::GetStrings(locale);
    std::vector<std::string> blocks;

    // Bloque 0: Deprecated
    if (!deprecated.empty()) {
        blocks.push_back("> **" + std::string(s.hoverDeprecated) + ":** " + deprecated);
    }

    // Bloque 1: Código + Ámbito
    std::string codeBlock = "```angelscript\n";
    if (!localScope.empty()) {
        if (kind == analysis::SymbolKind::Parameter) {
            codeBlock += "// " + localScope + "\n";
        } else {
            codeBlock += "// In " + localScope + "\n";
        }
    }
    codeBlock += rawSignature;
    if (!enumValue.empty()) codeBlock += " = " + enumValue;
    codeBlock += "\n```";
    blocks.push_back(codeBlock);

    // Bloque 2: Breve / Descripción
    if (!briefText.empty()) {
        std::string desc = briefText;
        if (!detailsText.empty()) desc += "\n\n" + detailsText;
        blocks.push_back(desc);
    }

    // Bloque 3: Parámetros / Retornos
    if (parameters && !parameters->empty()) {
        bool hasParamDocs = false;
        for (const auto& p : *parameters) {
            if (!p.docDescription.empty()) { hasParamDocs = true; break; }
        }
        if (hasParamDocs) {
            std::string pBlock = "### " + std::string(s.hoverParams) + "\n";
            for (const auto& p : *parameters) {
                std::string signature = p.typeName.empty() ? p.name : (p.typeName + " " + p.name);
                if (!p.defaultValue.empty()) signature += " = " + p.defaultValue;
                pBlock += "- `" + signature + "`";
                if (!p.docDescription.empty()) pBlock += " \xE2\x80\x94 " + p.docDescription;
                pBlock += "\n";
            }
            if (pBlock.back() == '\n') pBlock.pop_back();
            blocks.push_back(pBlock);
        }
    }

    if (templateParameters && !templateParameters->empty()) {
        bool hasTParamDocs = false;
        for (const auto& p : *templateParameters) {
            if (!p.docDescription.empty()) { hasTParamDocs = true; break; }
        }
        if (hasTParamDocs) {
            std::string tpSection = "### " + std::string(s.hoverTemplateParams) + "\n";
            for (const auto& p : *templateParameters) {
                tpSection += "- `" + p.name + "`";
                if (!p.docDescription.empty()) {
                    tpSection += " \xE2\x80\x94 " + p.docDescription;
                }
                tpSection += "\n";
            }
            if (tpSection.back() == '\n') tpSection.pop_back();
            blocks.push_back(tpSection);
        }
    }

    if (returnType && *returnType != "void" && !returnType->empty()) {
        if (!returnDoc.empty()) {
            std::string retSection = "### " + std::string(s.hoverReturns) + "\n`" + *returnType + "` \xE2\x80\x94 " + returnDoc;
            blocks.push_back(retSection);
        }
    }

    // Bloque 4: Notas / Advertencias
    for (const auto& note : notes) {
        blocks.push_back("> **" + std::string(s.hoverNote) + ":** " + note);
    }
    for (const auto& warn : warnings) {
        blocks.push_back("> **" + std::string(s.hoverWarning) + ":** " + warn);
    }

    // Bloque 5: Sobrecargas
    if (overloadCount > 0) {
        blocks.push_back("*+" + std::to_string(overloadCount) + " " + std::string(s.hoverOverloads) + "*");
    }

    // Unión de bloques intercalando '---' de forma segura
    std::string result;
    for (size_t i = 0; i < blocks.size(); ++i) {
        result += blocks[i];
        if (i + 1 < blocks.size()) {
            result += "\n\n---\n\n";
        }
    }

    std::vector<HoverSection> resultSections;
    if (!result.empty()) {
        HoverSection textSection;
        textSection.isCodeBlock = false;
        textSection.content = result;
        resultSections.push_back(textSection);
    }

    return resultSections;
}

} // namespace features
} // namespace angel_lsp
