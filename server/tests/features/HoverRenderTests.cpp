#include <doctest/doctest.h>
#include <iostream>
#include "features/hover/HoverInfo.h"
#include "i18n/LspStrings.h"
#include "analysis/Symbol.h"

using namespace angel_lsp::features;
using namespace i18n;

TEST_CASE("HoverInfo: function with full docs renders clangd-style")
{
    HoverInfo info;
    info.name = "Lerp";
    info.kind = analysis::SymbolKind::Function;
    info.rawSignature = "float Math::Lerp(float a, float b, float t)";
    info.localScope = "Math";
    
    info.briefText = "Interpolates between a and b.";
    info.detailsText = "Uses linear interpolation.";
    
    HoverParam pa; pa.name = "a"; pa.typeName = "float"; pa.docDescription = "start";
    HoverParam pb; pb.name = "b"; pb.typeName = "float"; pb.docDescription = "end";
    HoverParam pt; pt.name = "t"; pt.typeName = "float"; pt.docDescription = "alpha";
    
    info.localScope = "Engine::Math";
    
    info.parameters.emplace();
    info.parameters->push_back(pa);
    info.parameters->push_back(pb);
    info.parameters->push_back(pt);
    
    info.returnType = "float";
    info.returnDoc = "The interpolated value.";
    
    info.notes.push_back("Clamps t between 0 and 1.");
    
    auto sections = info.ToHoverSections(Locale::EN);
    std::string md = "";
    for (const auto& sec : sections) {
        if (sec.isCodeBlock) {
            md += "```" + sec.language + "\n" + sec.content + "\n```";
        } else {
            if (!md.empty()) md += "\n\n---\n\n";
            md += sec.content;
        }
    }
    std::cout << "GENERATED MD:\\n" << md << "\\nEND MD\\n";
    
    CHECK(md.find("```angelscript\n// In Engine::Math\nfloat Math::Lerp(float a, float b, float t)\n```") != std::string::npos);
    CHECK(md.find("Interpolates between a and b.") != std::string::npos);
    CHECK(md.find("Uses linear interpolation.") != std::string::npos);
    CHECK(md.find("### Parameters") != std::string::npos);
    CHECK(md.find("- `float a` \xE2\x80\x94 start") != std::string::npos);
    CHECK(md.find("### Returns") != std::string::npos);
    CHECK(md.find("`float` \xE2\x80\x94 The interpolated value.") != std::string::npos);
    CHECK(md.find("**Note:** Clamps t between 0 and 1.") != std::string::npos);
    CHECK(md.find("### ") != std::string::npos);
}

TEST_CASE("HoverInfo: void return is omitted from Returns section")
{
    HoverInfo info;
    info.name = "DoNothing";
    info.kind = analysis::SymbolKind::Function;
    info.rawSignature = "void DoNothing()";
    info.returnType = "void";
    
    auto sections = info.ToHoverSections(Locale::EN);
    std::string md = "";
    for (const auto& sec : sections) {
        if (sec.isCodeBlock) {
            md += "```" + sec.language + "\n" + sec.content + "\n```";
        } else {
            if (!md.empty()) md += "\n\n---\n\n";
            md += sec.content;
        }
    }
    CHECK(md.find("### Returns") == std::string::npos);
}

TEST_CASE("HoverInfo: overload count appended in italic")
{
    HoverInfo info;
    info.name = "Lerp";
    info.kind = analysis::SymbolKind::Function;
    info.rawSignature = "float Lerp()";
    info.overloadCount = 2;
    
    auto sections = info.ToHoverSections(Locale::EN);
    std::string md = "";
    for (const auto& sec : sections) {
        if (sec.isCodeBlock) {
            md += "```" + sec.language + "\n" + sec.content + "\n```";
        } else {
            if (!md.empty()) md += "\n\n---\n\n";
            md += sec.content;
        }
    }
    CHECK(md.find("*+2 overloads*") != std::string::npos);
}
