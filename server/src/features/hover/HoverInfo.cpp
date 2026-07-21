#include "features/hover/HoverInfo.h"
#include "i18n/LspStrings.h"

namespace angel_lsp {
namespace features {

std::string HoverInfo::ToMarkdown(i18n::Locale locale) const {
    const auto& s = i18n::GetStrings(locale);
    std::vector<std::string> blocks;

    // BLOQUE 0: Depreciado (Formato de Cita sin Emojis)
    if (!deprecated.empty()) {
        blocks.push_back("> **" + std::string(s.hoverDeprecated) + ":** " + deprecated);
    }

    // BLOQUE 1: Código + Ámbito
    std::string codeBlock = "```angelscript\n";
    if (!localScope.empty()) {
        if (kind == analysis::SymbolKind::Parameter) {
            // El contexto de parámetro se muestra como subtítulo formateado fuera o en comentario interno
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

    // BLOQUE 2: Descripción (Brief + Details)
    if (!briefText.empty()) {
        std::string desc = briefText;
        if (!detailsText.empty() && detailsText != briefText) {
            desc += "\n\n" + detailsText;
        }
        blocks.push_back(desc);
    }

    // BLOQUE 3: Parámetros de Plantilla
    if (templateParameters && !templateParameters->empty()) {
        std::string tpBlock = "### " + std::string(s.hoverTemplateParams) + "\n";
        for (const auto& p : *templateParameters) {
            tpBlock += "- `" + p.typeName + " " + p.name + "`";
            if (!p.docDescription.empty()) tpBlock += " \xE2\x80\x94 " + p.docDescription; // Guion largo
            tpBlock += "\n";
        }
        if (tpBlock.back() == '\n') tpBlock.pop_back();
        blocks.push_back(tpBlock);
    }

    // BLOQUE 4: Parámetros de Función
    if (parameters && !parameters->empty()) {
        bool hasParamDocs = false;
        for (const auto& p : *parameters) {
            if (!p.docDescription.empty()) { hasParamDocs = true; break; }
        }
        
        // Renderizar la sección si existen explicaciones en Doxygen o es un método
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

    // BLOQUE 5: Valor de Retorno
    if (returnType && *returnType != "void" && !returnType->empty()) {
        std::string retBlock = "### " + std::string(s.hoverReturns) + "\n`" + *returnType + "`";
        if (!returnDoc.empty()) {
            retBlock += " \xE2\x80\x94 " + returnDoc;
        }
        blocks.push_back(retBlock);
    }

    // BLOQUE 6: Notas (Bloque de Cita sin Emojis)
    for (const auto& note : notes) {
        blocks.push_back("> **" + std::string(s.hoverNote) + ":** " + note);
    }

    // BLOQUE 7: Advertencias (Bloque de Cita sin Emojis)
    for (const auto& warn : warnings) {
        blocks.push_back("> **" + std::string(s.hoverWarning) + ":** " + warn);
    }

    // BLOQUE 8: Contador de Sobrecargas
    if (overloadCount > 0) {
        blocks.push_back("*+" + std::to_string(overloadCount) + " " + std::string(s.hoverOverloads) + "*");
    }

    // ENSAMBLADO FINAL: Une únicamente bloques no vacíos intercalando '---'
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